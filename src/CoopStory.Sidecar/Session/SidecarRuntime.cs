using CoopStory.Protocol;
using CoopStory.Sidecar.Configuration;
using CoopStory.Sidecar.Diagnostics;
using CoopStory.Sidecar.Ipc;
using CoopStory.Sidecar.Networking;
using CoopStory.Sidecar.Persistence;
using System.Net;
using System.Text;
using System.Text.Json;
using System.Threading.Channels;

namespace CoopStory.Sidecar.Session;

// Says why the game should wait before sending normal multiplayer messages.
internal enum BridgeOutboundDeferReason
{
    // Neither of this policy's waiting conditions applies.
    None,
    // The peer has connected but has not yet agreed to the same movement replication mode.
    MotionModeNegotiation,
    // The guest still needs to finish its local reconnect reset and request the host replay.
    GuestReconnectResync
}

// Keeps the two wait rules in one place: agree on movement mode, then finish guest reconnect repair.
internal static class BridgeOutboundSessionPolicy
{
    // Choose whether normal outgoing gameplay must wait for movement-mode agreement or guest reconnect cleanup.
    // This only returns the reason to wait and does not queue, send, or repair a message.
    public static BridgeOutboundDeferReason Evaluate(
        SessionRole localRole,
        bool networkConnected,
        bool motionModeNegotiated,
        bool guestReconnectResyncPending,
        MessageType messageType)
    {
        _ = messageType;
        if (localRole == SessionRole.Guest &&
            guestReconnectResyncPending)
        {
            return BridgeOutboundDeferReason.GuestReconnectResync;
        }

        return networkConnected && !motionModeNegotiated
            ? BridgeOutboundDeferReason.MotionModeNegotiation
            : BridgeOutboundDeferReason.None;
    }
}

// Main C# multiplayer manager.
// It links RDR2 to the LAN, saves host state for reconnecting, and checks messages before passing them on.
public sealed class SidecarRuntime : IAsyncDisposable
{
    // RDR2 can stop reading the pipe while loading/paused.
    // A blocked important-message write can reset the pipe after two seconds, while replaceable state writes get longer.
    private const int BridgeDeliveryWatchdogIntervalMs = 250;
    private const int BridgeCriticalDeliveryStallAbortMs = 2_000;
    private const int BridgeSnapshotDeliveryStallAbortMs = 60_000;
    private const int PeerMotionModeNegotiationTimeoutMs = 3_000;
    private const int RemoteBridgeMappingFreshnessMs = 5_000;

    private SidecarConfig _config;
    private readonly SidecarConfig _menuBootstrapConfig;
    private readonly SessionCredentials _initialCredentials;
    private readonly JsonLineLogger _logger;
    private readonly BridgePipeServer _bridge;
    // Keeps only the newest fast update before the game pipe.
    // Important make, remove, and control messages stay in order.
    private readonly NetworkBridgeDeliveryPump _networkBridgePump;
    private readonly IPAddress? _hostListenAddress;
    private readonly bool _allowLoopbackGuestWorldView;
    private readonly TaskCompletionSource<Exception> _fatalSessionFailure =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    // This queue hands selected sessions to one background worker so session shutdown finishes before the next run starts.
    private readonly Channel<NetworkSessionRun> _networkSessions =
        Channel.CreateUnbounded<NetworkSessionRun>(
            new UnboundedChannelOptions
            {
                SingleReader = true,
                SingleWriter = false,
                AllowSynchronousContinuations = false
            });
    private readonly object _networkSessionSync = new();
    private NetworkSessionRun? _activeNetworkSession;
    private ILanSession? _network;
    // Player updates help smooth movement.
    // The world graph saves host NPCs so a reconnecting guest can rebuild them.
    private readonly ReplicatedEntityRegistry _entities;
    private readonly AuthoritativeWorldGraphRegistry _worldGraph;
    private readonly AuthoritativeInteractionRegistry _interactions = new();
    // These rules decide whether an action may start, continue, or end with host approval.
    // A guest's animation or requested action is not itself permission to apply a physical effect to another player.
    private readonly AuthoritativePlayerActionStateMachine _playerActions = new();
    private readonly PlayerIdentityPublisher _identityPublisher;
    // These caches remember the latest host-controlled state needed when a peer or the local game reconnects.
    // They hold protocol data rather than RDR2 entity handles.
    private readonly AuthoritativeMissionStateCache _missionStateCache = new();
    private readonly AuthoritativeMissionCinematicStateCache
        _missionCinematicStateCache = new();
    private readonly AuthoritativeAnimSceneDefinitionCache
        _animSceneDefinitionCache = new();
    // Journals remember grants and mission completions across runs so retries can refer to the same event IDs.
    private readonly CapabilityJournal _capabilityJournal = new();
    private readonly CapabilityJournalStore _capabilityJournalStore = new();
    private readonly MissionProgressionJournal _missionProgressionJournal = new();
    private readonly MissionProgressionJournalStore _missionProgressionJournalStore = new();
    private readonly PlayerInventoryRegistry _pickupClaims = new();
    private readonly InventoryStateStore _pickupClaimStateStore = new();
    private readonly SemaphoreSlim _capabilityJournalPersistenceGate = new(1, 1);
    private readonly SemaphoreSlim _missionProgressionJournalPersistenceGate = new(1, 1);
    private readonly GuestReconnectResyncGate
        _guestReconnectResyncGate = new();
    // A gate lets one related operation run at a time, similar to taking turns through a protected section.
    // The control gate preserves control-message order and the bridge gate separates old and new game sessions.
    private readonly PeerControlSendGate _peerControlSendGate = new();
    private readonly BridgeSessionGenerationGate
        _bridgeSessionGenerationGate = new();
    private readonly RemoteBridgeMappingGate _remoteBridgeMapping = new();
    // Track where messages were observed, forwarded, replaced by newer samples, or dropped.
    // These transport counters cannot prove that the game visibly applied a message.
    private readonly MessageFlowDiagnostics _messageFlowDiagnostics = new();
    private readonly HashSet<(MessageFlowDirection Direction, MessageType Type)>
        _openTransportGaps = [];
    private int _bridgeSequence;
    private int _motionReplicationConfigRevision;
    private int _bridgePipeConnected;
    private long _bridgeReadyGeneration;
    private long _bridgeToNetworkObserved;
    private long _bridgeToNetworkDelivered;
    private long _bridgeToNetworkNoPeer;
    private long _bridgeToNetworkDropped;
    private long _networkToBridgeObserved;
    private long _networkToBridgeDelivered;
    private long _networkToBridgeUnavailable;
    private int _networkBridgeQueueRejectionEvents;
    private int _peerAnimationStateObserved;
    private readonly object _peerMotionModeSync = new();
    private ILanSession? _negotiatedMotionNetwork;
    private ControlPeerToken _negotiatedMotionPeer;
    private int _peerMotionModeNegotiated;
    private int _peerMotionModeAnnouncementRevision;
    private long _peerMotionModeNegotiationStartedAt;
    private long _peerStreamingStartedAt;
    private long _lastPeerHeartbeatAtMs;
    private long _lastPeerClockDeltaEstimateMs = long.MinValue;
    private int _heartbeatDiagnosticsParseFailureShown;
    private uint _lastLoggedMissionTxEpoch;
    private uint _lastLoggedMissionTxRevision;
    private int _hasLoggedMissionTxVersion;
    private int _preNegotiationInboundDropShown;
    private int _preNegotiationOutboundDropShown;
    private int _motionModeNegotiationFailureShown;
    private int _localAnimationStateUnexpectedShown;
    private string _activeInviteCode = string.Empty;
    private string _sessionFingerprint = string.Empty;
    private int _sessionActivationStarted;
    private int _authenticationRejectionShown;
    private bool _disposed;

    private sealed record NetworkSessionRun(
        ILanSession Network,
        CancellationTokenSource Stop,
        TaskCompletionSource<bool> Stopped);

    private enum MotionNegotiationMarkDisposition
    {
        // The current peer's movement-mode agreement was recorded for the first time.
        Accepted,
        // Agreement for this same peer was already recorded.
        Duplicate,
        // The announcement belongs to a peer or session that is no longer current.
        StalePeer
    }

    internal event Action<ProtocolEnvelope>? BridgeEnvelopeDelivered;
    internal event Func<bool>? SoloTestToggleRequested;
    internal event Func<bool>? GhostRecordToggleRequested;
    internal event Func<bool>? GhostReplayToggleRequested;
    internal event Func<bool>? GuestWorldViewToggleRequested;

    // Connect the local game pipe, outgoing delivery queue, state caches, and event handlers used by this session.
    // With the in-game menu enabled, hosting or joining starts later when the player chooses it.
    public SidecarRuntime(
        SidecarConfig config,
        SessionCredentials credentials,
        JsonLineLogger logger,
        IPAddress? hostListenAddress = null,
        bool allowLoopbackGuestWorldView = false)
    {
        _config = (config ?? throw new ArgumentNullException(nameof(config))).Validate();
        _menuBootstrapConfig = _config;
        _initialCredentials =
            credentials ?? throw new ArgumentNullException(nameof(credentials));
        if (!_config.InGameMenuEnabled)
        {
            _sessionFingerprint = credentials.SessionId.ToString("N")[..12];
        }
        _logger = logger ?? throw new ArgumentNullException(nameof(logger));
        _hostListenAddress = hostListenAddress;
        _allowLoopbackGuestWorldView = allowLoopbackGuestWorldView;
        // The native ASI connects to this per-user local pipe.
        // The delivery pump is the only normal path from network callbacks to the game thread.
        _bridge = new BridgePipeServer(_config.PipeName);
        _networkBridgePump = new NetworkBridgeDeliveryPump(
            DeliverNetworkEnvelopeToBridgeAsync);
        if (!_config.InGameMenuEnabled)
        {
            StartNetworkSession(
                CreateNetwork(_config, _initialCredentials));
        }
        _entities = new ReplicatedEntityRegistry(
            _config.Replication.InterpolationDelayMs);
        _worldGraph = new AuthoritativeWorldGraphRegistry();
        _identityPublisher = new PlayerIdentityPublisher(_config.Nickname);

        _bridge.MessageReceived += OnBridgeEnvelopeAsync;
        _bridge.ConnectionFaulted += OnBridgeFaultedAsync;
        _bridge.ConnectionOpened += OnBridgeOpenedAsync;
        _bridge.ConnectionClosed += OnBridgeClosedAsync;
    }

    // Run the game connection, network sessions, identity updates, and diagnostics together until shutdown or failure.
    // The cleanup path stops the other background jobs so a failed session cannot leave them sending old data.
    public async Task RunAsync(CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        await RestoreCapabilityJournalAsync(cancellationToken).ConfigureAwait(false);
        await RestoreMissionProgressionJournalAsync(cancellationToken).ConfigureAwait(false);
        await RestorePickupClaimsAsync(cancellationToken).ConfigureAwait(false);
        if (!_config.InGameMenuEnabled)
        {
            await EnsureGuestProfileAsync(cancellationToken).ConfigureAwait(false);
        }
        var initialRole = _config.InGameMenuEnabled
            ? "PendingInGameMenu"
            : _config.Role.ToString();
        await _logger.InfoAsync(
            "runtime.started",
            $"Sidecar role {initialRole}; bridge pipe '{_bridge.PipeName}'.",
            new Dictionary<string, object?>
            {
                ["role"] = initialRole,
                ["inGameMenuEnabled"] = _config.InGameMenuEnabled,
                ["pipeName"] = _bridge.PipeName,
                ["tcpPort"] = _config.TcpPort,
                ["udpPort"] = _config.UdpPort,
                ["hostSaveFile"] = _config.HostSave?.FileName,
                ["hostSaveSha256Prefix"] =
                    _config.HostSave?.Sha256[..12],
                ["hostSaveAutomaticLoad"] =
                    _config.HostSave?.AutomaticGameLoad ?? false,
                ["motionReplicationMode"] =
                    _config.MotionReplicationMode.ToString(),
                ["sessionFingerprint"] = string.IsNullOrEmpty(
                    _sessionFingerprint)
                    ? null
                    : _sessionFingerprint
            },
            cancellationToken: cancellationToken).ConfigureAwait(false);
        await _logger.InfoAsync(
            "bridge.waiting",
            "Waiting for the RDR2 game bridge.",
            cancellationToken: cancellationToken).ConfigureAwait(false);

        using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        // Run the pipe, network, logging, player name, message queue, and stall checker at the same time.
        // If one stops, the session stops safely.
        var pipeTask = _bridge.RunAsync(linked.Token);
        var networkTask = RunNetworkAfterSelectionAsync(linked.Token);
        var diagnosticsTask = DiagnosticsLoopAsync(linked.Token);
        var identityTask = IdentityLoopAsync(linked.Token);
        var networkBridgeTask = _networkBridgePump.RunAsync(linked.Token);
        var bridgeWatchdogTask = BridgeDeliveryWatchdogAsync(linked.Token);
        var fatalSessionFailureTask = _fatalSessionFailure.Task;
        try
        {
            await Task.WhenAny(
                    pipeTask,
                    networkTask,
                    diagnosticsTask,
                    identityTask,
                    networkBridgeTask,
                    bridgeWatchdogTask,
                    fatalSessionFailureTask)
                .ConfigureAwait(false);
            await linked.CancelAsync().ConfigureAwait(false);
            _networkBridgePump.StopAccepting();
            await _bridge.StopAsync().ConfigureAwait(false);
            try
            {
                await Task.WhenAll(
                        pipeTask,
                        networkTask,
                        diagnosticsTask,
                        identityTask,
                        networkBridgeTask,
                        bridgeWatchdogTask)
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (linked.IsCancellationRequested)
            {
            }

            if (fatalSessionFailureTask.IsCompletedSuccessfully)
            {
                throw await fatalSessionFailureTask.ConfigureAwait(false);
            }
        }
        finally
        {
            await linked.CancelAsync().ConfigureAwait(false);
            _networkBridgePump.StopAccepting();
            await _bridge.StopAsync().ConfigureAwait(false);
            await WriteFinalStreamingDiagnosticsAsync().ConfigureAwait(false);
            await _logger.InfoAsync(
                "runtime.stopped",
                $"Sidecar role {_config.Role} stopped.",
                cancellationToken: CancellationToken.None).ConfigureAwait(false);
        }
    }

    // Stop accepting new delivery work and release this runtime's connections and synchronization helpers.
    // The disposed flag makes repeated cleanup calls harmless.
    public async ValueTask DisposeAsync()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _networkBridgePump.StopAccepting();
        NetworkSessionRun? session;
        lock (_networkSessionSync)
        {
            session = _activeNetworkSession;
        }
        if (session is not null)
        {
            await session.Stop.CancelAsync().ConfigureAwait(false);
            await session.Stopped.Task.ConfigureAwait(false);
        }
        await _bridge.DisposeAsync().ConfigureAwait(false);
        _bridgeSessionGenerationGate.Dispose();
        _capabilityJournalPersistenceGate.Dispose();
        _missionProgressionJournalPersistenceGate.Dispose();
    }

    // Load the host's saved list of campaign unlock grants so they can be resent after reconnecting.
    // The journal remembers grants and acknowledgements rather than recreating them as new rewards.
    private async Task RestoreCapabilityJournalAsync(CancellationToken cancellationToken)
    {
        // The host owns the source-of-truth log.
        // Guests receive a replay from it and apply those permissions to their own local game state.
        if (_config.Role != SessionRole.Host || _config.InGameMenuEnabled)
        {
            return;
        }

        var path = _config.ExpandedCapabilityJournalPath;
        try
        {
            var restored = await _capabilityJournalStore.LoadAsync(path, cancellationToken)
                .ConfigureAwait(false);
            _capabilityJournal.Restore(restored.CaptureState());
            await _logger.InfoAsync(
                "campaign.capability-journal-restored",
                "Restored shared campaign capability permissions for reconnect replay.",
                new Dictionary<string, object?>
                {
                    ["path"] = path,
                    ["eventCount"] = _capabilityJournal.CaptureState().Count
                }, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is IOException or JsonException or ArgumentException)
        {
            // Do not let a malformed optional co-op journal prevent Story Mode from starting.
            // The journal is kept intact for diagnosis.
            await _logger.WarningAsync(
                "campaign.capability-journal-unavailable",
                "Could not restore the campaign capability journal; starting an empty in-memory journal.",
                new Dictionary<string, object?>
                {
                    ["path"] = path,
                    ["error"] = exception.Message
                }, cancellationToken: cancellationToken).ConfigureAwait(false);
        }
    }

    // Save the current unlock journal while preventing two saves from writing it at the same time.
    // The finally block releases the save lock even if writing fails.
    private async Task PersistCapabilityJournalAsync(CancellationToken cancellationToken)
    {
        if (_config.Role != SessionRole.Host || _config.InGameMenuEnabled)
        {
            return;
        }

        await _capabilityJournalPersistenceGate.WaitAsync(cancellationToken)
            .ConfigureAwait(false);
        try
        {
            await _capabilityJournalStore.SaveAsync(
                    _config.ExpandedCapabilityJournalPath,
                    _capabilityJournal,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            _capabilityJournalPersistenceGate.Release();
        }
    }

    // Load saved mission-completion records and their guest acknowledgements for the selected host save.
    // Pending completions can then be retried without pretending that a network send proves the guest saved them.
    private async Task RestoreMissionProgressionJournalAsync(
        CancellationToken cancellationToken)
    {
        if (_config.Role != SessionRole.Host) return;
        if (string.IsNullOrEmpty(_sessionFingerprint)) return;
        var path = _config.ExpandedMissionProgressionJournalPath(
            _sessionFingerprint);
        try
        {
            var restored = await _missionProgressionJournalStore.LoadAsync(
                path, cancellationToken).ConfigureAwait(false);
            _missionProgressionJournal.Restore(restored.CaptureState());
            await _logger.InfoAsync(
                "campaign.progression-journal-restored",
                "Restored mission completion transactions for reconnect replay.",
                new Dictionary<string, object?>
                {
                    ["path"] = path,
                    ["pendingCount"] = _missionProgressionJournal.CapturePending().Count
                }, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception exception) when (
            exception is IOException or JsonException or ArgumentException)
        {
            await _logger.WarningAsync(
                "campaign.progression-journal-unavailable",
                "Could not restore the mission progression journal; progression retries start empty.",
                new Dictionary<string, object?>
                {
                    ["path"] = path,
                    ["error"] = exception.Message
                }, cancellationToken: cancellationToken).ConfigureAwait(false);
        }
    }

    // Save the mission-completion journal under its own write lock so concurrent requests cannot overlap.
    private async Task PersistMissionProgressionJournalAsync(
        CancellationToken cancellationToken)
    {
        if (_config.Role != SessionRole.Host ||
            string.IsNullOrEmpty(_sessionFingerprint)) return;
        await _missionProgressionJournalPersistenceGate.WaitAsync(cancellationToken)
            .ConfigureAwait(false);
        try
        {
            await _missionProgressionJournalStore.SaveAsync(
                _config.ExpandedMissionProgressionJournalPath(
                    _sessionFingerprint),
                _missionProgressionJournal,
                cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            _missionProgressionJournalPersistenceGate.Release();
        }
    }

    // Load remembered pickup claims used to recognize items that have already been collected.
    private async Task RestorePickupClaimsAsync(CancellationToken cancellationToken)
    {
        if (_config.Role != SessionRole.Host || _config.InGameMenuEnabled) return;
        try { _pickupClaims.RestoreReconnectState(await _pickupClaimStateStore.LoadAsync(_config.ExpandedPickupClaimStatePath, cancellationToken).ConfigureAwait(false)); }
        catch (FileNotFoundException) { }
        catch (Exception exception) when (exception is IOException or JsonException or ArgumentException)
        {
            await _logger.WarningAsync("pickup.claim-state-unavailable", "Could not restore pickup claim cursors; starting empty.", new Dictionary<string, object?> { ["error"] = exception.Message }, cancellationToken: cancellationToken).ConfigureAwait(false);
        }
    }

    // Write the current pickup-claim snapshot to storage so it is available on a later run.
    private Task PersistPickupClaimsAsync(CancellationToken cancellationToken) =>
        _config.Role == SessionRole.Host && !_config.InGameMenuEnabled
            ? _pickupClaimStateStore.SaveAsync(_config.ExpandedPickupClaimStatePath, _pickupClaims.CaptureReconnectState(), cancellationToken)
            : Task.CompletedTask;

    // Run selected host or guest sessions one at a time and clean up each session when it ends.
    // The menu can then start another session without needing to restart the whole Sidecar process.
    private async Task RunNetworkAfterSelectionAsync(
        CancellationToken cancellationToken)
    {
        await foreach (var session in _networkSessions.Reader.ReadAllAsync(
                           cancellationToken).ConfigureAwait(false))
        {
            using var linked = CancellationTokenSource.CreateLinkedTokenSource(
                cancellationToken,
                session.Stop.Token);
            try
            {
                if (_menuBootstrapConfig.InGameMenuEnabled)
                {
                    await EnsureGuestProfileAsync(linked.Token)
                        .ConfigureAwait(false);
                }
                await session.Network.RunAsync(linked.Token)
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (linked.IsCancellationRequested)
            {
            }
            finally
            {
                await session.Network.DisposeAsync().ConfigureAwait(false);
                var ownsSession = false;
                lock (_networkSessionSync)
                {
                    if (ReferenceEquals(_activeNetworkSession, session))
                    {
                        ownsSession = true;
                        _activeNetworkSession = null;
                        _network = null;
                    }
                }
                if (ownsSession)
                {
                    await ResetInGameSessionStateAsync()
                        .ConfigureAwait(false);
                }
                session.Stopped.TrySetResult(true);
                session.Stop.Dispose();
            }

            if (!_menuBootstrapConfig.InGameMenuEnabled)
            {
                return;
            }
            if (!cancellationToken.IsCancellationRequested)
            {
                await SendSessionStatusAsync(
                    new SessionMenuStatusPayload(
                        SessionMenuStatusKind.Waiting,
                        "Session stopped. Select HOST or JOIN again."),
                    cancellationToken).ConfigureAwait(false);
                await _logger.InfoAsync(
                    "session.returned-to-menu",
                    "The network session stopped; the sidecar remains available for another HOST/JOIN cycle.",
                    cancellationToken: cancellationToken).ConfigureAwait(false);
            }
        }
    }

    // Register a newly selected network session and queue it for the network worker to run.
    // Reject a second active session so callbacks from two sessions cannot share the same game state.
    private void StartNetworkSession(ILanSession network)
    {
        ArgumentNullException.ThrowIfNull(network);
        _messageFlowDiagnostics.Reset();
        _remoteBridgeMapping.Clear();
        var session = new NetworkSessionRun(
            network,
            new CancellationTokenSource(),
            new TaskCompletionSource<bool>(
                TaskCreationOptions.RunContinuationsAsynchronously));
        AttachNetwork(network);
        lock (_networkSessionSync)
        {
            if (_activeNetworkSession is not null)
            {
                throw new InvalidOperationException(
                    "A network session is already active.");
            }
            _activeNetworkSession = session;
            _network = network;
        }
        if (!_networkSessions.Writer.TryWrite(session))
        {
            lock (_networkSessionSync)
            {
                _activeNetworkSession = null;
                _network = null;
            }
            session.Stop.Dispose();
            throw new InvalidOperationException(
                "Could not queue the selected network session.");
        }
    }

    // Return to the menu's starting state and clear connection-specific caches, counters, and queued deliveries.
    // Changing the bridge connection token makes work captured for the previous session no longer valid.
    private async ValueTask ResetInGameSessionStateAsync()
    {
        await using var authorityBoundary =
            await _bridgeSessionGenerationGate.EnterBoundaryAsync(
                    CancellationToken.None)
                .ConfigureAwait(false);
        Volatile.Write(ref _bridgeReadyGeneration, 0);
        _remoteBridgeMapping.Clear();
        await using var deliveryBarrier = await _networkBridgePump
            .EnterDeliveryBarrierAsync(CancellationToken.None)
            .ConfigureAwait(false);
        if (_bridge.TryCaptureConnection(out var bridgeConnection))
        {
            _ = await _bridge.RotateConnectionTokenAsync(
                    bridgeConnection,
                    CancellationToken.None)
                .ConfigureAwait(false);
        }

        _config = _menuBootstrapConfig;
        _activeInviteCode = string.Empty;
        _sessionFingerprint = string.Empty;
        _entities.Clear();
        _worldGraph.Clear();
        _ = await _peerControlSendGate.RunAsync(
                token =>
                {
                    _interactions.Clear(emitFreeStates: false);
                    _playerActions.Clear();
                    return ValueTask.FromResult(true);
                },
                CancellationToken.None)
            .ConfigureAwait(false);
        _networkBridgePump.ClearPending();
        _missionStateCache.Clear();
        _missionCinematicStateCache.Clear();
        _animSceneDefinitionCache.Clear();
        _guestReconnectResyncGate.Clear();
        Interlocked.Exchange(ref _sessionActivationStarted, 0);
        Interlocked.Exchange(ref _authenticationRejectionShown, 0);
        Interlocked.Exchange(ref _peerAnimationStateObserved, 0);
        ResetMotionModeNegotiation();
        Interlocked.Exchange(ref _preNegotiationInboundDropShown, 0);
        Interlocked.Exchange(ref _preNegotiationOutboundDropShown, 0);
        Interlocked.Exchange(ref _motionModeNegotiationFailureShown, 0);
        Interlocked.Exchange(ref _localAnimationStateUnexpectedShown, 0);
        Interlocked.Exchange(ref _peerMotionModeNegotiationStartedAt, 0);
        Interlocked.Exchange(ref _peerStreamingStartedAt, 0);
        Interlocked.Exchange(ref _lastPeerHeartbeatAtMs, 0);
        Interlocked.Exchange(
            ref _lastPeerClockDeltaEstimateMs,
            long.MinValue);
        Interlocked.Exchange(
            ref _heartbeatDiagnosticsParseFailureShown,
            0);
        _lastLoggedMissionTxEpoch = 0;
        _lastLoggedMissionTxRevision = 0;
        Volatile.Write(ref _hasLoggedMissionTxVersion, 0);
    }

    // Optionally tell the peer we are leaving, request the active network session to stop, and wait for its cleanup.
    // If there is no active session, still restore the reusable menu state.
    private async ValueTask StopActiveSessionAsync(
        bool notifyPeer,
        CancellationToken cancellationToken)
    {
        NetworkSessionRun? session;
        lock (_networkSessionSync)
        {
            session = _activeNetworkSession;
        }
        if (session is null)
        {
            await ResetInGameSessionStateAsync().ConfigureAwait(false);
            await SendSessionStatusAsync(
                new SessionMenuStatusPayload(
                    SessionMenuStatusKind.Waiting,
                    "No active session. Select HOST or JOIN."),
                cancellationToken).ConfigureAwait(false);
            return;
        }

        if (notifyPeer && session.Network.IsConnected)
        {
            _ = await SendPeerControlAsync(
                    session.Network,
                    MessageType.Goodbye,
                    Encoding.UTF8.GetBytes("coop session stopped from the in-game menu"),
                    NetworkClock.Tick,
                    controlSendGateHeld: false,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        await session.Stop.CancelAsync().ConfigureAwait(false);
        await session.Stopped.Task
            .WaitAsync(TimeSpan.FromSeconds(5), cancellationToken)
            .ConfigureAwait(false);
    }

    // Choose the LAN implementation for our role: the host listens for a guest and the guest connects to a host.
    private ILanSession CreateNetwork(
        SidecarConfig config,
        SessionCredentials credentials) =>
        config.Role switch
        {
            SessionRole.Host => new LanSessionHost(
                config,
                credentials,
                _logger,
                _hostListenAddress),
            SessionRole.Guest => new LanSessionGuest(
                config,
                credentials,
                _logger),
            _ => throw new ConfigurationException(
                $"Unsupported role {config.Role}.")
        };

    // Subscribe to this network object's incoming messages, authentication failures, and connection changes.
    // Passing that source object into callbacks lets later checks reject events from a replaced session.
    private void AttachNetwork(ILanSession network)
    {
        network.EnvelopeReceived += OnNetworkEnvelopeAsync;
        network.AuthenticationRejected += OnNetworkAuthenticationRejectedAsync;
        network.ConnectionChanged += (connected, cancellationToken) =>
            OnNetworkConnectionChangedAsync(
                network,
                connected,
                cancellationToken);
    }

    // Handle the game's host, join, stop, and test-mode menu requests.
    // For host or join, build the selected configuration, start its network session, and tell the local bridge its role.
    private async ValueTask ActivateSessionFromMenuAsync(
        SessionMenuRequestPayload request,
        CancellationToken cancellationToken)
    {
        if (request.Action == SessionMenuAction.StopSession)
        {
            await _logger.InfoAsync(
                "runtime.stop-requested",
                "The in-game menu requested a reusable session stop.",
                cancellationToken: cancellationToken).ConfigureAwait(false);
            await StopActiveSessionAsync(
                    notifyPeer: true,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }
        if (request.Action == SessionMenuAction.ToggleSoloTest)
        {
            await ToggleSoloTestFromMenuAsync(cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        if (request.Action == SessionMenuAction.ToggleGuestWorldView)
        {
            await ToggleGuestWorldViewFromMenuAsync(cancellationToken)
                .ConfigureAwait(false);
            return;
        }
        if (request.Action == SessionMenuAction.ToggleGhostRecord)
        {
            await ToggleGhostModeFromMenuAsync(
                    record: true,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }
        if (request.Action == SessionMenuAction.ToggleGhostReplay)
        {
            await ToggleGhostModeFromMenuAsync(
                    record: false,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        if (!_menuBootstrapConfig.InGameMenuEnabled)
        {
            await SendSessionStatusAsync(
                new SessionMenuStatusPayload(
                    SessionMenuStatusKind.Error,
                    "The in-game session menu is not enabled in this configuration."),
                cancellationToken).ConfigureAwait(false);
            return;
        }
        // Reserve session activation once so repeated menu clicks cannot start two hosts or two join attempts.
        if (Interlocked.CompareExchange(
                ref _sessionActivationStarted,
                1,
                0) != 0)
        {
            await SendSessionStatusAsync(
                new SessionMenuStatusPayload(
                    SessionMenuStatusKind.Error,
                    "A session is already running. Use STOP in F8 to change roles."),
                cancellationToken).ConfigureAwait(false);
            return;
        }

        try
        {
            var host = request.Action == SessionMenuAction.Host;
            await SendSessionStatusAsync(
                new SessionMenuStatusPayload(
                    host
                        ? SessionMenuStatusKind.StartingHost
                        : SessionMenuStatusKind.StartingGuest,
                    host
                        ? "Creating a private LAN session..."
                        : "Checking the code and connecting to the host..."),
                cancellationToken).ConfigureAwait(false);

            var activation = host
                ? InGameSessionCoordinator.CreateHost(_config)
                : InGameSessionCoordinator.CreateGuest(
                    _config,
                    request.InviteCode);
            _config = activation.Config;
            _activeInviteCode = activation.InviteCode;
            _sessionFingerprint =
                activation.Credentials.SessionId.ToString("N")[..12];
            await RestoreCapabilityJournalAsync(cancellationToken)
                .ConfigureAwait(false);
            await RestoreMissionProgressionJournalAsync(cancellationToken)
                .ConfigureAwait(false);
            var network = CreateNetwork(
                activation.Config,
                activation.Credentials);
            Interlocked.Exchange(ref _authenticationRejectionShown, 0);
            try
            {
                StartNetworkSession(network);
            }
            catch
            {
                await network.DisposeAsync().ConfigureAwait(false);
                throw;
            }

            await using (await _bridgeSessionGenerationGate
                             .EnterBoundaryAsync(cancellationToken)
                             .ConfigureAwait(false))
            {
                if (!ReferenceEquals(_network, network))
                {
                    throw new InvalidOperationException(
                        "The selected network session ended before the game bridge could acknowledge it.");
                }

                await AcknowledgeAndReplayBridgeStateUnderBoundaryAsync(
                        cancellationToken)
                    .ConfigureAwait(false);
                await SendSessionStatusAsync(
                    new SessionMenuStatusPayload(
                        host
                            ? SessionMenuStatusKind.ReadyHost
                            : SessionMenuStatusKind.ReadyGuest,
                        host
                            ? $"HOST {_config.HostAddress}: code ready; " +
                              (_config.HostSave is null
                                  ? "no save selected; load Story Mode manually."
                                  : $"save {_config.HostSave.FileName}; load this slot from the RDR2 menu.")
                            : "Code accepted; handshake in progress. Wait for REMOTE STREAMING.",
                        activation.InviteCode),
                    cancellationToken).ConfigureAwait(false);
                await _logger.InfoAsync(
                    "session.in-game-selected",
                    host
                        ? "Host session selected from the in-game overlay."
                        : "Guest session selected from the in-game overlay.",
                    new Dictionary<string, object?>
                    {
                        ["role"] = _config.Role.ToString(),
                        ["hostAddress"] = _config.HostAddress,
                        ["tcpPort"] = _config.TcpPort,
                        ["udpPort"] = _config.UdpPort,
                        ["hostSaveFile"] =
                            host ? _config.HostSave?.FileName : null,
                        ["hostSaveSha256Prefix"] =
                            host ? _config.HostSave?.Sha256[..12] : null,
                        ["sessionFingerprint"] =
                            _sessionFingerprint
                    },
                    cancellationToken).ConfigureAwait(false);
            }
        }
        catch (Exception exception) when (
            exception is ConfigurationException or
                FormatException or ProtocolException or
                InvalidOperationException)
        {
            await ResetInGameSessionStateAsync().ConfigureAwait(false);
            Interlocked.Exchange(ref _sessionActivationStarted, 0);
            await SendSessionStatusAsync(
                new SessionMenuStatusPayload(
                    SessionMenuStatusKind.Error,
                    exception.Message),
                cancellationToken).ConfigureAwait(false);
            await _logger.WarningAsync(
                "session.in-game-selection-rejected",
                exception.Message,
                cancellationToken: cancellationToken).ConfigureAwait(false);
        }
    }

    // Ask the optional solo-test handler to turn the local movement mirror on or off and report the result in-game.
    // This method requests the toggle rather than performing the movement replay itself.
    private async ValueTask ToggleSoloTestFromMenuAsync(
        CancellationToken cancellationToken)
    {
        var handler = SoloTestToggleRequested;
        if (handler is null)
        {
            const string unavailable =
                "The solo test is not active. Start it with the SOLO TEST button in the launcher.";
            await SendSessionStatusAsync(
                new SessionMenuStatusPayload(
                    SessionMenuStatusKind.Error,
                    unavailable),
                cancellationToken).ConfigureAwait(false);
            await _logger.WarningAsync(
                "local-test.f9-unavailable",
                unavailable,
                cancellationToken: cancellationToken).ConfigureAwait(false);
            return;
        }

        bool enabled;
        try
        {
            enabled = handler.Invoke();
        }
        catch (Exception exception)
        {
            await SendSessionStatusAsync(
                new SessionMenuStatusPayload(
                    SessionMenuStatusKind.Error,
                    "The solo test could not be toggled. Export diagnostics."),
                cancellationToken).ConfigureAwait(false);
            await _logger.ErrorAsync(
                "local-test.f9-toggle-failed",
                "The in-game F9 solo-test toggle failed.",
                exception,
                cancellationToken: cancellationToken).ConfigureAwait(false);
            return;
        }

        var message = enabled
            ? "LIVE MIRROR ENABLED: the co-op replica now repeats the local player beside you."
            : "LIVE MIRROR STOPPED: the replica will disappear after the stream expires.";
        await SendSessionStatusAsync(
            new SessionMenuStatusPayload(
                SessionMenuStatusKind.ReadyHost,
                message,
                _activeInviteCode),
            cancellationToken).ConfigureAwait(false);
        await _logger.InfoAsync(
            enabled
                ? "local-test.f9-enabled"
                : "local-test.f9-disabled",
            message,
            new Dictionary<string, object?>
            {
                ["enabled"] = enabled,
                ["source"] = "bridge-f9"
            },
            cancellationToken).ConfigureAwait(false);
    }

    // Ask the solo-test handler to switch between the original host NPCs and the test guest replicas.
    // Report an unavailable feature or a failed toggle through the game's session status display.
    private async ValueTask ToggleGuestWorldViewFromMenuAsync(
        CancellationToken cancellationToken)
    {
        var handler = GuestWorldViewToggleRequested;
        if (handler is null)
        {
            const string unavailable =
                "Guest World View is available only from SOLO TEST in the launcher.";
            await SendSessionStatusAsync(
                new SessionMenuStatusPayload(
                    SessionMenuStatusKind.Error,
                    unavailable),
                cancellationToken).ConfigureAwait(false);
            await _logger.WarningAsync(
                "local-test.guest-world-view-unavailable",
                unavailable,
                cancellationToken: cancellationToken).ConfigureAwait(false);
            return;
        }

        bool enabled;
        try
        {
            enabled = handler.Invoke();
        }
        catch (Exception exception)
        {
            await SendSessionStatusAsync(
                new SessionMenuStatusPayload(
                    SessionMenuStatusKind.Error,
                    "Guest World View could not be toggled. Export diagnostics."),
                cancellationToken).ConfigureAwait(false);
            await _logger.ErrorAsync(
                "local-test.guest-world-view-toggle-failed",
                "The in-game F9 Guest World View toggle failed.",
                exception,
                cancellationToken: cancellationToken).ConfigureAwait(false);
            return;
        }

        var message = enabled
            ? "CO-OP WORLD VIEW: host NPCs are replaced by the offset guest replicas."
            : "HOST WORLD VIEW: guest NPC replicas were removed and local NPCs restored.";
        await SendSessionStatusAsync(
            new SessionMenuStatusPayload(
                SessionMenuStatusKind.ReadyHost,
                message,
                _activeInviteCode),
            cancellationToken).ConfigureAwait(false);
        await _logger.InfoAsync(
            enabled
                ? "local-test.guest-world-view-enabled"
                : "local-test.guest-world-view-disabled",
            message,
            new Dictionary<string, object?>
            {
                ["enabled"] = enabled,
                ["source"] = "bridge-f9"
            },
            cancellationToken).ConfigureAwait(false);
    }

    // Choose the optional record or replay handler and report whether ghost recording or playback is now active.
    // The handler owns the actual recorded route and playback behavior.
    private async ValueTask ToggleGhostModeFromMenuAsync(
        bool record,
        CancellationToken cancellationToken)
    {
        var handler = record
            ? GhostRecordToggleRequested
            : GhostReplayToggleRequested;
        var feature = record ? "Ghost Record" : "Ghost Replay";
        var eventPrefix = record ? "ghost-record" : "ghost-replay";
        if (handler is null)
        {
            var unavailable =
                $"{feature} is not active. Start SOLO TEST in the launcher.";
            await SendSessionStatusAsync(
                new SessionMenuStatusPayload(
                    SessionMenuStatusKind.Error,
                    unavailable),
                cancellationToken).ConfigureAwait(false);
            await _logger.WarningAsync(
                $"{eventPrefix}.f9-unavailable",
                unavailable,
                cancellationToken: cancellationToken).ConfigureAwait(false);
            return;
        }

        bool active;
        try
        {
            active = handler.Invoke();
        }
        catch (Exception exception) when (
            exception is InvalidOperationException or
                InvalidDataException or IOException or UnauthorizedAccessException)
        {
            await SendSessionStatusAsync(
                new SessionMenuStatusPayload(
                    SessionMenuStatusKind.Error,
                    exception.Message),
                cancellationToken).ConfigureAwait(false);
            await _logger.ErrorAsync(
                $"{eventPrefix}.f9-toggle-failed",
                $"The in-game F9 {feature} toggle failed.",
                exception,
                cancellationToken: cancellationToken).ConfigureAwait(false);
            return;
        }

        var message = record
            ? active
                ? "GHOST RECORD: nagrywanie trasy rozpoczete."
                : "GHOST RECORD: route saved; you can start Ghost Replay."
            : active
                ? "GHOST REPLAY: the puppet replays the latest route."
                : "GHOST REPLAY: playback stopped.";
        await SendSessionStatusAsync(
            new SessionMenuStatusPayload(
                SessionMenuStatusKind.ReadyHost,
                message,
                _activeInviteCode),
            cancellationToken).ConfigureAwait(false);
        await _logger.InfoAsync(
            active
                ? $"{eventPrefix}.f9-enabled"
                : $"{eventPrefix}.f9-disabled",
            message,
            new Dictionary<string, object?>
            {
                ["active"] = active,
                ["source"] = "bridge-f9"
            },
            cancellationToken).ConfigureAwait(false);
    }

    // Tell this exact game-pipe connection whether it is host or guest and which movement replication mode to use.
    // Only mark the bridge ready after both configuration messages were written and its connection is still current.
    private async ValueTask SendRoleAcknowledgementAsync(
        BridgePipeConnectionToken bridgeConnection,
        CancellationToken cancellationToken)
    {
        var acknowledgement = new ProtocolEnvelope(
            MessageType.HelloAck,
            unchecked((uint)Interlocked.Increment(ref _bridgeSequence)),
            NetworkClock.Tick,
            BridgeRolePayloadCodec.Encode(_config.Role));
        if (!await _bridge.SendAsync(
                    bridgeConnection,
                    acknowledgement,
                    cancellationToken)
                .ConfigureAwait(false))
        {
            throw new IOException(
                "Bridge disconnected before role negotiation completed.");
        }

        var motionConfig = new MotionReplicationConfigPayload(
            AnimationReplicationPayloadCodec.MotionReplicationConfigSchemaVersion,
            ToWireMotionMode(_config.MotionReplicationMode),
            // AnimGraph Replica is an independent experimental engine.
            // The bridge may fail closed if its versioned reader is unavailable; it must not silently blend the task/navmesh motor into this mode.
            _config.AnimSceneStoryVmProbeEnabled
                ? MotionReplicationConfigFlags.EnableAnimSceneStoryVmProbe
                : MotionReplicationConfigFlags.None,
            NextMotionReplicationConfigRevision());
        var motionEnvelope = new ProtocolEnvelope(
            MessageType.MotionReplicationConfig,
            unchecked((uint)Interlocked.Increment(ref _bridgeSequence)),
            NetworkClock.Tick,
            AnimationReplicationPayloadCodec.EncodeMotionReplicationConfig(
                motionConfig));
        if (!await _bridge.SendAsync(
                    bridgeConnection,
                    motionEnvelope,
                    cancellationToken)
                .ConfigureAwait(false))
        {
            throw new IOException(
                "Bridge disconnected before motion replication configuration completed.");
        }

        if (!_bridge.IsConnectionCurrent(bridgeConnection))
        {
            throw new IOException(
                "Bridge connection changed before role negotiation completed.");
        }
        Volatile.Write(
            ref _bridgeReadyGeneration,
            bridgeConnection.Generation);
        await _logger.InfoAsync(
            "bridge.connected",
            $"Game bridge acknowledged with role {_config.Role} and " +
            $"motion mode {_config.MotionReplicationMode}.",
            new Dictionary<string, object?>
            {
                ["role"] = _config.Role.ToString(),
                ["motionReplicationMode"] =
                    _config.MotionReplicationMode.ToString(),
                ["motionConfigRevision"] = motionConfig.Revision,
                ["taskNavmeshFallbackAllowed"] = false,
                ["animSceneStoryVmProbeEnabled"] =
                    _config.AnimSceneStoryVmProbeEnabled
            },
            cancellationToken: cancellationToken).ConfigureAwait(false);
    }

    // Pause normal queued delivery while replacing the bridge's session token and sending its role configuration.
    // For a guest, replay cached host state only when the current peer is negotiated and reconnect cleanup permits it.
    private async ValueTask AcknowledgeAndReplayBridgeStateUnderBoundaryAsync(
        CancellationToken cancellationToken)
    {
        // A role boundary must drain one already-started pump frame, discard every pending old-session frame and rotate the logical pipe token before HelloAck.
        // Queued direct sends holding the previous token then fail under BridgePipeServer's send gate even on the same pipe.
        Volatile.Write(ref _bridgeReadyGeneration, 0);
        _remoteBridgeMapping.Clear();
        if (!_bridge.TryCaptureConnection(out var expectedConnection))
        {
            throw new IOException(
                "Bridge disconnected before role negotiation started.");
        }

        // Wait for any active queued write and prevent another from starting during the bridge's reset and replay.
        await using var deliveryBarrier = await _networkBridgePump
            .EnterDeliveryBarrierAsync(cancellationToken)
            .ConfigureAwait(false);
        var rotatedConnection = await _bridge.RotateConnectionTokenAsync(
                expectedConnection,
                cancellationToken)
            .ConfigureAwait(false);
        if (rotatedConnection is not { } bridgeConnection)
        {
            throw new IOException(
                "Bridge connection changed before role negotiation started.");
        }
        // Discard queued work from before the token change instead of feeding old gameplay into the newly configured bridge.
        _networkBridgePump.ClearPending();

        await SendRoleAcknowledgementAsync(
                bridgeConnection,
                cancellationToken)
            .ConfigureAwait(false);
        if (_config.Role == SessionRole.Guest)
        {
            var canReplayCachedState = false;
            var hadPendingReset =
                _guestReconnectResyncGate.HasPendingRequest;
            if (_network is { } activeNetwork &&
                activeNetwork.TryCaptureControlPeer(out var peer) &&
                IsMotionModeNegotiated(activeNetwork, peer))
            {
                await ReplayDeferredGuestReconnectResyncAsync(
                        activeNetwork,
                        peer,
                        bridgeConnection,
                        cancellationToken)
                    .ConfigureAwait(false);
                canReplayCachedState =
                    !hadPendingReset &&
                    IsMotionModeNegotiated(activeNetwork, peer) &&
                    !_guestReconnectResyncGate.HasPendingRequest;
            }

            if (canReplayCachedState)
            {
                await ReplayMissionStateToBridgeAsync(
                        bridgeConnection,
                        cancellationToken)
                    .ConfigureAwait(false);
                await ReplayMissionCinematicStateToBridgeAsync(
                        bridgeConnection,
                        cancellationToken)
                    .ConfigureAwait(false);
                await ReplayWorldGraphToBridgeAsync(
                        bridgeConnection,
                        cancellationToken)
                    .ConfigureAwait(false);
                await ReplayAnimSceneDefinitionToBridgeAsync(
                        bridgeConnection,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            else
            {
                await _logger.InfoAsync(
                    "bridge.guest-cache-replay-deferred",
                    "Deferred guest cache replay until the current peer generation is negotiated and its local reset is confirmed.",
                    new Dictionary<string, object?>
                    {
                        ["peerModeNegotiated"] =
                            IsMotionModeNegotiated(),
                        ["resyncPending"] =
                            _guestReconnectResyncGate.HasPendingRequest
                    },
                    cancellationToken).ConfigureAwait(false);
            }
        }
        await ReplayRestraintsToBridgeAsync(cancellationToken)
            .ConfigureAwait(false);
    }

    // Return a token identifying the currently configured game connection, or false if none is ready.
    // The token is a connection version so later code can notice that the pipe changed while it was waiting.
    private bool TryCaptureReadyBridgeConnection(
        out BridgePipeConnectionToken connection)
    {
        var generation = Volatile.Read(ref _bridgeReadyGeneration);
        if (generation == 0)
        {
            connection = default;
            return false;
        }

        connection = new BridgePipeConnectionToken(generation);
        if (_bridge.IsConnectionCurrent(connection))
        {
            return true;
        }

        _ = Interlocked.CompareExchange(
            ref _bridgeReadyGeneration,
            0,
            generation);
        connection = default;
        return false;
    }

    // Check that this token still names the ready game connection rather than an older connection.
    private bool IsReadyBridgeConnection(
        BridgePipeConnectionToken connection) =>
        connection.IsValid &&
        Volatile.Read(ref _bridgeReadyGeneration) == connection.Generation &&
        _bridge.IsConnectionCurrent(connection);

    // Check whether any currently configured game connection can be captured for delivery.
    private bool IsBridgeReady() =>
        TryCaptureReadyBridgeConnection(out _);

    // Check for a recent guest-player update delivered to this host bridge for this exact actor and connection pair.
    // This is delivery-based evidence that the host bridge knows the actor, not a direct query of the RDR2 entity.
    private bool IsRemoteBridgeMappingReady(
        ILanSession network,
        ControlPeerToken peer,
        BridgePipeConnectionToken bridge,
        NetEntityId actorEntityId) =>
        _remoteBridgeMapping.IsCurrent(
            network,
            peer,
            bridge,
            actorEntityId,
            Environment.TickCount64,
            RemoteBridgeMappingFreshnessMs);

    // Remember a guest player's successful delivery to the host's current game connection.
    // Old network callbacks cannot make a replacement peer or bridge appear ready.
    private void MarkRemoteBridgeMappingDelivered(
        ILanSession sourceNetwork,
        ControlPeerToken sourcePeer,
        NetEntityId entityId)
    {
        if (_config.Role != SessionRole.Host ||
            !ReferenceEquals(_network, sourceNetwork) ||
            !sourceNetwork.IsControlPeerCurrent(sourcePeer) ||
            !TryCaptureReadyBridgeConnection(out var bridgeConnection))
        {
            return;
        }

        _remoteBridgeMapping.MarkDelivered(
            sourceNetwork,
            sourcePeer,
            bridgeConnection,
            entityId,
            Environment.TickCount64);
    }

    // Get a new local bridge configuration revision and skip zero, which is reserved as an unset value.
    private uint NextMotionReplicationConfigRevision()
    {
        while (true)
        {
            var revision = unchecked((uint)Interlocked.Increment(
                ref _motionReplicationConfigRevision));
            if (revision != 0)
            {
                return revision;
            }
        }
    }

    // Get a new movement-mode announcement revision for the peer and skip the reserved zero value.
    private uint NextPeerMotionModeAnnouncementRevision()
    {
        while (true)
        {
            var revision = unchecked((uint)Interlocked.Increment(
                ref _peerMotionModeAnnouncementRevision));
            if (revision != 0)
            {
                return revision;
            }
        }
    }

    // Remember movement-mode agreement only for the network session and peer connection that supplied it.
    // Report whether this is new agreement, a repeated announcement, or an announcement from an old connection.
    private MotionNegotiationMarkDisposition TryMarkMotionModeNegotiated(
        ILanSession network,
        ControlPeerToken peer)
    {
        var disposition = MotionNegotiationMarkDisposition.StalePeer;
        if (!network.TryRunForControlPeer(peer, () =>
        {
            lock (_peerMotionModeSync)
            {
                if (!ReferenceEquals(_network, network))
                {
                    return;
                }

                if (Volatile.Read(ref _peerMotionModeNegotiated) != 0 &&
                    ReferenceEquals(_negotiatedMotionNetwork, network) &&
                    _negotiatedMotionPeer == peer)
                {
                    disposition =
                        MotionNegotiationMarkDisposition.Duplicate;
                    return;
                }

                _negotiatedMotionNetwork = network;
                _negotiatedMotionPeer = peer;
                Volatile.Write(ref _peerMotionModeNegotiated, 1);
                disposition = MotionNegotiationMarkDisposition.Accepted;
            }
        }))
        {
            return MotionNegotiationMarkDisposition.StalePeer;
        }
        return disposition;
    }

    // Check that movement-mode agreement belongs to the current peer connection rather than a previous connection.
    private bool IsMotionModeNegotiated(
        ILanSession network,
        ControlPeerToken peer)
    {
        if (!network.IsControlPeerCurrent(peer))
        {
            return false;
        }

        lock (_peerMotionModeSync)
        {
            return Volatile.Read(ref _peerMotionModeNegotiated) != 0 &&
                ReferenceEquals(_network, network) &&
                ReferenceEquals(_negotiatedMotionNetwork, network) &&
                _negotiatedMotionPeer == peer;
        }
    }

    // Check that movement-mode agreement belongs to the current peer connection rather than a previous connection.
    private bool IsMotionModeNegotiated()
    {
        var network = _network;
        return network is not null &&
            network.TryCaptureControlPeer(out var peer) &&
            IsMotionModeNegotiated(network, peer);
    }

    // Forget the previous peer's movement-mode agreement so a new connection must negotiate for itself.
    private void ResetMotionModeNegotiation()
    {
        lock (_peerMotionModeSync)
        {
            _negotiatedMotionNetwork = null;
            _negotiatedMotionPeer = default;
            Volatile.Write(ref _peerMotionModeNegotiated, 0);
        }
    }

    // Translate the local movement configuration enum into the enum used in multiplayer payloads.
    // Reject unknown modes instead of silently choosing a different movement system.
    private static MotionReplicationWireMode ToWireMotionMode(
        MotionReplicationMode mode) =>
        mode switch
        {
            MotionReplicationMode.TaskNavmesh =>
                MotionReplicationWireMode.TaskNavmesh,
            MotionReplicationMode.AnimGraphReplica =>
                MotionReplicationWireMode.AnimGraphReplica,
            _ => throw new ConfigurationException(
                $"Unsupported motion replication mode {mode}.")
        };

    // Send a status message to the local game's session menu, such as connecting, ready, or error.
    // This is a local pipe message rather than a gameplay update sent to the other player.
    private async ValueTask SendSessionStatusAsync(
        SessionMenuStatusPayload status,
        CancellationToken cancellationToken)
    {
        if (!_bridge.TryCaptureConnection(out var bridgeConnection))
        {
            throw new IOException(
                "Bridge disconnected before the session-menu status was queued.");
        }
        var envelope = new ProtocolEnvelope(
            MessageType.SessionMenuStatus,
            unchecked((uint)Interlocked.Increment(ref _bridgeSequence)),
            NetworkClock.Tick,
            SessionMenuPayloadCodec.EncodeStatus(status));
        if (!await _bridge.SendAsync(
                    bridgeConnection,
                    envelope,
                    cancellationToken)
                .ConfigureAwait(false))
        {
            throw new IOException(
                "Bridge disconnected before the session-menu status was delivered.");
        }
    }

    // Send the cached host NPC and object list as spawn messages so a reconnected game bridge can rebuild it.
    // Stop if the captured bridge connection changes instead of sending the remaining list to a new connection.
    private async ValueTask ReplayWorldGraphToBridgeAsync(
        BridgePipeConnectionToken bridgeConnection,
        CancellationToken cancellationToken)
    {
        // When RDR2 reconnects, resend saved NPCs in parent-first order so it can rebuild them without waiting for the next world update.
        var snapshot = _worldGraph.CaptureSpawnSnapshot();
        if (snapshot.Count == 0)
        {
            return;
        }

        var delivered = 0;
        // Stop if this bridge generation changes.
        // Never finish replaying an old session's graph into a freshly opened game connection.
        foreach (var envelope in snapshot)
        {
            if (!IsReadyBridgeConnection(bridgeConnection) ||
                !await _bridge.SendAsync(
                        bridgeConnection,
                        envelope,
                        CancellationToken.None)
                    .ConfigureAwait(false))
            {
                break;
            }
            delivered++;
            BridgeEnvelopeDelivered?.Invoke(envelope);
        }

        await _logger.InfoAsync(
            "entity-graph.bridge-replay",
            "Replayed the cached host-authoritative world graph after game-pipe reconnect.",
            new Dictionary<string, object?>
            {
                ["cachedNodes"] = snapshot.Count,
                ["deliveredNodes"] = delivered,
                ["complete"] = delivered == snapshot.Count
            },
            cancellationToken).ConfigureAwait(false);
    }

    // Restore the cached host mission state before replaying the NPC and object list that depends on it.
    private async ValueTask ReplayMissionStateToBridgeAsync(
        BridgePipeConnectionToken bridgeConnection,
        CancellationToken cancellationToken)
    {
        var mission = _missionStateCache.Capture();
        if (mission is null)
        {
            return;
        }

        var state = BinaryPayloadCodec.DecodeMissionState(
            mission.Payload.Span);
        var delivered = IsReadyBridgeConnection(bridgeConnection) &&
            await _bridge.SendAsync(
                    bridgeConnection,
                    mission,
                    CancellationToken.None)
                .ConfigureAwait(false);
        if (delivered)
        {
            BridgeEnvelopeDelivered?.Invoke(mission);
        }
        await _logger.InfoAsync(
            "mission-rx.bridge-replay",
            "Replayed the cached host-authoritative mission state before the world graph.",
            CreateMissionStateDiagnosticsData(
                mission,
                state,
                ("delivered", delivered),
                ("cacheDisposition", "Replay")),
            cancellationToken).ConfigureAwait(false);
    }

    // Restore the latest cached cinematic state after mission state and before rebuilding the shared world.
    private async ValueTask ReplayMissionCinematicStateToBridgeAsync(
        BridgePipeConnectionToken bridgeConnection,
        CancellationToken cancellationToken)
    {
        var cinematic = _missionCinematicStateCache.Capture();
        if (cinematic is null)
        {
            return;
        }

        var delivered = IsReadyBridgeConnection(bridgeConnection) &&
            await _bridge.SendAsync(
                    bridgeConnection,
                    cinematic,
                    CancellationToken.None)
                .ConfigureAwait(false);
        if (delivered)
        {
            BridgeEnvelopeDelivered?.Invoke(cinematic);
        }
        await _logger.InfoAsync(
            "mission-cinematic-rx.bridge-replay",
            "Replayed the cached cinematic FSM after MissionState and before the world graph.",
            CreateControlDiagnosticsData(
                cinematic,
                DescribeControlEnvelope(cinematic),
                "cache-to-bridge",
                delivered,
                ("cacheDisposition", "Replay")),
            cancellationToken).ConfigureAwait(false);
    }

    // Restore the cached animation-scene definition after its shared entity list has been replayed.
    // The definition tells the bridge which scene and roles belong together rather than playing the scene here.
    private async ValueTask ReplayAnimSceneDefinitionToBridgeAsync(
        BridgePipeConnectionToken bridgeConnection,
        CancellationToken cancellationToken)
    {
        var definition = _animSceneDefinitionCache.Capture();
        if (definition is null)
        {
            return;
        }

        var decoded = BinaryPayloadCodec.DecodeAnimSceneDefinition(
            definition.Payload.Span);
        var delivered = IsReadyBridgeConnection(bridgeConnection) &&
            await _bridge.SendAsync(
                    bridgeConnection,
                    definition,
                    CancellationToken.None)
                .ConfigureAwait(false);
        if (delivered)
        {
            BridgeEnvelopeDelivered?.Invoke(definition);
        }
        await _logger.InfoAsync(
            "animscene-definition.bridge-replay",
            "Replayed the latest active AnimScene definition after the authoritative world graph.",
            CreateAnimSceneDefinitionDiagnosticsData(
                definition,
                decoded,
                "cache-to-bridge",
                delivered,
                ("cacheDisposition", "Replay")),
            cancellationToken).ConfigureAwait(false);
    }

    // Build and send the host's ordered reconnect snapshot to the captured guest connection.
    // After a complete replay, also retry recorded campaign grants and pending mission completions.
    private async ValueTask ReplayAuthoritativeStateToPeerAsync(
        ILanSession network,
        ControlPeerToken peer,
        CancellationToken cancellationToken)
    {
        if (_config.Role != SessionRole.Host)
        {
            return;
        }

        AuthoritativePeerResyncReplayPlan? capturedPlan = null;
        var peerAccepted = false;
        // Save the current host state and resend it as one ordered repair.
        // Do not mix normal NPC spawns into the middle of that repair.
        var result = await _peerControlSendGate.RunAsync(
                async token =>
                {
                    if (!ReferenceEquals(_network, network) ||
                        !network.IsControlPeerCurrent(peer))
                    {
                        return default(AuthoritativePeerResyncReplayResult);
                    }

                    peerAccepted = true;
                    // Block stale remote-to-bridge mappings while authoritative frames are replayed to this new/reconnected peer.
                    // Stop trusting the old guest actor mapping while the host prepares the guest's replacement world snapshot.
                    _remoteBridgeMapping.BeginResync(network, peer);
                    _entities.Clear();
                    // Capture the related caches together under the control gate so the replay uses a consistent mission and entity list.
                    capturedPlan = AuthoritativePeerResyncReplay.Create(
                        _missionStateCache.Capture(),
                        _missionCinematicStateCache.Capture(),
                        _worldGraph.CaptureSpawnSnapshot(),
                        _animSceneDefinitionCache.Capture());

                    async ValueTask<bool> SendReliableAsync(
                        ProtocolEnvelope frame,
                        CancellationToken sendToken)
                    {
                        var delivered = await network.SendControlAsync(
                                peer,
                                frame.Type,
                                frame.Payload,
                                frame.Tick,
                                sendToken)
                            .ConfigureAwait(false);
                        if (delivered)
                        {
                            _messageFlowDiagnostics.MarkDelivered(
                                MessageFlowDirection.BridgeToNetwork,
                                frame.Type);
                        }
                        else
                        {
                            _messageFlowDiagnostics.MarkDropped(
                                MessageFlowDirection.BridgeToNetwork,
                                frame.Type);
                        }
                        return delivered;
                    }

                    return await AuthoritativePeerResyncReplay.SendAsync(
                            capturedPlan.Value,
                            SendReliableAsync,
                            token)
                        .ConfigureAwait(false);
                },
                cancellationToken)
            .ConfigureAwait(false);
        // Only follow a completed world replay with durable-event retries, keeping each grant or completion's original identity.
        if (peerAccepted && result.Completed)
        {
            foreach (var grant in _capabilityJournal.CaptureReplay())
            {
                var payload = BinaryPayloadCodec.EncodeCampaignCapability(
                    new CampaignCapabilityPayload(
                        (CampaignCapabilityKind)grant.Kind,
                        grant.RecordHash,
                        grant.HostEventId,
                        grant.GrantedAtUnixMilliseconds));
                _ = await SendPeerControlAsync(
                        network, peer, MessageType.CampaignCapability,
                        payload, unchecked((ulong)Environment.TickCount64),
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            foreach (var completion in _missionProgressionJournal.CapturePending())
            {
                var payload = BinaryPayloadCodec.EncodeMissionProgression(
                    completion.ToPayload());
                _ = await SendPeerControlAsync(
                        network, peer, MessageType.MissionProgression,
                        payload, unchecked((ulong)Environment.TickCount64),
                        cancellationToken)
                    .ConfigureAwait(false);
            }
        }
        var plan = capturedPlan ?? new AuthoritativePeerResyncReplayPlan(
            [],
            AuthoritativePeerResyncDefinitionDisposition.NotCached);
        var data = new Dictionary<string, object?>
        {
            ["peerAccepted"] = peerAccepted,
            ["peerGeneration"] = peer.Generation,
            ["plannedFrames"] = plan.Frames.Count,
            ["deliveredFrames"] = result.DeliveredFrames,
            ["complete"] = result.Completed,
            ["failedType"] = result.FailedType?.ToString(),
            ["definitionDisposition"] =
                plan.DefinitionDisposition.ToString(),
            ["definitionDelivered"] = result.DefinitionDelivered,
            ["order"] = string.Join(
                ",",
                plan.Frames.Select(static frame => frame.Type))
        };
        if (peerAccepted &&
            result.Completed &&
            plan.DefinitionDisposition is
                AuthoritativePeerResyncDefinitionDisposition.Included or
                AuthoritativePeerResyncDefinitionDisposition.NotCached)
        {
            await _logger.InfoAsync(
                "network.resync-cache-replay",
                "Replayed the coherent host control cache after the guest resync request.",
                data,
                cancellationToken).ConfigureAwait(false);
        }
        else
        {
            await _logger.WarningAsync(
                "network.resync-cache-replay-incomplete",
                "Stopped or restricted host cache replay before an unsafe AnimScene definition could be sent.",
                data,
                cancellationToken).ConfigureAwait(false);
        }
    }

    // Identify host messages whose cache changes must wait their turn alongside a reconnect replay.
    // This keeps a replay from mixing mission or entity-list changes from different moments.
    private bool ShouldSerializeHostReplayStateMutation(
        MessageType type) =>
        _config.Role == SessionRole.Host &&
        type is MessageType.MissionState or
            MessageType.MissionCinematicState or
            MessageType.AnimSceneDefinition or
            MessageType.AnimSceneControl or
            MessageType.EntitySpawn or
            MessageType.EntityDespawn;

    // Send a reliable control message through the shared send gate so it keeps its order with other control work.
    // The overload with an already-held gate avoids trying to acquire that same gate again.
    private ValueTask<bool> SendPeerControlAsync(
        ILanSession expectedNetwork,
        MessageType type,
        ReadOnlyMemory<byte> payload,
        ulong tick,
        bool controlSendGateHeld,
        CancellationToken cancellationToken)
    {
        if (controlSendGateHeld)
        {
            return SendPeerControlUnderGateAsync(
                expectedNetwork,
                type,
                payload,
                tick,
                cancellationToken);
        }

        return _peerControlSendGate.RunAsync(
            token => SendPeerControlUnderGateAsync(
                expectedNetwork,
                type,
                payload,
                tick,
                token),
            cancellationToken);
    }

    // With the send gate already held, verify the requested session or peer is still current before sending.
    // Return false if its connection was replaced rather than sending the message to the replacement peer.
    private async ValueTask<bool> SendPeerControlUnderGateAsync(
        ILanSession expectedNetwork,
        MessageType type,
        ReadOnlyMemory<byte> payload,
        ulong tick,
        CancellationToken cancellationToken)
    {
        if (!ReferenceEquals(_network, expectedNetwork) ||
            !expectedNetwork.TryCaptureControlPeer(out var peer))
        {
            return false;
        }

        return await expectedNetwork.SendControlAsync(
                peer,
                type,
                payload,
                tick,
                cancellationToken)
            .ConfigureAwait(false);
    }

    // Send a reliable control message through the shared send gate so it keeps its order with other control work.
    // The overload with an already-held gate avoids trying to acquire that same gate again.
    private ValueTask<bool> SendPeerControlAsync(
        ILanSession expectedNetwork,
        ControlPeerToken expectedPeer,
        MessageType type,
        ReadOnlyMemory<byte> payload,
        ulong tick,
        CancellationToken cancellationToken) =>
        _peerControlSendGate.RunAsync(
            token => SendPeerControlUnderGateAsync(
                expectedNetwork,
                expectedPeer,
                type,
                payload,
                tick,
                token),
            cancellationToken);

    // With the send gate already held, verify the requested session or peer is still current before sending.
    // Return false if its connection was replaced rather than sending the message to the replacement peer.
    private async ValueTask<bool> SendPeerControlUnderGateAsync(
        ILanSession expectedNetwork,
        ControlPeerToken expectedPeer,
        MessageType type,
        ReadOnlyMemory<byte> payload,
        ulong tick,
        CancellationToken cancellationToken)
    {
        if (!ReferenceEquals(_network, expectedNetwork) ||
            !expectedNetwork.IsControlPeerCurrent(expectedPeer))
        {
            return false;
        }

        return await expectedNetwork.SendControlAsync(
                expectedPeer,
                type,
                payload,
                tick,
                cancellationToken)
            .ConfigureAwait(false);
    }

    // Take a turn through the control gate before restoring cached restraint states to the local game.
    private async ValueTask ReplayRestraintsToBridgeAsync(
        CancellationToken cancellationToken)
    {
        _ = await _peerControlSendGate.RunAsync(
                async token =>
                {
                    await ReplayRestraintsToBridgeUnderGateAsync(token)
                        .ConfigureAwait(false);
                    return true;
                },
                cancellationToken)
            .ConfigureAwait(false);
    }

    // Send each remembered restraint state to the captured ready bridge while the caller holds the control gate.
    // Stop if that bridge disappears so the replay does not spill into another connection.
    private async ValueTask ReplayRestraintsToBridgeUnderGateAsync(
        CancellationToken cancellationToken)
    {
        if (!TryCaptureReadyBridgeConnection(out var bridgeConnection))
        {
            return;
        }

        var states = _interactions.CaptureRestraints();
        foreach (var state in states)
        {
            var envelope = CreateBridgeEnvelope(
                MessageType.RestraintState,
                BinaryPayloadCodec.EncodeRestraintState(state));
            if (!IsReadyBridgeConnection(bridgeConnection) ||
                !await _bridge.SendAsync(
                        bridgeConnection,
                        envelope,
                        CancellationToken.None)
                    .ConfigureAwait(false))
            {
                break;
            }
            BridgeEnvelopeDelivered?.Invoke(envelope);
        }
    }

    // Wrap payload bytes with their message type, a new local pipe sequence number, and the current network-clock tick.
    // This constructs a message but does not send it.
    private ProtocolEnvelope CreateBridgeEnvelope(
        MessageType type,
        ReadOnlyMemory<byte> payload) =>
        new(
            type,
            unchecked((uint)Interlocked.Increment(ref _bridgeSequence)),
            NetworkClock.Tick,
            payload);

    // Look up the most recently stored player snapshot for authority checks, or return null if it is unknown.
    private ReplicatedPlayerSnapshot? LookupLatestPlayer(
        NetEntityId entityId) =>
        _entities.TryGetLatest(entityId, out var snapshot)
            ? snapshot
            : null;

    // Send a host-approved interaction result or state to both the local game and the peer.
    // Log partial delivery because writing to one endpoint does not guarantee the other received the same decision.
    private async ValueTask BroadcastAuthoritativeControlAsync(
        MessageType type,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        if (_config.Role != SessionRole.Host)
        {
            throw new InvalidOperationException(
                "Only the host may broadcast authoritative interaction state.");
        }

        var bridgeEnvelope = CreateBridgeEnvelope(type, payload);
        var bridgeDelivered =
            TryCaptureReadyBridgeConnection(out var bridgeConnection) &&
            await _bridge.SendAsync(
                    bridgeConnection,
                    bridgeEnvelope,
                    cancellationToken)
                .ConfigureAwait(false);
        if (bridgeDelivered)
        {
            BridgeEnvelopeDelivered?.Invoke(bridgeEnvelope);
        }

        var network = _network;
        var peerDelivered = network is not null &&
            await SendPeerControlAsync(
                    network,
                    type,
                    payload,
                    NetworkClock.Tick,
                    controlSendGateHeld: false,
                    cancellationToken)
                .ConfigureAwait(false);
        if (!bridgeDelivered || (network?.IsConnected == true && !peerDelivered))
        {
            await _logger.WarningAsync(
                "interaction.authority-delivery-partial",
                "An authoritative interaction transition was not delivered to every active endpoint.",
                new Dictionary<string, object?>
                {
                    ["messageType"] = type.ToString(),
                    ["bridgeDelivered"] = bridgeDelivered,
                    ["peerConnected"] = network?.IsConnected ?? false,
                    ["peerDelivered"] = peerDelivered
                },
                cancellationToken).ConfigureAwait(false);
        }
    }

    // Deliver a peer-triggered, host-approved result to the exact host bridge captured before the decision.
    // A replacement game connection must not receive a decision made for the old one.
    private async ValueTask<bool>
        DeliverPeerAuthoritativeControlToBridgeUnderGateAsync(
            BridgePipeConnectionToken expectedBridge,
            MessageType type,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken)
    {
        if (_config.Role != SessionRole.Host)
        {
            throw new InvalidOperationException(
                "Only the host may broadcast authoritative interaction state.");
        }

        if (!IsReadyBridgeConnection(expectedBridge))
        {
            return false;
        }

        var bridgeEnvelope = CreateBridgeEnvelope(type, payload);
        var bridgeDelivered = await _bridge.SendAsync(
                    expectedBridge,
                    bridgeEnvelope,
                    CancellationToken.None)
                .ConfigureAwait(false);
        if (bridgeDelivered)
        {
            BridgeEnvelopeDelivered?.Invoke(bridgeEnvelope);
        }

        if (!bridgeDelivered)
        {
            await _logger.WarningAsync(
                "interaction.authority-bridge-delivery-failed",
                "A peer-originated authority transition was not delivered to its captured game-pipe generation.",
                new Dictionary<string, object?>
                {
                    ["messageType"] = type.ToString(),
                    ["bridgeDelivered"] = bridgeDelivered,
                    ["bridgeGeneration"] = expectedBridge.Generation
                },
                cancellationToken).ConfigureAwait(false);
        }
        return bridgeDelivered;
    }

    // Send a local host decision to the peer and optional bridge captured before the state change.
    // Keep using those connection tokens so a reconnect cannot redirect half of the decision to new endpoints.
    private async ValueTask<bool>
        BroadcastLocalAuthoritativeControlUnderGateAsync(
            ILanSession expectedNetwork,
            ControlPeerToken expectedPeer,
            BridgePipeConnectionToken? expectedBridge,
            MessageType type,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken)
    {
        if (_config.Role != SessionRole.Host)
        {
            throw new InvalidOperationException(
                "Only the host may broadcast authoritative interaction state.");
        }

        var bridgeEnvelope = CreateBridgeEnvelope(type, payload);
        var bridgeDelivered = expectedBridge is { } bridgeConnection &&
            IsReadyBridgeConnection(bridgeConnection) &&
            await _bridge.SendAsync(
                    bridgeConnection,
                    bridgeEnvelope,
                    CancellationToken.None)
                .ConfigureAwait(false);
        if (bridgeDelivered)
        {
            BridgeEnvelopeDelivered?.Invoke(bridgeEnvelope);
        }

        var peerDelivered = await SendPeerControlUnderGateAsync(
                expectedNetwork,
                expectedPeer,
                type,
                payload,
                NetworkClock.Tick,
                cancellationToken)
            .ConfigureAwait(false);
        if (!bridgeDelivered || !peerDelivered)
        {
            await _logger.WarningAsync(
                "interaction.authority-delivery-partial",
                "A local generation-bound interaction transition was not delivered to every endpoint captured before its mutation.",
                new Dictionary<string, object?>
                {
                    ["messageType"] = type.ToString(),
                    ["bridgeDelivered"] = bridgeDelivered,
                    ["peerDelivered"] = peerDelivered,
                    ["peerGeneration"] = expectedPeer.Generation
                },
                cancellationToken).ConfigureAwait(false);
        }
        return bridgeDelivered && peerDelivered;
    }

    // Deliver emergency local recovery to the captured game connection and notify a captured peer if one exists.
    // Its return value reports local game delivery, even when peer notification fails.
    private async ValueTask<bool>
        BroadcastLocalSelfRecoveryUnderGateAsync(
            BridgePipeConnectionToken expectedBridge,
            ILanSession? expectedNetwork,
            ControlPeerToken? expectedPeer,
            MessageType type,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken)
    {
        var bridgeEnvelope = CreateBridgeEnvelope(type, payload);
        var bridgeDelivered = IsReadyBridgeConnection(expectedBridge) &&
            await _bridge.SendAsync(
                    expectedBridge,
                    bridgeEnvelope,
                    CancellationToken.None)
                .ConfigureAwait(false);
        if (bridgeDelivered)
        {
            BridgeEnvelopeDelivered?.Invoke(bridgeEnvelope);
        }

        var peerDelivered = expectedNetwork is null ||
            expectedPeer is null ||
            await SendPeerControlUnderGateAsync(
                    expectedNetwork,
                    expectedPeer.Value,
                    type,
                    payload,
                    NetworkClock.Tick,
                    cancellationToken)
                .ConfigureAwait(false);
        if (!bridgeDelivered || !peerDelivered)
        {
            await _logger.WarningAsync(
                "interaction.authority-delivery-partial",
                "The local emergency recovery completed only on endpoints that retained its captured authority generation.",
                new Dictionary<string, object?>
                {
                    ["messageType"] = type.ToString(),
                    ["bridgeDelivered"] = bridgeDelivered,
                    ["peerExpected"] = expectedPeer.HasValue,
                    ["peerDelivered"] = peerDelivered,
                    ["peerGeneration"] = expectedPeer?.Generation
                },
                cancellationToken).ConfigureAwait(false);
        }
        return bridgeDelivered;
    }

    // Host checks an interaction requested by its own RDR2 bridge, records the accepted state, then sends the same result to the guest when needed.
    // Ask the host's interaction rules to resolve a local request and distribute the resulting interaction and restraint states.
    // Emergency self-recovery can still reach the local game without requiring a negotiated peer.
    private async ValueTask<bool> ResolveLocalHostInteractionAsync(
        InteractionIntentPayload intent,
        CancellationToken cancellationToken)
    {
        BridgePipeConnectionToken? bridgeConnection = null;
        InteractionAuthorityResolution? resolution = null;
        bool processed;
        if (intent.Kind == InteractionKind.EmergencyRecover &&
            intent.ActorEntityId == intent.TargetEntityId)
        {
            processed = await _peerControlSendGate.RunAsync(
                async token =>
                {
                    if (!TryCaptureReadyBridgeConnection(
                            out var capturedBridge))
                    {
                        return false;
                    }
                    bridgeConnection = capturedBridge;
                    ILanSession? peerNetwork = null;
                    ControlPeerToken? peer = null;
                    if (_network is { } candidate &&
                        candidate.TryCaptureControlPeer(out var candidatePeer) &&
                        ReferenceEquals(_network, candidate) &&
                        IsMotionModeNegotiated(candidate, candidatePeer))
                    {
                        peerNetwork = candidate;
                        peer = candidatePeer;
                    }
                    resolution = _interactions.Resolve(
                        intent,
                        Environment.TickCount64,
                        LookupLatestPlayer);
                    await BroadcastLocalSelfRecoveryUnderGateAsync(
                            capturedBridge,
                            peerNetwork,
                            peer,
                            MessageType.InteractionResult,
                            BinaryPayloadCodec.EncodeInteractionResult(
                                resolution.Value.Result),
                            token)
                        .ConfigureAwait(false);
                    if (resolution.Value.RestraintState is { } restraint)
                    {
                        await BroadcastLocalSelfRecoveryUnderGateAsync(
                                capturedBridge,
                                peerNetwork,
                                peer,
                                MessageType.RestraintState,
                                BinaryPayloadCodec.EncodeRestraintState(
                                    restraint),
                                token)
                            .ConfigureAwait(false);
                    }
                    return true;
                },
                cancellationToken)
                .ConfigureAwait(false);
        }
        else
        {
            processed = await NegotiatedPeerMutationTransaction.RunAsync(
                    _peerControlSendGate,
                    () => _network,
                    IsMotionModeNegotiated,
                    () =>
                    {
                        bridgeConnection =
                            TryCaptureReadyBridgeConnection(
                                out var capturedBridge)
                                ? capturedBridge
                                : null;
                        resolution = _interactions.Resolve(
                            intent,
                            Environment.TickCount64,
                            LookupLatestPlayer);
                        return resolution.Value;
                    },
                    async (network, peer, resolved, token) =>
                    {
                        await BroadcastLocalAuthoritativeControlUnderGateAsync(
                                network,
                                peer,
                                bridgeConnection,
                                MessageType.InteractionResult,
                                BinaryPayloadCodec.EncodeInteractionResult(
                                    resolved.Result),
                                token)
                            .ConfigureAwait(false);
                        if (resolved.RestraintState is { } restraint)
                        {
                            await BroadcastLocalAuthoritativeControlUnderGateAsync(
                                    network,
                                    peer,
                                    bridgeConnection,
                                    MessageType.RestraintState,
                                    BinaryPayloadCodec.EncodeRestraintState(
                                        restraint),
                                    token)
                                .ConfigureAwait(false);
                        }
                    },
                    cancellationToken)
                .ConfigureAwait(false);
        }

        if (!processed || resolution is null)
        {
            return false;
        }

        await _logger.InfoAsync(
            "interaction.authority-resolved",
            $"Host resolved {intent.Kind} as {resolution.Value.Result.Status}.",
            new Dictionary<string, object?>
            {
                ["interactionId"] = intent.InteractionId,
                ["revision"] = intent.Revision,
                ["kind"] = intent.Kind.ToString(),
                ["phase"] = intent.Phase.ToString(),
                ["actorEntityId"] = intent.ActorEntityId.Value,
                ["targetEntityId"] = intent.TargetEntityId.Value,
                ["status"] = resolution.Value.Result.Status.ToString(),
                ["reason"] = resolution.Value.Result.RejectReason.ToString(),
                ["progressMs"] =
                    resolution.Value.Result.ProgressMilliseconds,
                ["requiredMs"] =
                    resolution.Value.Result.RequiredDurationMilliseconds
            },
            cancellationToken).ConfigureAwait(false);
        return true;
    }

    // Host checks a guest interaction request.
    // Guest input is never forwarded straight into RDR2 without range/state/role validation here.
    // Resolve a guest interaction under checks tying the peer, host game connection, and guest actor mapping together.
    // Capture the previous registry state so a failure before any local control delivery can be rolled back.
    private async ValueTask<bool> ResolvePeerInteractionAsync(
        InteractionIntentPayload intent,
        ILanSession sourceNetwork,
        ControlPeerToken sourcePeer,
        CancellationToken cancellationToken)
    {
        InteractionAuthorityResolution? resolved = null;
        var processed = await PeerBridgeAuthorityTransaction.RunAsync<
                InteractionAuthorityResolution,
                PeerAuthorityControlFrame,
                AuthoritativeInteractionRegistry.TransactionSnapshot>(
                _bridgeSessionGenerationGate,
                _peerControlSendGate,
                sourceNetwork,
                sourcePeer,
                () => _network,
                IsMotionModeNegotiated,
                () => TryCaptureReadyBridgeConnection(
                        out var bridgeConnection)
                    ? bridgeConnection
                    : null,
                IsReadyBridgeConnection,
                (network, peer, bridge) =>
                    IsRemoteBridgeMappingReady(
                        network,
                        peer,
                        bridge,
                        intent.ActorEntityId),
                _interactions.CaptureTransactionSnapshot,
                () =>
                {
                    resolved = _interactions.Resolve(
                        intent,
                        Environment.TickCount64,
                        LookupLatestPlayer);
                    return resolved.Value;
                },
                _interactions.RestoreTransactionSnapshot,
                static resolution => resolution.RestraintState is
                    { } restraint
                        ?
                        [
                            new PeerAuthorityControlFrame(
                                MessageType.InteractionResult,
                                BinaryPayloadCodec.EncodeInteractionResult(
                                    resolution.Result)),
                            new PeerAuthorityControlFrame(
                                MessageType.RestraintState,
                                BinaryPayloadCodec.EncodeRestraintState(
                                    restraint))
                        ]
                        :
                        [
                            new PeerAuthorityControlFrame(
                                MessageType.InteractionResult,
                                BinaryPayloadCodec.EncodeInteractionResult(
                                    resolution.Result))
                        ],
                (bridge, control, token) =>
                    DeliverPeerAuthoritativeControlToBridgeUnderGateAsync(
                        bridge,
                        control.Type,
                        control.Payload,
                        token),
                (network, peer, control, token) =>
                    SendPeerControlUnderGateAsync(
                        network,
                        peer,
                        control.Type,
                        control.Payload,
                        NetworkClock.Tick,
                        token),
                cancellationToken)
            .ConfigureAwait(false);
        if (!processed || resolved is null)
        {
            return false;
        }

        await _logger.InfoAsync(
            "interaction.authority-resolved",
            $"Host resolved {intent.Kind} as {resolved.Value.Result.Status}.",
            new Dictionary<string, object?>
            {
                ["interactionId"] = intent.InteractionId,
                ["revision"] = intent.Revision,
                ["kind"] = intent.Kind.ToString(),
                ["phase"] = intent.Phase.ToString(),
                ["actorEntityId"] = intent.ActorEntityId.Value,
                ["targetEntityId"] = intent.TargetEntityId.Value,
                ["status"] = resolved.Value.Result.Status.ToString(),
                ["reason"] =
                    resolved.Value.Result.RejectReason.ToString(),
                ["progressMs"] =
                    resolved.Value.Result.ProgressMilliseconds,
                ["requiredMs"] =
                    resolved.Value.Result.RequiredDurationMilliseconds,
                ["peerGeneration"] = sourcePeer.Generation
            },
            cancellationToken).ConfigureAwait(false);
        return true;
    }

    // Check the requested guest actor, optional host target, snapshot ages, and action range before producing a host-approved action.
    // Ending an action relaxes the age and distance checks so stale movement does not by itself prevent cleanup.
    private (
        PlayerActionPayload? Resolved,
        string Rejection,
        long? ActorAgeMilliseconds,
        long? TargetAgeMilliseconds) EvaluateGuestPlayerAction(
            PlayerActionPayload intent)
    {
        var terminal = intent.Phase is
            PlayerActionPhase.End or
            PlayerActionPhase.Cancel or
            PlayerActionPhase.Reject;
        var actor = LookupLatestPlayer(intent.ActorEntityId);
        var target = intent.Flags.HasFlag(
                PlayerActionFlags.TargetEntityValid)
            ? LookupLatestPlayer(intent.TargetEntityId)
            : null;
        var rejection = string.Empty;
        // A missing or old actor snapshot cannot support a new action decision based on the player's current position.
        if (actor is null ||
            actor.Value.State.Slot != (byte)SessionRole.Guest ||
            (!terminal && actor.Value.AgeMilliseconds > 2_000))
        {
            rejection = "invalid-or-stale-guest-actor";
        }
        else if (intent.Flags.HasFlag(
                     PlayerActionFlags.TargetEntityValid) &&
                 (target is null ||
                  target.Value.State.Slot != (byte)SessionRole.Host ||
                  (!terminal && target.Value.AgeMilliseconds > 2_000)))
        {
            rejection = "invalid-or-stale-host-target";
        }
        else if (!terminal && actor is not null && target is not null)
        {
            var distance = System.Numerics.Vector3.Distance(
                actor.Value.State.Position,
                target.Value.State.Position);
            // Different actions have different allowed ranges, so a valid aim distance would not automatically allow melee at that distance.
            var maximumDistance = intent.Kind switch
            {
                PlayerActionKind.Lasso or PlayerActionKind.Hogtie => 15.0f,
                PlayerActionKind.Aim => 250.0f,
                _ => 4.0f
            };
            if (!float.IsFinite(distance) || distance > maximumDistance)
            {
                rejection = "target-out-of-authoritative-range";
            }
        }

        if (rejection.Length != 0)
        {
            return (
                Resolved: null,
                rejection,
                actor?.AgeMilliseconds,
                target?.AgeMilliseconds);
        }

        // Change the message from a guest request into a host-approved result only after the checks above pass.
        var resolvedFlags =
            (intent.Flags & ~PlayerActionFlags.Intent) |
            PlayerActionFlags.Authoritative;
        return (intent with
        {
            AuthoritySlot = (byte)SessionRole.Host,
            Flags = resolvedFlags,
            ActorAnchor = intent.Flags.HasFlag(
                PlayerActionFlags.ActorAnchorValid)
                    ? actor!.Value.State.Position
                    : System.Numerics.Vector3.Zero,
            TargetPoint =
                intent.Flags.HasFlag(PlayerActionFlags.TargetPointValid) &&
                target is not null
                    ? target.Value.State.Position +
                        new System.Numerics.Vector3(0, 0, 0.75f)
                    : intent.TargetPoint
        },
        Rejection: string.Empty,
        actor?.AgeMilliseconds,
        target?.AgeMilliseconds);
    }

    // Turn an ending lasso or hogtie action into host-approved cleanup without requiring fresh movement samples.
    // The caller separately checks that it matches an existing restraint before using this special path.
    internal static (
        PlayerActionPayload? Resolved,
        string Rejection,
        long? ActorAgeMilliseconds,
        long? TargetAgeMilliseconds) EvaluateTerminalRestraintCleanup(
            PlayerActionPayload intent)
    {
        if (intent.Kind is not (
                PlayerActionKind.Lasso or PlayerActionKind.Hogtie) ||
            intent.Phase is not (
                PlayerActionPhase.End or
                PlayerActionPhase.Cancel or
                PlayerActionPhase.Reject))
        {
            return (
                null,
                "invalid-terminal-restraint-cleanup",
                null,
                null);
        }

        // The registry proof is evaluated while both authority-generation gates are held.
        // A terminal lasso/hogtie commonly arrives after the Resync reset has deliberately cleared the transient PlayerState snapshot, so cleanup must not depend on that positional cache.
        // The stable actor + action id still has to match an active restraint.
        return (
            intent with
            {
                AuthoritySlot = (byte)SessionRole.Host,
                Flags =
                    (intent.Flags & ~PlayerActionFlags.Intent) |
                    PlayerActionFlags.Authoritative
            },
            string.Empty,
            null,
            null);
    }

    // Record why an action was rejected together with the actor and target snapshot ages.
    // These values help distinguish stale movement data from an invalid action or out-of-range target.
    private ValueTask LogRejectedGuestPlayerActionAsync(
        PlayerActionPayload intent,
        string rejection,
        long? actorAgeMilliseconds,
        long? targetAgeMilliseconds,
        CancellationToken cancellationToken) =>
        _logger.WarningAsync(
            "player-action.authority-rejected",
            $"Host rejected guest {intent.Kind} action: {rejection}.",
            new Dictionary<string, object?>
            {
                ["actionId"] = intent.ActionId,
                ["revision"] = intent.Revision,
                ["kind"] = intent.Kind.ToString(),
                ["phase"] = intent.Phase.ToString(),
                ["actorEntityId"] = intent.ActorEntityId.Value,
                ["targetEntityId"] = intent.TargetEntityId.Value,
                ["reason"] = rejection,
                ["actorAgeMs"] = actorAgeMilliseconds,
                ["targetAgeMs"] = targetAgeMilliseconds
            },
            cancellationToken);

    // Host validates a guest action transaction and returns a host-owned action state.
    // This is the control boundary for lasso/melee/crafting requests.
    // Validate a guest action, check its allowed action-state transition, and prepare any resulting restraint change.
    // Deliver through the captured host bridge and peer connection, with rollback available until the first local control delivery.
    private async ValueTask<bool> ResolvePeerGuestPlayerActionAsync(
        PlayerActionPayload intent,
        ILanSession sourceNetwork,
        ControlPeerToken sourcePeer,
        CancellationToken cancellationToken)
    {
        // Cleanup for an already-authoritative restraint must survive expiry of the short positional mapping lease.
        // This proof cannot authorize a new physical action: actor and action id must match an active host restraint while the transaction holds both generation gates.
        var terminalRestraintCleanup = false;
        (
            PlayerActionPayload? Resolved,
            string Rejection,
            long? ActorAgeMilliseconds,
            long? TargetAgeMilliseconds) evaluation =
            (null, string.Empty, null, null);
        var processed = await PeerBridgeAuthorityTransaction.RunAsync<
                PeerGuestPlayerActionAuthorityResolution,
                PeerAuthorityControlFrame,
                (
                    AuthoritativeInteractionRegistry.TransactionSnapshot Interactions,
                    AuthoritativePlayerActionStateMachine.TransactionSnapshot Actions)>(
                _bridgeSessionGenerationGate,
                _peerControlSendGate,
                sourceNetwork,
                sourcePeer,
                () => _network,
                IsMotionModeNegotiated,
                () => TryCaptureReadyBridgeConnection(
                        out var bridgeConnection)
                    ? bridgeConnection
                    : null,
                IsReadyBridgeConnection,
                (network, peer, bridge) =>
                {
                    terminalRestraintCleanup =
                        _interactions.HasMatchingTerminalRestraint(intent);
                    return terminalRestraintCleanup ||
                        IsRemoteBridgeMappingReady(
                        network,
                        peer,
                        bridge,
                        intent.ActorEntityId);
                },
                () => (
                    _interactions.CaptureTransactionSnapshot(),
                    _playerActions.CaptureTransactionSnapshot()),
                () =>
                {
                    evaluation = terminalRestraintCleanup
                        ? EvaluateTerminalRestraintCleanup(intent)
                        : EvaluateGuestPlayerAction(intent);
                    if (evaluation.Resolved is { } candidate &&
                        !terminalRestraintCleanup &&
                        !_playerActions.TryAuthorize(
                            candidate,
                            out var actionRejection))
                    {
                        evaluation = (
                            null,
                            actionRejection,
                            evaluation.ActorAgeMilliseconds,
                            evaluation.TargetAgeMilliseconds);
                    }
                    var restraint = evaluation.Resolved is { } resolved
                        ? _interactions.ObserveAuthoritativePlayerAction(
                            resolved)
                        : null;
                    return new PeerGuestPlayerActionAuthorityResolution(
                        evaluation.Resolved,
                        restraint);
                },
                snapshot =>
                {
                    _interactions.RestoreTransactionSnapshot(
                        snapshot.Interactions);
                    _playerActions.RestoreTransactionSnapshot(
                        snapshot.Actions);
                },
                static resolution => resolution.Resolved is
                    { } resolved
                        ? resolution.Restraint is { } restraint
                            ?
                            [
                                new PeerAuthorityControlFrame(
                                    MessageType.PlayerAction,
                                    BinaryPayloadCodec.EncodePlayerAction(
                                        resolved)),
                                new PeerAuthorityControlFrame(
                                    MessageType.RestraintState,
                                    BinaryPayloadCodec.EncodeRestraintState(
                                        restraint))
                            ]
                            :
                            [
                                new PeerAuthorityControlFrame(
                                    MessageType.PlayerAction,
                                    BinaryPayloadCodec.EncodePlayerAction(
                                        resolved))
                            ]
                        : [],
                (bridge, control, token) =>
                    DeliverPeerAuthoritativeControlToBridgeUnderGateAsync(
                        bridge,
                        control.Type,
                        control.Payload,
                        token),
                (network, peer, control, token) =>
                    SendPeerControlUnderGateAsync(
                        network,
                        peer,
                        control.Type,
                        control.Payload,
                        NetworkClock.Tick,
                        token),
                cancellationToken)
            .ConfigureAwait(false);
        if (!processed)
        {
            return false;
        }

        if (evaluation.Resolved is null)
        {
            await LogRejectedGuestPlayerActionAsync(
                    intent,
                    evaluation.Rejection,
                    evaluation.ActorAgeMilliseconds,
                    evaluation.TargetAgeMilliseconds,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        return true;
    }

    // Host turns its own sampled action into the shared authoritative version before broadcasting it to the guest and its local RDR2 bridge.
    // Check a locally reported host action against the action rules, then send it and any related restraint change.
    // The negotiated-peer transaction keeps that restraint change tied to the peer captured before it was made.
    private async ValueTask<bool> ProcessLocalHostPlayerActionAsync(
        ProtocolEnvelope envelope,
        CancellationToken cancellationToken)
    {
        var action = BinaryPayloadCodec.DecodePlayerAction(
            envelope.Payload.Span);
        if (!_playerActions.TryAuthorize(action, out var actionRejection))
        {
            await LogRejectedGuestPlayerActionAsync(
                    action,
                    actionRejection,
                    actorAgeMilliseconds: null,
                    targetAgeMilliseconds: null,
                    cancellationToken)
                .ConfigureAwait(false);
            return true;
        }
        BridgePipeConnectionToken? bridgeConnection = null;
        return await NegotiatedPeerMutationTransaction.RunAsync(
                _peerControlSendGate,
                () => _network,
                IsMotionModeNegotiated,
                () =>
                {
                    bridgeConnection =
                        TryCaptureReadyBridgeConnection(out var capturedBridge)
                            ? capturedBridge
                            : null;
                    return _interactions.ObserveAuthoritativePlayerAction(
                        action);
                },
                async (network, peer, restraint, token) =>
                {
                    var delivered = await SendPeerControlUnderGateAsync(
                            network,
                            peer,
                            envelope.Type,
                            envelope.Payload,
                            envelope.Tick,
                            token)
                        .ConfigureAwait(false);
                    if (delivered)
                    {
                        _messageFlowDiagnostics.MarkDelivered(
                            MessageFlowDirection.BridgeToNetwork,
                            envelope.Type);
                    }
                    else
                    {
                        _messageFlowDiagnostics.MarkDropped(
                            MessageFlowDirection.BridgeToNetwork,
                            envelope.Type);
                        await _logger.WarningAsync(
                                "network.control.dropped",
                                "Could not deliver the local host PlayerAction to the peer generation captured before its restraint mutation.",
                                CreateControlDiagnosticsData(
                                    envelope,
                                    DescribeControlEnvelope(envelope),
                                    "bridge-to-network",
                                    delivered: false,
                                    ("peerGeneration", peer.Generation)),
                                token)
                            .ConfigureAwait(false);
                    }

                    if (restraint is { } restraintState)
                    {
                        await BroadcastLocalAuthoritativeControlUnderGateAsync(
                                network,
                                peer,
                                bridgeConnection,
                                MessageType.RestraintState,
                                BinaryPayloadCodec.EncodeRestraintState(
                                    restraintState),
                                token)
                            .ConfigureAwait(false);
                    }
                },
                cancellationToken)
            .ConfigureAwait(false);
    }

    // Guest forwards allowed local action intent to the host and waits for the later authoritative action/result instead of deciding it locally.
    // Check a guest action and broadcast the host-approved action plus any restraint state it produces.
    // Unlike ResolvePeerGuestPlayerActionAsync, this helper does not take explicit source-peer tokens itself.
    private async ValueTask ResolveGuestPlayerActionAsync(
        PlayerActionPayload intent,
        CancellationToken cancellationToken)
    {
        var evaluation = EvaluateGuestPlayerAction(intent);
        if (evaluation.Resolved is not { } resolved)
        {
            await LogRejectedGuestPlayerActionAsync(
                    intent,
                    evaluation.Rejection,
                    evaluation.ActorAgeMilliseconds,
                    evaluation.TargetAgeMilliseconds,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }
        if (!_playerActions.TryAuthorize(resolved, out var actionRejection))
        {
            await LogRejectedGuestPlayerActionAsync(
                    intent,
                    actionRejection,
                    evaluation.ActorAgeMilliseconds,
                    evaluation.TargetAgeMilliseconds,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        await BroadcastAuthoritativeControlAsync(
                MessageType.PlayerAction,
                BinaryPayloadCodec.EncodePlayerAction(resolved),
                cancellationToken)
            .ConfigureAwait(false);
        if (_interactions.ObserveAuthoritativePlayerAction(resolved) is
            { } restraint)
        {
            await BroadcastAuthoritativeControlAsync(
                    MessageType.RestraintState,
                    BinaryPayloadCodec.EncodeRestraintState(restraint),
                    cancellationToken)
                .ConfigureAwait(false);
        }
    }

    // Show one useful handshake error in the game menu without repeatedly showing the same rejection.
    // If the bridge cannot display it, allow a later rejection to try again.
    internal async ValueTask OnNetworkAuthenticationRejectedAsync(
        string reason,
        CancellationToken cancellationToken)
    {
        if (!_config.InGameMenuEnabled ||
            Interlocked.CompareExchange(
                ref _authenticationRejectionShown,
                1,
                0) != 0)
        {
            return;
        }

        var category = reason.Contains(
            "session-id",
            StringComparison.OrdinalIgnoreCase)
            ? "session-id"
            : "authentication";
        await _logger.WarningAsync(
            "session.authentication-rejected",
            "LAN handshake was rejected; restart guidance was sent to the game overlay.",
            new Dictionary<string, object?>
            {
                ["role"] = _config.Role.ToString(),
                ["category"] = category
            },
            cancellationToken: CancellationToken.None).ConfigureAwait(false);

        var message = _config.Role == SessionRole.Guest
            ? "Handshake rejected. Check IPv4 and enter exactly the password set by the host."
            : "Guest handshake rejected. Both players must enter the same session password.";
        try
        {
            await SendSessionStatusAsync(
                new SessionMenuStatusPayload(
                    SessionMenuStatusKind.Error,
                    message),
                cancellationToken).ConfigureAwait(false);
        }
        catch (IOException)
        {
            Interlocked.Exchange(ref _authenticationRejectionShown, 0);
            await _logger.WarningAsync(
                "session.authentication-overlay-unavailable",
                "Could not show handshake rejection because the game bridge is unavailable.",
                cancellationToken: CancellationToken.None).ConfigureAwait(false);
        }
    }

    // First entry point for messages from the local RDR2 Bridge.
    // It validates the pipe/session boundary before the message can enter LAN logic.
    // Receive a message from this PC's RDR2 bridge and handle connection or menu messages first.
    // For gameplay, verify the bridge connection, payload, and local role before recording durable events or forwarding the message.
    private async ValueTask OnBridgeEnvelopeAsync(
        ProtocolEnvelope envelope,
        BridgePipeConnectionToken receiveConnection,
        CancellationToken cancellationToken)
    {
        if (envelope.Type == MessageType.Goodbye)
        {
            var reason = DecodeGoodbyeReason(envelope.Payload.Span);
            await _logger.WarningAsync(
                "bridge.goodbye",
                $"Game bridge requested fail-closed shutdown: {reason}",
                cancellationToken: cancellationToken).ConfigureAwait(false);
            throw new BridgeShutdownException(reason);
        }

        // Hello is the local game's connection setup message, not a player-position update.
        if (envelope.Type == MessageType.Hello)
        {
            if (!envelope.Payload.IsEmpty)
            {
                throw new ProtocolException(
                    "Bridge IPC Hello payload must be empty.");
            }

            await using var authorityBoundary =
                await _bridgeSessionGenerationGate.EnterBoundaryAsync(
                        cancellationToken)
                    .ConfigureAwait(false);
            if (_config.InGameMenuEnabled && _network is null)
            {
                Volatile.Write(ref _bridgeReadyGeneration, 0);
                if (!_bridge.TryCaptureConnection(out var expectedConnection))
                {
                    throw new IOException(
                        "Bridge disconnected while processing Hello.");
                }
                await using var deliveryBarrier = await _networkBridgePump
                    .EnterDeliveryBarrierAsync(cancellationToken)
                    .ConfigureAwait(false);
                var rotatedConnection = await _bridge
                    .RotateConnectionTokenAsync(
                        expectedConnection,
                        cancellationToken)
                    .ConfigureAwait(false);
                if (rotatedConnection is null)
                {
                    throw new IOException(
                        "Bridge connection changed while processing Hello.");
                }
                _networkBridgePump.ClearPending();
                await SendSessionStatusAsync(
                        new SessionMenuStatusPayload(
                            SessionMenuStatusKind.Waiting,
                            "Start HOST or JOIN from the launcher and enter the session password."),
                        cancellationToken)
                    .ConfigureAwait(false);
                return;
            }

            await AcknowledgeAndReplayBridgeStateUnderBoundaryAsync(
                    cancellationToken)
                .ConfigureAwait(false);
            if (_config.InGameMenuEnabled)
            {
                await SendSessionStatusAsync(
                    new SessionMenuStatusPayload(
                        _config.Role == SessionRole.Host
                            ? SessionMenuStatusKind.ReadyHost
                            : SessionMenuStatusKind.ReadyGuest,
                        _config.Role == SessionRole.Host
                            ? $"HOST {_config.HostAddress}: code ready; waiting for the guest."
                            : "Code accepted; handshake in progress. Wait for REMOTE STREAMING.",
                        _activeInviteCode),
                    cancellationToken).ConfigureAwait(false);
            }
            return;
        }

        if (envelope.Type == MessageType.SessionMenuRequest)
        {
            await ActivateSessionFromMenuAsync(
                SessionMenuPayloadCodec.DecodeRequest(envelope.Payload.Span),
                cancellationToken).ConfigureAwait(false);
            return;
        }

        // The receive token is captured before BridgePipeServer begins reading the frame.
        // Holding this gate through every bridge-originated mutation makes a logical rotation atomic with cache/entity clearing: either the old handler finishes first and the boundary clears after it, or the boundary wins and this exact old generation is rejected here.
        // A ready connection token prevents a delayed local message from crossing a host, guest, or reconnect boundary.
        await using var inboundAuthority =
            await _bridgeSessionGenerationGate.TryEnterInboundAsync(
                    receiveConnection,
                    IsReadyBridgeConnection,
                    cancellationToken)
                .ConfigureAwait(false);
        if (inboundAuthority is null)
        {
            _messageFlowDiagnostics.MarkDropped(
                MessageFlowDirection.BridgeToNetwork,
                envelope.Type);
            return;
        }

        // First reject malformed data, then separately decide whether the local role is allowed to produce that message.
        ValidateBinaryControlPayload(envelope);

        if (envelope.Type == MessageType.CampaignCapability &&
            !IsSupportedCampaignCapability(
                BinaryPayloadCodec.DecodeCampaignCapability(
                    envelope.Payload.Span)))
        {
            _messageFlowDiagnostics.MarkDropped(
                MessageFlowDirection.BridgeToNetwork,
                envelope.Type);
            await _logger.WarningAsync(
                "campaign.capability-unknown-record",
                "Dropped an unverified campaign capability record; unknown records are never replayed or applied.",
                CreateCapabilityDiagnosticsData(envelope, "bridge-to-network"),
                cancellationToken).ConfigureAwait(false);
            return;
        }

        if (!IsLocalBridgeEnvelopeAuthorized(_config.Role, envelope))
        {
            await _logger.WarningAsync(
                "bridge.authority-rejected",
                $"Rejected {envelope.Type} emitted by the non-authoritative local role.",
                new Dictionary<string, object?>
                {
                    ["messageType"] = envelope.Type.ToString(),
                    ["localRole"] = _config.Role.ToString(),
                    ["direction"] = "bridge-to-network"
                },
                cancellationToken: cancellationToken).ConfigureAwait(false);
            return;
        }

        if (_config.Role == SessionRole.Host &&
            envelope.Type == MessageType.CampaignCapability)
        {
            var capability = BinaryPayloadCodec.DecodeCampaignCapability(
                envelope.Payload.Span);
            var recorded = _capabilityJournal.Record(new CapabilityGrant(
                $"{capability.Kind}:0x{capability.RecordHash:X8}",
                (CapabilityKind)capability.Kind,
                capability.RecordHash,
                capability.HostEventId,
                capability.GrantedAtUnixMilliseconds));
            if (recorded)
            {
                await PersistCapabilityJournalAsync(cancellationToken)
                    .ConfigureAwait(false);
            }
        }
        if (_config.Role == SessionRole.Host &&
            envelope.Type == MessageType.MissionProgression)
        {
            var progression = BinaryPayloadCodec.DecodeMissionProgression(
                envelope.Payload.Span);
            if (progression.Phase == MissionProgressionPhase.Completion &&
                progression.Flags ==
                    MissionProgressionFlags.VerifiedCompletionMapping &&
                _missionProgressionJournal.Record(
                    progression,
                    DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()))
            {
                // Commit the retry record before attempting network delivery.
                await PersistMissionProgressionJournalAsync(cancellationToken)
                    .ConfigureAwait(false);
            }
        }
        if (_config.Role == SessionRole.Host &&
            envelope.Type == MessageType.PickupCollected)
        {
            var pickup = BinaryPayloadCodec.DecodePickupCollected(envelope.Payload.Span);
            var claim = _pickupClaims.ClaimLoot(pickup.ActorEntityId,
                new MapLoot($"pickup:{pickup.PickupHash:X8}:{pickup.CollectionId:X16}"));
            await PersistPickupClaimsAsync(cancellationToken).ConfigureAwait(false);
            await _logger.InfoAsync("pickup.claimed",
                $"Recorded native pickup claim ({claim.Status}); no private inventory copied.",
                new Dictionary<string, object?> { ["actorEntityId"] = pickup.ActorEntityId.Value, ["pickupHash"] = $"0x{pickup.PickupHash:X8}", ["collectionId"] = pickup.CollectionId },
                cancellationToken).ConfigureAwait(false);
        }

        // Wait behind any host replay before changing its cached mission or entity list, so the replay is not assembled from mixed states.
        if (ShouldSerializeHostReplayStateMutation(envelope.Type))
        {
            _ = await _peerControlSendGate.RunAsync(
                    async token =>
                    {
                        await ProcessAuthorizedBridgeEnvelopeAsync(
                                envelope,
                                controlSendGateHeld: true,
                                token)
                            .ConfigureAwait(false);
                        return true;
                    },
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        await ProcessAuthorizedBridgeEnvelopeAsync(
                envelope,
                controlSendGateHeld: false,
                cancellationToken)
            .ConfigureAwait(false);
    }

    // Routes a permitted local Bridge message: cache host state, send it to the peer, answer it locally, or deliberately keep only its latest snapshot.
    // Process a local game message after its initial payload and authority checks have passed.
    // Update relevant caches, resolve host-owned actions, and choose snapshot or reliable control delivery to the peer.
    private async ValueTask ProcessAuthorizedBridgeEnvelopeAsync(
        ProtocolEnvelope envelope,
        bool controlSendGateHeld,
        CancellationToken cancellationToken)
    {

        if (_config.Role == SessionRole.Host &&
            envelope.Type == MessageType.MissionState)
        {
            var update = _missionStateCache.Apply(envelope);
            if (update.Disposition == MissionStateCacheDisposition.Stale)
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.BridgeToNetwork,
                    envelope.Type);
                return;
            }
        }

        if (_config.Role == SessionRole.Host &&
            envelope.Type == MessageType.MissionCinematicState)
        {
            var update = _missionCinematicStateCache.Apply(envelope);
            _ = _animSceneDefinitionCache.ClearTerminal(update.State);
            if (update.Disposition ==
                MissionCinematicStateCacheDisposition.Stale)
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.BridgeToNetwork,
                    envelope.Type);
                return;
            }
        }

        AnimSceneDefinitionCacheUpdate? animSceneDefinitionUpdate = null;
        if (_config.Role == SessionRole.Host &&
            envelope.Type == MessageType.AnimSceneDefinition)
        {
            animSceneDefinitionUpdate = _animSceneDefinitionCache.Apply(envelope);
            if (animSceneDefinitionUpdate.Value.Disposition ==
                AnimSceneDefinitionCacheDisposition.Stale)
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.BridgeToNetwork,
                    envelope.Type);
                await _logger.InfoAsync(
                    "animscene-definition.tx-stale-dropped",
                    "Dropped a stale or duplicate host AnimScene definition before network delivery.",
                    CreateAnimSceneDefinitionDiagnosticsData(
                        envelope,
                        animSceneDefinitionUpdate.Value.Definition,
                        "bridge-to-network",
                        delivered: false,
                        ("cacheDisposition", "Stale")),
                    cancellationToken).ConfigureAwait(false);
                return;
            }
        }

        if (_config.Role == SessionRole.Host &&
            envelope.Type == MessageType.AnimSceneControl)
        {
            var control = BinaryPayloadCodec.DecodeAnimSceneControl(
                envelope.Payload.Span);
            if (control.Kind == AnimSceneControlKind.HostAbort)
            {
                _ = _animSceneDefinitionCache.ClearMatching(control);
            }
        }

        _messageFlowDiagnostics.Observe(
            MessageFlowDirection.BridgeToNetwork,
            envelope.Type);

        if (TryDecodeDiagnosticMarker(envelope, out _))
        {
            await WriteDiagnosticMarkerSnapshotAsync(
                    envelope,
                    "local-bridge-origin",
                    cancellationToken)
                .ConfigureAwait(false);
        }

        if (envelope.Type == MessageType.PlayerState)
        {
            _ = Interlocked.Increment(ref _bridgeToNetworkObserved);
            var state = BinaryPayloadCodec.DecodePlayerState(envelope.Payload.Span);
            if (state.Slot != (byte)_config.Role)
            {
                throw new ProtocolException(
                    "Local game bridge sent PlayerState for a different role.");
            }
            if ((state.Flags & PlayerStateFlags.OnlineModeDetected) != 0)
            {
                await RefuseOnlineModeAsync(cancellationToken).ConfigureAwait(false);
                throw new SafetyViolationException(
                    "Online mode was detected; the sidecar stopped forwarding game state.");
            }

            if (await ShouldDeferOutboundUntilMotionModeNegotiatedAsync(
                    envelope.Type,
                    cancellationToken).ConfigureAwait(false))
            {
                _ = Interlocked.Increment(ref _bridgeToNetworkDropped);
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.BridgeToNetwork,
                    envelope.Type);
                return;
            }

            // Remember our latest local player sample for later authority checks before trying to forward it to the peer.
            _ = _entities.ApplyPlayerState(envelope);
            _identityPublisher.ObservePlayerState(state);
            var network = _network;
            if (network is null)
            {
                _ = Interlocked.Increment(ref _bridgeToNetworkNoPeer);
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.BridgeToNetwork,
                    envelope.Type);
                return;
            }
            var peerConnected = network.IsConnected;
            // Player movement uses the snapshot path, where later samples replace the need to retransmit every old position.
            // A successful send here reports transport delivery status rather than remote game application.
            var delivered = await network.SendSnapshotAsync(
                envelope.Type,
                envelope.Payload,
                envelope.Tick,
                cancellationToken).ConfigureAwait(false);
            if (delivered)
            {
                _ = Interlocked.Increment(ref _bridgeToNetworkDelivered);
                _messageFlowDiagnostics.MarkDelivered(
                    MessageFlowDirection.BridgeToNetwork,
                    envelope.Type);
            }
            else if (!peerConnected)
            {
                _ = Interlocked.Increment(ref _bridgeToNetworkNoPeer);
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.BridgeToNetwork,
                    envelope.Type);
            }
            else
            {
                _ = Interlocked.Increment(ref _bridgeToNetworkDropped);
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.BridgeToNetwork,
                    envelope.Type);
            }
            await PublishLocalIdentityAsync(
                network,
                envelope.Tick,
                force: false,
                cancellationToken).ConfigureAwait(false);
            return;
        }

        if (envelope.Type == MessageType.InteractionIntent &&
            _config.Role == SessionRole.Host)
        {
            var processed = await ResolveLocalHostInteractionAsync(
                    BinaryPayloadCodec.DecodeInteractionIntent(
                        envelope.Payload.Span),
                    cancellationToken)
                .ConfigureAwait(false);
            if (!processed)
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.BridgeToNetwork,
                    envelope.Type);
            }
            return;
        }

        if (_config.Role == SessionRole.Host &&
            envelope.Type == MessageType.PlayerAction)
        {
            var processed = await ProcessLocalHostPlayerActionAsync(
                    envelope,
                    cancellationToken)
                .ConfigureAwait(false);
            if (!processed)
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.BridgeToNetwork,
                    envelope.Type);
            }
            return;
        }

        if (_config.Role == SessionRole.Host &&
            envelope.Type is MessageType.EntitySpawn or
                MessageType.EntityUpdate or
                MessageType.EntityDespawn)
        {
            // Keep a Sidecar copy of the host's shared NPC list so reconnect replay can rebuild the guest's world.
            _ = _worldGraph.Apply(envelope);
        }

        if (envelope.Type == MessageType.PlayerMountState)
        {
            var mount = BinaryPayloadCodec.DecodePlayerMountState(
                envelope.Payload.Span);
            if (mount.Slot != (byte)_config.Role)
            {
                throw new ProtocolException(
                    "Local game bridge sent mount state for a different role.");
            }
        }
        else if (envelope.Type == MessageType.PlayerTraversal)
        {
            var traversal = BinaryPayloadCodec.DecodePlayerTraversal(
                envelope.Payload.Span);
            if (traversal.Slot != (byte)_config.Role)
            {
                throw new ProtocolException(
                    "Local game bridge sent traversal for a different role.");
            }
        }
        else if (envelope.Type == MessageType.PlayerAnimationState)
        {
            var animation =
                AnimationReplicationPayloadCodec.DecodePlayerAnimationState(
                    envelope.Payload.Span);
            if (animation.Slot != (byte)_config.Role)
            {
                throw new ProtocolException(
                    "Local game bridge sent PlayerAnimationState for a different role.");
            }
            if (_config.MotionReplicationMode !=
                MotionReplicationMode.AnimGraphReplica)
            {
                if (Interlocked.CompareExchange(
                        ref _localAnimationStateUnexpectedShown,
                        1,
                        0) == 0)
                {
                    await _logger.WarningAsync(
                        "bridge.motion-mode.unexpected-animation-state",
                        "Dropped PlayerAnimationState because this endpoint selected Task/Navmesh Puppet.",
                        new Dictionary<string, object?>
                        {
                            ["configuredMode"] =
                                _config.MotionReplicationMode.ToString(),
                            ["slot"] = animation.Slot
                        },
                        cancellationToken: cancellationToken)
                        .ConfigureAwait(false);
                }
                return;
            }
        }
        else if (envelope.Type == MessageType.PlayerAppearanceState)
        {
            var appearance = BinaryPayloadCodec.DecodePlayerAppearanceState(
                envelope.Payload.Span);
            if (appearance.Slot != (byte)_config.Role)
            {
                throw new ProtocolException(
                    "Local game bridge sent PlayerAppearanceState for a different role.");
            }
        }

        // Choose the fast snapshot route for these frequently changing samples and the reliable control route for the remaining types.
        // The MessageType enum only names message kinds and does not make this routing choice itself.
        var snapshot =
            envelope.Type is
                MessageType.EntityUpdate or
                MessageType.PlayerMountState or
                MessageType.PlayerAnimationState or
                MessageType.MissionCameraState or
                MessageType.AnimSceneReplicaState;
        var activeNetwork = _network;
        if (activeNetwork is null)
        {
            _messageFlowDiagnostics.MarkDropped(
                MessageFlowDirection.BridgeToNetwork,
                envelope.Type);
            return;
        }
        if (await ShouldDeferOutboundUntilMotionModeNegotiatedAsync(
                envelope.Type,
                cancellationToken).ConfigureAwait(false))
        {
            _messageFlowDiagnostics.MarkDropped(
                MessageFlowDirection.BridgeToNetwork,
                envelope.Type);
            return;
        }
        if (snapshot)
        {
            var delivered = await activeNetwork.SendSnapshotAsync(
                envelope.Type,
                envelope.Payload,
                envelope.Tick,
                cancellationToken).ConfigureAwait(false);
            if (delivered)
            {
                _messageFlowDiagnostics.MarkDelivered(
                    MessageFlowDirection.BridgeToNetwork,
                    envelope.Type);
            }
            else
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.BridgeToNetwork,
                    envelope.Type);
            }
        }
        else
        {
            var description = DescribeControlEnvelope(envelope);
            var missionState = envelope.Type == MessageType.MissionState
                ? BinaryPayloadCodec.DecodeMissionState(envelope.Payload.Span)
                : (MissionStatePayload?)null;
            var peerConnected = activeNetwork.IsConnected;
            var delivered = await SendPeerControlAsync(
                    activeNetwork,
                    envelope.Type,
                    envelope.Payload,
                    envelope.Tick,
                    controlSendGateHeld,
                    cancellationToken)
                .ConfigureAwait(false);
            if (delivered)
            {
                _messageFlowDiagnostics.MarkDelivered(
                    MessageFlowDirection.BridgeToNetwork,
                    envelope.Type);
                if (ShouldLogOutboundControlSuccess(
                        envelope,
                        missionState))
                {
                    await _logger.InfoAsync(
                        missionState.HasValue
                            ? "mission-tx.network-delivered"
                            : "network.control.delivered",
                        $"Delivered {description} from the game bridge to the authenticated peer.",
                        missionState.HasValue
                            ? CreateMissionStateDiagnosticsData(
                                envelope,
                                missionState.Value,
                                ("control", description),
                                ("direction", "bridge-to-network"),
                                ("delivered", true))
                            : CreateControlDiagnosticsData(
                                envelope,
                                description,
                                "bridge-to-network",
                                delivered: true),
                        cancellationToken).ConfigureAwait(false);
                }
            }
            else
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.BridgeToNetwork,
                    envelope.Type);
                await _logger.WarningAsync(
                    missionState.HasValue
                        ? "mission-tx.network-dropped"
                        : "network.control.dropped",
                    peerConnected
                        ? $"Could not deliver {description} to the authenticated peer."
                        : $"Dropped {description}: no authenticated peer is connected.",
                    missionState.HasValue
                        ? CreateMissionStateDiagnosticsData(
                            envelope,
                            missionState.Value,
                            ("control", description),
                            ("direction", "bridge-to-network"),
                            ("delivered", false),
                            ("peerConnected", peerConnected))
                        : CreateControlDiagnosticsData(
                            envelope,
                            description,
                            "bridge-to-network",
                            delivered: false,
                            ("peerConnected", peerConnected)),
                        cancellationToken).ConfigureAwait(false);
            }
        }

    }

    // First entry point for messages from the other PC.
    // It checks the active peer/session, then validates, authorizes, caches, or forwards the frame.
    // Receive a message from the peer, check its current connection and role, and update the appropriate local cache.
    // Resolve guest requests on the host or queue approved state for the local game instead of changing RDR2 entities directly here.
    private async ValueTask OnNetworkEnvelopeAsync(
        ProtocolEnvelope envelope,
        ControlPeerToken sourcePeer,
        CancellationToken cancellationToken)
    {
        // A network message can arrive after reconnecting.
        // Check that it still belongs to the current player before changing anything.
        var sourceNetwork = _network;
        if (sourceNetwork is null ||
            !ReferenceEquals(_network, sourceNetwork) ||
            !sourceNetwork.IsControlPeerCurrent(sourcePeer))
        {
            _messageFlowDiagnostics.MarkDropped(
                MessageFlowDirection.NetworkToBridge,
                envelope.Type);
            return;
        }

        bool IsSourcePeerCurrent() =>
            ReferenceEquals(_network, sourceNetwork) &&
            sourceNetwork.IsControlPeerCurrent(sourcePeer);

        // Recheck the peer while changing shared state so a disconnect or replacement cannot slip between the check and the change.
        bool TryRunSourcePeerMutation(Action mutation)
        {
            var applied = false;
            if (!sourceNetwork.TryRunForControlPeer(sourcePeer, () =>
            {
                if (!ReferenceEquals(_network, sourceNetwork))
                {
                    return;
                }

                mutation();
                applied = true;
            }))
            {
                return false;
            }

            return applied;
        }

        if (envelope.Type == MessageType.Goodbye)
        {
            await _logger.InfoAsync(
                "session.peer-stop-requested",
                "The authenticated peer stopped the coop session; returning to the HOST/JOIN menu.",
                cancellationToken: CancellationToken.None).ConfigureAwait(false);
            if (!ReferenceEquals(_network, sourceNetwork) ||
                !sourceNetwork.IsControlPeerCurrent(sourcePeer))
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.NetworkToBridge,
                    envelope.Type);
                return;
            }
            NetworkSessionRun? session;
            lock (_networkSessionSync)
            {
                session = _activeNetworkSession;
            }
            if (session is not null &&
                ReferenceEquals(session.Network, sourceNetwork))
            {
                session.Stop.Cancel();
            }
            return;
        }
        if (envelope.Type == MessageType.MotionReplicationConfig)
        {
            await HandlePeerMotionModeAnnouncementAsync(
                    envelope,
                    sourceNetwork,
                    sourcePeer,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        if (envelope.Type is MessageType.SessionMenuRequest or
            MessageType.SessionMenuStatus)
        {
            await _logger.WarningAsync(
                "network.local-ipc-message-rejected",
                $"Rejected local-only {envelope.Type} received from the LAN peer.",
                cancellationToken: cancellationToken).ConfigureAwait(false);
            return;
        }

        // Heartbeats have no gameplay side effect; they feed liveness and a best-effort remote clock diagnostic, then bypass bridge delivery.
        // Heartbeat timing is useful for diagnostics, but it is not a movement sample and does not move the remote player.
        // The wall-clock difference estimate can also include network delay and is not a precise clock synchronization result.
        if (envelope.Type == MessageType.Heartbeat)
        {
            Interlocked.Exchange(
                ref _lastPeerHeartbeatAtMs,
                Environment.TickCount64);
            if (!envelope.Payload.IsEmpty)
            {
                try
                {
                    var heartbeat =
                        PayloadJson.Deserialize<HeartbeatPayload>(
                            envelope.Payload.Span);
                    Interlocked.Exchange(
                        ref _lastPeerClockDeltaEstimateMs,
                        DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() -
                            heartbeat.UnixTimeMilliseconds);
                }
                catch (ProtocolException)
                {
                    if (Interlocked.CompareExchange(
                            ref _heartbeatDiagnosticsParseFailureShown,
                            1,
                            0) == 0)
                    {
                        await _logger.WarningAsync(
                            "diagnostics.heartbeat-clock-unavailable",
                            "The authenticated heartbeat arrived, but its optional clock sample could not be decoded.",
                            cancellationToken: cancellationToken)
                            .ConfigureAwait(false);
                    }
                }
            }
            return;
        }

        // From here every non-heartbeat frame is counted before it is deferred, rejected, cached, forwarded, or coalesced for diagnostics.
        _messageFlowDiagnostics.Observe(
            MessageFlowDirection.NetworkToBridge,
            envelope.Type);

        // Both PCs must use the same movement system.
        // Until they agree, only keep the guest reset message; drop other game messages.
        // Hold normal gameplay behind movement-mode agreement so the two games do not interpret motion using different systems.
        if (!IsMotionModeNegotiated(sourceNetwork, sourcePeer))
        {
            if (_config.Role == SessionRole.Guest &&
                envelope.Type == MessageType.ResyncRequest)
            {
                var disposition = default(
                    GuestReconnectResyncDeferDisposition);
                // Unlike ordinary pre-negotiation traffic, this guest reconnect reset is remembered so it can be completed later.
                if (!TryRunSourcePeerMutation(() =>
                    disposition = _guestReconnectResyncGate.Defer(envelope)))
                {
                    _messageFlowDiagnostics.MarkDropped(
                        MessageFlowDirection.NetworkToBridge,
                        envelope.Type);
                    return;
                }
                if (disposition ==
                    GuestReconnectResyncDeferDisposition.Duplicate)
                {
                    _messageFlowDiagnostics.MarkDropped(
                        MessageFlowDirection.NetworkToBridge,
                        envelope.Type);
                }
                await _logger.InfoAsync(
                    disposition ==
                        GuestReconnectResyncDeferDisposition.Stored
                        ? "network.motion-mode.pre-negotiation-resync-deferred"
                        : "network.motion-mode.pre-negotiation-resync-duplicate",
                    disposition ==
                        GuestReconnectResyncDeferDisposition.Stored
                        ? "Stored the validated guest-local reconnect reset until motion-mode negotiation completes."
                        : "Ignored a duplicate guest-local reconnect reset while one is already deferred.",
                    new Dictionary<string, object?>
                    {
                        ["messageType"] = envelope.Type.ToString(),
                        ["configuredMode"] =
                            _config.MotionReplicationMode.ToString(),
                        ["direction"] = "local-network-to-bridge",
                        ["sequence"] = envelope.Sequence,
                        ["disposition"] = disposition.ToString()
                    },
                    cancellationToken).ConfigureAwait(false);
                return;
            }

            if (Interlocked.CompareExchange(
                    ref _preNegotiationInboundDropShown,
                    1,
                    0) == 0)
            {
                var data = new Dictionary<string, object?>
                {
                    ["messageType"] = envelope.Type.ToString(),
                    ["configuredMode"] =
                        _config.MotionReplicationMode.ToString(),
                    ["direction"] = "network-to-bridge"
                };
                if (envelope.Type == MessageType.ResyncRequest)
                {
                    await _logger.InfoAsync(
                        "network.motion-mode.pre-negotiation-resync-deferred",
                        "Deferred the guest's automatic resync until motion-mode negotiation completes.",
                        data,
                        cancellationToken).ConfigureAwait(false);
                }
                else
                {
                    await _logger.WarningAsync(
                        "network.motion-mode.pre-negotiation-frame-dropped",
                        "Dropped authenticated peer traffic until motion-mode negotiation completes.",
                        data,
                        cancellationToken).ConfigureAwait(false);
                }
            }
            _messageFlowDiagnostics.MarkDropped(
                MessageFlowDirection.NetworkToBridge,
                envelope.Type);
            return;
        }

        // Decode/validate control payload shape before role authorization and cache mutation; this is the hard boundary for peer input bytes.
        ValidateBinaryControlPayload(envelope);

        if (envelope.Type == MessageType.CampaignCapability &&
            !IsSupportedCampaignCapability(
                BinaryPayloadCodec.DecodeCampaignCapability(
                    envelope.Payload.Span)))
        {
            _messageFlowDiagnostics.MarkDropped(
                MessageFlowDirection.NetworkToBridge,
                envelope.Type);
            await _logger.WarningAsync(
                "campaign.capability-unknown-record",
                "Dropped an unverified campaign capability record; unknown records are never forwarded to the game.",
                CreateCapabilityDiagnosticsData(envelope, "network-to-bridge"),
                cancellationToken).ConfigureAwait(false);
            return;
        }

        // The explicitly enabled solo-test view can feed guest-world samples back into the host process for inspection.
        // Normal LAN traffic still has to pass the peer-role checks.
        var loopbackGuestWorldEnvelope =
            _allowLoopbackGuestWorldView &&
            _config.Role == SessionRole.Host &&
            envelope.Type is MessageType.EntitySpawn or
                MessageType.EntityUpdate or
                MessageType.EntityDespawn;
        if (!loopbackGuestWorldEnvelope &&
            !IsPeerEnvelopeAuthorized(_config.Role, envelope))
        {
            _messageFlowDiagnostics.MarkDropped(
                MessageFlowDirection.NetworkToBridge,
                envelope.Type);
            await _logger.WarningAsync(
                "network.authority-rejected",
                $"Rejected non-authoritative {envelope.Type} from the authenticated peer.",
                new Dictionary<string, object?>
                {
                    ["messageType"] = envelope.Type.ToString(),
                    ["localRole"] = _config.Role.ToString(),
                    ["direction"] = "network-to-bridge"
                },
                cancellationToken).ConfigureAwait(false);
            return;
        }

        if (_config.Role == SessionRole.Host &&
            envelope.Type == MessageType.CampaignCapabilityAck)
        {
            var acknowledgement = BinaryPayloadCodec.DecodeCampaignCapabilityAck(
                envelope.Payload.Span);
            // Match the acknowledgement to a recorded grant before marking it saved, rather than trusting an arbitrary event ID.
            var matchingGrant = _capabilityJournal.CaptureState().SingleOrDefault(
                grant => grant.HostEventId == acknowledgement.HostEventId &&
                    grant.Kind == (CapabilityKind)acknowledgement.Kind &&
                    grant.RecordHash == acknowledgement.RecordHash);
            if (matchingGrant is null)
            {
                _messageFlowDiagnostics.MarkDropped(MessageFlowDirection.NetworkToBridge, envelope.Type);
                return;
            }
            if (_capabilityJournal.Acknowledge(acknowledgement.HostEventId,
                    DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()))
            {
                await PersistCapabilityJournalAsync(cancellationToken).ConfigureAwait(false);
            }
            return;
        }
        if (_config.Role == SessionRole.Host &&
            envelope.Type == MessageType.MissionProgression)
        {
            var progression = BinaryPayloadCodec.DecodeMissionProgression(
                envelope.Payload.Span);
            if (progression.Phase == MissionProgressionPhase.Applied &&
                _missionProgressionJournal.Acknowledge(
                    progression,
                    DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()))
            {
                await PersistMissionProgressionJournalAsync(cancellationToken)
                    .ConfigureAwait(false);
            }
        }
        if (_config.Role == SessionRole.Host &&
            envelope.Type == MessageType.PickupCollected)
        {
            var pickup = BinaryPayloadCodec.DecodePickupCollected(envelope.Payload.Span);
            var claim = _pickupClaims.ClaimLoot(pickup.ActorEntityId,
                new MapLoot($"pickup:{pickup.PickupHash:X8}:{pickup.CollectionId:X16}"));
            await PersistPickupClaimsAsync(cancellationToken).ConfigureAwait(false);
            await _logger.InfoAsync("pickup.claimed",
                $"Recorded peer native pickup claim ({claim.Status}); no private inventory copied.",
                new Dictionary<string, object?> { ["actorEntityId"] = pickup.ActorEntityId.Value, ["pickupHash"] = $"0x{pickup.PickupHash:X8}", ["collectionId"] = pickup.CollectionId },
                cancellationToken).ConfigureAwait(false);
            return;
        }

        if (TryDecodeDiagnosticMarker(envelope, out _))
        {
            await WriteDiagnosticMarkerSnapshotAsync(
                    envelope,
                    "authenticated-peer-received",
                    cancellationToken)
                .ConfigureAwait(false);
        }

        if (!IsSourcePeerCurrent())
        {
            _messageFlowDiagnostics.MarkDropped(
                MessageFlowDirection.NetworkToBridge,
                envelope.Type);
            return;
        }

        if (_config.Role == SessionRole.Host &&
            envelope.Type == MessageType.InteractionIntent)
        {
            var processed = await ResolvePeerInteractionAsync(
                    BinaryPayloadCodec.DecodeInteractionIntent(
                        envelope.Payload.Span),
                    sourceNetwork,
                    sourcePeer,
                    cancellationToken)
                .ConfigureAwait(false);
            if (!processed)
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.NetworkToBridge,
                    envelope.Type);
            }
            return;
        }

        if (_config.Role == SessionRole.Host &&
            envelope.Type == MessageType.PlayerAction)
        {
            var processed = await ResolvePeerGuestPlayerActionAsync(
                    BinaryPayloadCodec.DecodePlayerAction(
                        envelope.Payload.Span),
                    sourceNetwork,
                    sourcePeer,
                    cancellationToken)
                .ConfigureAwait(false);
            if (!processed)
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.NetworkToBridge,
                    envelope.Type);
            }
            return;
        }

        // Host world lifecycle is authoritative.
        // Cache it first so a later guest pipe reconnect can receive the same parent-first graph again.
        if (_config.Role == SessionRole.Guest &&
            envelope.Type == MessageType.RestraintState)
        {
            var restraint = BinaryPayloadCodec.DecodeRestraintState(
                envelope.Payload.Span);
            if (!TryRunSourcePeerMutation(() =>
                _ = _interactions.ApplyAuthoritativeRestraint(restraint)))
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.NetworkToBridge,
                    envelope.Type);
                return;
            }
        }

        if (_config.Role == SessionRole.Guest &&
            envelope.Type is MessageType.EntitySpawn or
                MessageType.EntityUpdate or
                MessageType.EntityDespawn)
        {
            if (!TryRunSourcePeerMutation(() =>
                _ = _worldGraph.Apply(envelope)))
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.NetworkToBridge,
                    envelope.Type);
                return;
            }
        }

        MissionStateCacheUpdate? missionStateUpdate = null;
        MissionCinematicStateCacheUpdate? cinematicStateUpdate = null;
        AnimSceneDefinitionCacheUpdate? animSceneDefinitionUpdate = null;
        // For a guest PlayerState received by the host, remember which actor needs mapping evidence after the pipe write succeeds.
        var remotePlayerMappingEntity = NetEntityId.None;
        // PlayerState is high-frequency latest-only state.
        // Reject a peer that claims the local role, then store accepted snapshots for smoothing.
        if (envelope.Type == MessageType.PlayerState)
        {
            _ = Interlocked.Increment(ref _networkToBridgeObserved);
            var playerState = BinaryPayloadCodec.DecodePlayerState(
                envelope.Payload.Span);
            // The peer reports its own player, so a payload claiming our local slot is invalid.
            if (playerState.Slot == (byte)_config.Role)
            {
                throw new ProtocolException(
                    "Authenticated peer sent PlayerState for the local role.");
            }
            if (!TryRunSourcePeerMutation(() =>
                _ = _entities.ApplyPlayerState(envelope)))
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.NetworkToBridge,
                    envelope.Type);
                return;
            }
            if (_config.Role == SessionRole.Host)
            {
                remotePlayerMappingEntity = playerState.EntityId;
            }
        }
        else if (envelope.Type == MessageType.PlayerAnimationState)
        {
            var animation =
                AnimationReplicationPayloadCodec.DecodePlayerAnimationState(
                    envelope.Payload.Span);
            if (animation.Slot == (byte)_config.Role)
            {
                throw new ProtocolException(
                    "Authenticated peer sent PlayerAnimationState for the local role.");
            }
            if (_config.MotionReplicationMode !=
                MotionReplicationMode.AnimGraphReplica)
            {
                await FailMotionModeNegotiationForPeerAsync(
                        "Peer emitted AnimGraph state after negotiating Task/Navmesh Puppet.",
                        peerMode: MotionReplicationWireMode.AnimGraphReplica,
                        peerRevision: 0,
                        sourceNetwork,
                        sourcePeer,
                        cancellationToken)
                    .ConfigureAwait(false);
                return;
            }

            if (Interlocked.CompareExchange(
                    ref _peerAnimationStateObserved,
                    1,
                    0) == 0)
            {
                await _logger.InfoAsync(
                    "network.motion-mode.animgraph-peer-observed",
                    "Received the first validated AnimGraph sample from the authenticated peer.",
                    new Dictionary<string, object?>
                    {
                        ["configuredMode"] =
                            _config.MotionReplicationMode.ToString(),
                        ["peerAnimationCapabilities"] =
                            animation.Capabilities.ToString(),
                        ["peerSlot"] = animation.Slot,
                        ["locomotionEpoch"] = animation.LocomotionEpoch
                    },
                    cancellationToken: cancellationToken)
                    .ConfigureAwait(false);
            }
        }
        else if (envelope.Type == MessageType.PlayerMountState)
        {
            var mount = BinaryPayloadCodec.DecodePlayerMountState(
                envelope.Payload.Span);
            if (mount.Slot == (byte)_config.Role)
            {
                throw new ProtocolException(
                    "Authenticated peer sent mount state for the local role.");
            }
        }
        else if (envelope.Type == MessageType.MissionState)
        {
            if (!TryRunSourcePeerMutation(() =>
                missionStateUpdate = _missionStateCache.Apply(envelope)))
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.NetworkToBridge,
                    envelope.Type);
                return;
            }
            var appliedMissionState = missionStateUpdate ??
                throw new InvalidOperationException(
                    "Current peer mission mutation did not produce a cache update.");
            if (appliedMissionState.Disposition ==
                MissionStateCacheDisposition.Stale)
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.NetworkToBridge,
                    envelope.Type);
                await _logger.InfoAsync(
                    "mission-rx.stale-dropped",
                    "Dropped a stale or duplicate host-authoritative mission state before bridge delivery.",
                    CreateMissionStateDiagnosticsData(
                        envelope,
                        appliedMissionState.State,
                        ("cacheDisposition", "Stale"),
                        ("direction", "network-to-bridge")),
                    cancellationToken).ConfigureAwait(false);
                return;
            }
        }
        else if (envelope.Type == MessageType.MissionCinematicState)
        {
            if (!TryRunSourcePeerMutation(() =>
            {
                cinematicStateUpdate =
                    _missionCinematicStateCache.Apply(envelope);
                _ = _animSceneDefinitionCache.ClearTerminal(
                    cinematicStateUpdate.Value.State);
            }))
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.NetworkToBridge,
                    envelope.Type);
                return;
            }
            var appliedCinematicState = cinematicStateUpdate ??
                throw new InvalidOperationException(
                    "Current peer cinematic mutation did not produce a cache update.");
            if (appliedCinematicState.Disposition ==
                MissionCinematicStateCacheDisposition.Stale)
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.NetworkToBridge,
                    envelope.Type);
                await _logger.InfoAsync(
                    "mission-cinematic-rx.stale-dropped",
                    "Dropped a stale or duplicate cinematic FSM state before bridge delivery.",
                    CreateControlDiagnosticsData(
                        envelope,
                        DescribeControlEnvelope(envelope),
                        "network-to-bridge",
                        delivered: false,
                        ("cacheDisposition", "Stale")),
                    cancellationToken).ConfigureAwait(false);
                return;
            }
        }
        else if (envelope.Type == MessageType.AnimSceneDefinition)
        {
            if (!TryRunSourcePeerMutation(() =>
                animSceneDefinitionUpdate =
                    _animSceneDefinitionCache.Apply(envelope)))
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.NetworkToBridge,
                    envelope.Type);
                return;
            }
            var appliedAnimSceneDefinition = animSceneDefinitionUpdate ??
                throw new InvalidOperationException(
                    "Current peer AnimScene mutation did not produce a cache update.");
            if (appliedAnimSceneDefinition.Disposition ==
                AnimSceneDefinitionCacheDisposition.Stale)
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.NetworkToBridge,
                    envelope.Type);
                await _logger.InfoAsync(
                    "animscene-definition.rx-stale-dropped",
                    "Dropped a stale or duplicate host AnimScene definition before bridge delivery.",
                    CreateAnimSceneDefinitionDiagnosticsData(
                        envelope,
                        appliedAnimSceneDefinition.Definition,
                        "network-to-bridge",
                        delivered: false,
                        ("cacheDisposition", "Stale")),
                    cancellationToken).ConfigureAwait(false);
                return;
            }
        }
        else if (envelope.Type == MessageType.AnimSceneControl)
        {
            var control = BinaryPayloadCodec.DecodeAnimSceneControl(
                envelope.Payload.Span);
            if (_config.Role == SessionRole.Guest &&
                control.Kind == AnimSceneControlKind.HostAbort)
            {
                if (!TryRunSourcePeerMutation(() =>
                    _ = _animSceneDefinitionCache.ClearMatching(control)))
                {
                    _messageFlowDiagnostics.MarkDropped(
                        MessageFlowDirection.NetworkToBridge,
                        envelope.Type);
                    return;
                }
            }
        }
        else if (envelope.Type == MessageType.PlayerTraversal)
        {
            var traversal = BinaryPayloadCodec.DecodePlayerTraversal(
                envelope.Payload.Span);
            if (traversal.Slot == (byte)_config.Role)
            {
                throw new ProtocolException(
                    "Authenticated peer sent traversal for the local role.");
            }
        }
        else if (envelope.Type == MessageType.PlayerIdentity)
        {
            var identity = BinaryPayloadCodec.DecodePlayerIdentity(
                envelope.Payload.Span);
            if (identity.Slot == (byte)_config.Role)
            {
                throw new ProtocolException(
                    "Authenticated peer sent identity for the local player slot.");
            }
            if (!TryRunSourcePeerMutation(() => ReportLobbyStatus(
                    "identity",
                    nickname: identity.Nickname)))
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.NetworkToBridge,
                    envelope.Type);
                return;
            }
        }
        else if (envelope.Type == MessageType.PlayerAppearanceState)
        {
            var appearance = BinaryPayloadCodec.DecodePlayerAppearanceState(
                envelope.Payload.Span);
            if (appearance.Slot == (byte)_config.Role)
            {
                throw new ProtocolException(
                    "Authenticated peer sent appearance for the local player slot.");
            }
        }
        else if (envelope.Type == MessageType.EntityDespawn)
        {
            var despawn = BinaryPayloadCodec.DecodeEntityDespawn(
                envelope.Payload.Span);
            if (!TryRunSourcePeerMutation(() =>
                _entities.Remove(despawn.EntityId)))
            {
                _messageFlowDiagnostics.MarkDropped(
                    MessageFlowDirection.NetworkToBridge,
                    envelope.Type);
                return;
            }
        }
        // Resync has different work on each side: the guest clears its cached host state, while the host replays its saved authoritative state.
        else if (envelope.Type == MessageType.ResyncRequest)
        {
            if (_config.Role == SessionRole.Guest)
            {
                if (!TryRunSourcePeerMutation(
                        ClearGuestAuthoritativeStateForResync))
                {
                    _messageFlowDiagnostics.MarkDropped(
                        MessageFlowDirection.NetworkToBridge,
                        envelope.Type);
                    return;
                }
            }
            else if (_config.Role == SessionRole.Host)
            {
                await ReplayAuthoritativeStateToPeerAsync(
                        sourceNetwork,
                        sourcePeer,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
        }

        var controlDescription =
            envelope.Type is MessageType.PlayerState or
                MessageType.EntityUpdate or
                MessageType.PlayerMountState or
                MessageType.PlayerAnimationState or
                MessageType.MissionCameraState or
                MessageType.AnimSceneReplicaState
                ? null
                : DescribeControlEnvelope(envelope);
        if (controlDescription is not null &&
            ShouldLogInboundControlSuccess(
                envelope,
                missionStateUpdate))
        {
            await _logger.InfoAsync(
                missionStateUpdate.HasValue
                    ? "mission-rx.network-received"
                    : "network.control.received",
                $"Received {controlDescription} from the authenticated peer.",
                missionStateUpdate.HasValue
                    ? CreateMissionStateDiagnosticsData(
                        envelope,
                        missionStateUpdate.Value.State,
                        ("control", controlDescription),
                        ("direction", "network-to-bridge"),
                        ("cacheDisposition",
                            missionStateUpdate.Value.Disposition.ToString()))
                    : CreateControlDiagnosticsData(
                        envelope,
                        controlDescription,
                        "network-to-bridge",
                        delivered: null),
                cancellationToken).ConfigureAwait(false);
        }

        if (!IsSourcePeerCurrent())
        {
            _messageFlowDiagnostics.MarkDropped(
                MessageFlowDirection.NetworkToBridge,
                envelope.Type);
            return;
        }

        // Do not mark the host's remote actor mapping ready just because its PlayerState arrived over the network.
        // The callback runs only after successful delivery from the queue to the local game pipe.
        Action<ProtocolEnvelope>? afterBridgeDelivery = null;
        if (remotePlayerMappingEntity.IsValid)
        {
            afterBridgeDelivery = _ =>
                MarkRemoteBridgeMappingDelivered(
                    sourceNetwork,
                    sourcePeer,
                    remotePlayerMappingEntity);
        }
        else if (_config.Role == SessionRole.Host &&
                 envelope.Type == MessageType.ResyncRequest)
        {
            // A PlayerState that was already in-flight when Resync arrived may refresh the mapping first.
            // Clear only after the later Resync frame has physically crossed the pipe; a subsequently queued PlayerState may then establish the new replica mapping.
            afterBridgeDelivery = _ =>
                _remoteBridgeMapping.CompleteResync(
                    sourceNetwork,
                    sourcePeer);
        }

        // Queue the approved message without waiting for RDR2 to read it, and attach a check that its source peer is still current.
        var enqueue = _networkBridgePump.TryEnqueue(
            envelope,
            () => ReferenceEquals(_network, sourceNetwork) &&
                sourceNetwork.IsControlPeerCurrent(sourcePeer),
            afterBridgeDelivery);
        // Coalesced means this state shares an existing waiting entry that keeps the newest usable sample.
        // An older arriving sample may be ignored, and the counter does not mean both samples were delivered.
        if (enqueue.Disposition == NetworkBridgeEnqueueDisposition.Coalesced)
        {
            _messageFlowDiagnostics.MarkCoalesced(
                MessageFlowDirection.NetworkToBridge,
                envelope.Type);
        }
        // A full or stopping queue cannot accept this message, so record the drop instead of reporting game delivery.
        if (enqueue.Disposition == NetworkBridgeEnqueueDisposition.Rejected)
        {
            _messageFlowDiagnostics.MarkDropped(
                MessageFlowDirection.NetworkToBridge,
                envelope.Type);
            if (envelope.Type == MessageType.PlayerState)
            {
                _ = Interlocked.Increment(ref _networkToBridgeUnavailable);
                return;
            }

            var rejectionCount = Interlocked.Increment(
                ref _networkBridgeQueueRejectionEvents);
            if (rejectionCount > 3 &&
                (rejectionCount & (rejectionCount - 1)) != 0)
            {
                return;
            }

            await _logger.WarningAsync(
                missionStateUpdate.HasValue
                    ? "mission-rx.delivery-queue-rejected"
                    : "bridge.delivery-queue.rejected",
                $"Dropped {controlDescription ?? envelope.Type.ToString()}; " +
                "the bounded network-to-bridge queue is full or stopping.",
                missionStateUpdate.HasValue
                    ? CreateMissionStateDiagnosticsData(
                        envelope,
                        missionStateUpdate.Value.State,
                        ("control",
                            controlDescription ?? envelope.Type.ToString()),
                        ("direction", "network-to-bridge"),
                        ("backlog", enqueue.Backlog),
                        ("rejectionCount", rejectionCount),
                        ("bridgeReady", IsBridgeReady()))
                    : CreateControlDiagnosticsData(
                        envelope,
                        controlDescription ?? envelope.Type.ToString(),
                        "network-to-bridge",
                        delivered: false,
                        ("backlog", enqueue.Backlog),
                        ("rejectionCount", rejectionCount),
                        ("bridgeReady", IsBridgeReady())),
                cancellationToken: cancellationToken).ConfigureAwait(false);
        }
    }

    // Last hop to RDR2.
    // The delivery pump preserves critical message order but keeps only the newest high-rate snapshots to avoid pipe backlog/desync.
    // Write one queued network message to the currently ready local game pipe and update delivery diagnostics.
    // A true result means the pipe write succeeded, not that an NPC was visibly created or a gameplay action completed.
    private async ValueTask<bool> DeliverNetworkEnvelopeToBridgeAsync(
        ProtocolEnvelope envelope)
    {
        var controlDescription =
            envelope.Type is MessageType.PlayerState or
                MessageType.EntityUpdate or
                MessageType.PlayerMountState or
                MessageType.PlayerAnimationState or
                MessageType.MissionCameraState or
                MessageType.AnimSceneReplicaState
                ? null
                : DescribeControlEnvelope(envelope);
        // Capture the ready pipe generation at dispatch time.
        // A game reload makes an old queued envelope fail delivery instead of crossing into the newly created bridge session.
        var delivered =
            TryCaptureReadyBridgeConnection(out var bridgeConnection) &&
            await _bridge.SendAsync(
                    bridgeConnection,
                    envelope,
                    CancellationToken.None)
                .ConfigureAwait(false);
        if (!delivered)
        {
            _messageFlowDiagnostics.MarkDropped(
                MessageFlowDirection.NetworkToBridge,
                envelope.Type);
            if (envelope.Type == MessageType.PlayerState)
            {
                _ = Interlocked.Increment(ref _networkToBridgeUnavailable);
                return false;
            }

            var missionState = envelope.Type == MessageType.MissionState
                ? BinaryPayloadCodec.DecodeMissionState(envelope.Payload.Span)
                : (MissionStatePayload?)null;
            await _logger.WarningAsync(
                controlDescription is null
                    ? "bridge.unavailable"
                    : missionState.HasValue
                        ? "mission-rx.bridge-dropped"
                        : "bridge.control.dropped",
                $"Dropped {controlDescription ?? envelope.Type.ToString()}; game bridge is not connected.",
                controlDescription is null
                    ? null
                    : missionState.HasValue
                        ? CreateMissionStateDiagnosticsData(
                            envelope,
                            missionState.Value,
                            ("control", controlDescription),
                            ("direction", "network-to-bridge"),
                            ("delivered", false),
                            ("bridgeReady", IsBridgeReady()))
                        : CreateControlDiagnosticsData(
                            envelope,
                            controlDescription,
                            "network-to-bridge",
                            delivered: false,
                            ("bridgeReady", IsBridgeReady())),
                cancellationToken: CancellationToken.None).ConfigureAwait(false);
            return false;
        }

        _messageFlowDiagnostics.MarkDelivered(
            MessageFlowDirection.NetworkToBridge,
            envelope.Type);

        // Count high-rate snapshots separately; they are intentionally silent in normal logs but still appear in streaming-gap diagnostics.
        if (envelope.Type == MessageType.PlayerState)
        {
            _ = Interlocked.Increment(ref _networkToBridgeDelivered);
        }
        else if (controlDescription is not null &&
                 ShouldLogBridgeDeliverySuccess(envelope))
        {
            var missionState = envelope.Type == MessageType.MissionState
                ? BinaryPayloadCodec.DecodeMissionState(envelope.Payload.Span)
                : (MissionStatePayload?)null;
            await _logger.InfoAsync(
                missionState.HasValue
                    ? "mission-rx.bridge-delivered"
                    : "bridge.control.delivered",
                $"Delivered {controlDescription} from the authenticated peer to the game bridge.",
                missionState.HasValue
                    ? CreateMissionStateDiagnosticsData(
                        envelope,
                        missionState.Value,
                        ("control", controlDescription),
                        ("direction", "network-to-bridge"),
                        ("delivered", true))
                    : CreateControlDiagnosticsData(
                        envelope,
                        controlDescription,
                        "network-to-bridge",
                        delivered: true),
                cancellationToken: CancellationToken.None).ConfigureAwait(false);
        }
        BridgeEnvelopeDelivered?.Invoke(envelope);
        return true;
    }

    // Record a local game-pipe failure separately from LAN errors so diagnostics show which connection failed.
    private ValueTask OnBridgeFaultedAsync(Exception exception) =>
        _logger.WarningAsync(
            "bridge.connection-fault",
            exception.Message,
            cancellationToken: CancellationToken.None);

    // Record that RDR2 connected, but leave gameplay readiness false until role and movement configuration are sent.
    private ValueTask OnBridgeOpenedAsync()
    {
        Volatile.Write(ref _bridgePipeConnected, 1);
        Volatile.Write(ref _bridgeReadyGeneration, 0);
        _remoteBridgeMapping.Clear();
        ReportLobbyStatus("bridge", connected: true);
        return _logger.InfoAsync(
            "bridge.pipe-connected",
            "RDR2 process connected to the bridge pipe; waiting for role negotiation.",
            cancellationToken: CancellationToken.None);
    }

    // Clear readiness and remembered guest-actor delivery when the local game connection closes.
    private ValueTask OnBridgeClosedAsync()
    {
        Volatile.Write(ref _bridgePipeConnected, 0);
        Volatile.Write(ref _bridgeReadyGeneration, 0);
        _remoteBridgeMapping.Clear();
        ReportLobbyStatus("bridge", connected: false);
        return _logger.InfoAsync(
            "bridge.disconnected",
            "RDR2 game bridge disconnected from the sidecar.",
            cancellationToken: CancellationToken.None);
    }

    // Handle a peer connecting or disconnecting only if the callback belongs to the active network session.
    // Disconnect releases remembered restraints and agreement state, while connect starts fresh movement-mode negotiation.
    private async ValueTask OnNetworkConnectionChangedAsync(
        ILanSession sourceNetwork,
        bool connected,
        CancellationToken cancellationToken)
    {
        if (!ReferenceEquals(_network, sourceNetwork))
        {
            return;
        }
        ReportLobbyStatus(
            "peer",
            connected,
            sourceNetwork.RemoteAddress?.ToString());
        if (!connected)
        {
            var clearedCurrentSession = await _peerControlSendGate.RunAsync(
                    async token =>
                    {
                        if (!ReferenceEquals(_network, sourceNetwork))
                        {
                            return false;
                        }
                        _remoteBridgeMapping.Clear();
                        // Release remembered restraint effects locally so a departing peer does not leave the player stuck in a restraint.
                        var releasedRestraints = _interactions.Clear(
                            emitFreeStates: true);
                        foreach (var restraint in releasedRestraints)
                        {
                            var cleanupEnvelope = CreateBridgeEnvelope(
                                MessageType.RestraintState,
                                BinaryPayloadCodec.EncodeRestraintState(
                                    restraint));
                            var delivered =
                                TryCaptureReadyBridgeConnection(
                                    out var bridgeConnection) &&
                                await _bridge.SendAsync(
                                        bridgeConnection,
                                        cleanupEnvelope,
                                        CancellationToken.None)
                                    .ConfigureAwait(false);
                            if (delivered)
                            {
                                BridgeEnvelopeDelivered?.Invoke(
                                    cleanupEnvelope);
                            }
                        }
                        return true;
                    },
                    CancellationToken.None)
                .ConfigureAwait(false);
            if (!clearedCurrentSession ||
                !ReferenceEquals(_network, sourceNetwork))
            {
                return;
            }
            Interlocked.Exchange(ref _peerAnimationStateObserved, 0);
            ResetMotionModeNegotiation();
            _guestReconnectResyncGate.Clear();
            Interlocked.Exchange(ref _preNegotiationInboundDropShown, 0);
            Interlocked.Exchange(ref _preNegotiationOutboundDropShown, 0);
            Interlocked.Exchange(ref _peerMotionModeNegotiationStartedAt, 0);
            Interlocked.Exchange(ref _peerStreamingStartedAt, 0);
            Interlocked.Exchange(ref _lastPeerHeartbeatAtMs, 0);
            Interlocked.Exchange(
                ref _lastPeerClockDeltaEstimateMs,
                long.MinValue);
            Interlocked.Exchange(
                ref _heartbeatDiagnosticsParseFailureShown,
                0);
            return;
        }

        if (!ReferenceEquals(_network, sourceNetwork))
        {
            return;
        }
        var network = sourceNetwork;
        if (!network.TryCaptureControlPeer(out var peer))
        {
            return;
        }

        Interlocked.Exchange(ref _peerAnimationStateObserved, 0);
        ResetMotionModeNegotiation();
        Interlocked.Exchange(ref _preNegotiationInboundDropShown, 0);
        Interlocked.Exchange(ref _preNegotiationOutboundDropShown, 0);
        Interlocked.Exchange(
            ref _peerMotionModeNegotiationStartedAt,
            Environment.TickCount64);

        var announcement = new MotionReplicationConfigPayload(
            AnimationReplicationPayloadCodec.MotionReplicationConfigSchemaVersion,
            ToWireMotionMode(_config.MotionReplicationMode),
            MotionReplicationConfigFlags.None,
            NextPeerMotionModeAnnouncementRevision());
        var delivered = await SendPeerControlAsync(
                network,
                peer,
                MessageType.MotionReplicationConfig,
                AnimationReplicationPayloadCodec.EncodeMotionReplicationConfig(
                    announcement),
                NetworkClock.Tick,
                cancellationToken)
            .ConfigureAwait(false);
        if (!delivered)
        {
            if (!ReferenceEquals(_network, network) ||
                !network.IsControlPeerCurrent(peer))
            {
                return;
            }
            await FailMotionModeNegotiationForPeerAsync(
                    "Could not deliver the local motion-mode announcement to the authenticated peer.",
                    peerMode: null,
                    peerRevision: 0,
                    network,
                    peer,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        await _logger.InfoAsync(
            "network.motion-mode.announcement-sent",
            "Announced the selected motion mode to the authenticated peer; streaming remains gated until the peer confirms the same mode.",
            new Dictionary<string, object?>
            {
                ["configuredMode"] =
                    _config.MotionReplicationMode.ToString(),
                ["wireMode"] = announcement.Mode.ToString(),
                ["revision"] = announcement.Revision,
                ["protocolVersion"] = ProtocolConstants.Version,
                ["peerModeNegotiated"] = false
            },
            cancellationToken: cancellationToken).ConfigureAwait(false);
    }

    // Print a small status record for the surrounding UI to read, such as peer connection state or nickname.
    // A reporting failure is ignored so it cannot stop multiplayer.
    private static void ReportLobbyStatus(
        string eventName,
        bool? connected = null,
        string? address = null,
        string? nickname = null)
    {
        try
        {
            var payload = JsonSerializer.Serialize(new
            {
                @event = eventName,
                connected,
                address,
                nickname
            });
            Console.WriteLine("COOP_LOBBY_STATUS=" + payload);
        }
        catch
        {
            // The launcher lobby is informational and cannot affect sync.
        }
    }

    // Require the peer to use the same movement replication mode and reject invalid or incompatible announcements.
    // Once agreement is recorded, publish our identity and let the guest finish any deferred reconnect reset.
    private async ValueTask HandlePeerMotionModeAnnouncementAsync(
        ProtocolEnvelope envelope,
        ILanSession sourceNetwork,
        ControlPeerToken sourcePeer,
        CancellationToken cancellationToken)
    {
        MotionReplicationConfigPayload announcement;
        try
        {
            announcement =
                AnimationReplicationPayloadCodec.DecodeMotionReplicationConfig(
                    envelope.Payload.Span);
        }
        catch (ProtocolException exception)
        {
            await FailMotionModeNegotiationForPeerAsync(
                    $"Authenticated peer sent an invalid motion-mode announcement: {exception.Message}",
                    peerMode: null,
                    peerRevision: 0,
                    sourceNetwork,
                    sourcePeer,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        var localMode = ToWireMotionMode(_config.MotionReplicationMode);
        if (announcement.Mode != localMode ||
            announcement.Flags != MotionReplicationConfigFlags.None)
        {
            await FailMotionModeNegotiationForPeerAsync(
                    announcement.Mode != localMode
                        ? $"Motion-mode mismatch: local {localMode}, peer {announcement.Mode}."
                        : "Peer requested a motion-mode fallback, but this build keeps both engines separate.",
                    announcement.Mode,
                    announcement.Revision,
                    sourceNetwork,
                    sourcePeer,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        var mark = TryMarkMotionModeNegotiated(sourceNetwork, sourcePeer);
        if (mark == MotionNegotiationMarkDisposition.StalePeer)
        {
            _messageFlowDiagnostics.MarkDropped(
                MessageFlowDirection.NetworkToBridge,
                envelope.Type);
            return;
        }

        if (mark == MotionNegotiationMarkDisposition.Duplicate)
        {
            await _logger.InfoAsync(
                "network.motion-mode.duplicate-confirmation",
                "Ignored a duplicate compatible peer motion-mode announcement.",
                new Dictionary<string, object?>
                {
                    ["mode"] = announcement.Mode.ToString(),
                    ["revision"] = announcement.Revision
                },
                cancellationToken).ConfigureAwait(false);
            return;
        }

        Interlocked.Exchange(ref _peerMotionModeNegotiationStartedAt, 0);
        Interlocked.Exchange(
            ref _peerStreamingStartedAt,
            Environment.TickCount64);
        await _logger.InfoAsync(
            "network.motion-mode.negotiated",
            "Both authenticated endpoints selected the same motion mode; streaming is now enabled.",
            new Dictionary<string, object?>
            {
                ["mode"] = announcement.Mode.ToString(),
                ["peerRevision"] = announcement.Revision,
                ["fallbackAllowed"] = false,
                ["peerModeNegotiated"] = true
            },
            cancellationToken).ConfigureAwait(false);

        if (!IsMotionModeNegotiated(sourceNetwork, sourcePeer))
        {
            return;
        }
        var network = sourceNetwork;

        await PublishLocalIdentityAsync(
            network,
            sourcePeer,
            NetworkClock.Tick,
            force: true,
            cancellationToken).ConfigureAwait(false);
        if (_config.Role == SessionRole.Guest)
        {
            await ReplayDeferredGuestReconnectResyncAsync(
                    network,
                    sourcePeer,
                    cancellationToken)
                .ConfigureAwait(false);
        }
    }

    // Forget cached player, world, mission, and animation-scene state plus pending game deliveries before a guest rebuild.
    // This clears Sidecar's remembered state rather than directly deleting RDR2 NPCs.
    private void ClearGuestAuthoritativeStateForResync()
    {
        // On guest reconnect, forget old NPC/mission data before taking the fresh host copy.
        _entities.Clear();
        _worldGraph.Clear();
        _missionStateCache.Clear();
        _missionCinematicStateCache.Clear();
        _animSceneDefinitionCache.Clear();
        // Old queued updates must not be replayed after the guest has forgotten the world they belonged to.
        _networkBridgePump.ClearPending();
    }

    // Pause ordinary queued delivery while sending a deferred reset to the captured guest game connection.
    private async ValueTask<bool> DeliverDeferredGuestResyncToBridgeAsync(
        ProtocolEnvelope envelope,
        CancellationToken cancellationToken)
    {
        if (!TryCaptureReadyBridgeConnection(out var bridgeConnection))
        {
            return false;
        }

        // Exclude normal queued state while the reset frame crosses the pipe; otherwise a pre-reset snapshot could arrive after the clear command.
        await using var barrier = await _networkBridgePump
            .EnterDeliveryBarrierAsync(cancellationToken)
            .ConfigureAwait(false);
        if (!IsReadyBridgeConnection(bridgeConnection))
        {
            return false;
        }

        return await DeliverDeferredGuestResyncToBridgeUnderBarrierAsync(
                bridgeConnection,
                envelope)
            .ConfigureAwait(false);
    }

    // Write the deferred reset while the caller already holds the delivery barrier and record whether the write succeeded.
    private async ValueTask<bool>
        DeliverDeferredGuestResyncToBridgeUnderBarrierAsync(
            BridgePipeConnectionToken bridgeConnection,
            ProtocolEnvelope envelope)
    {
        if (!IsReadyBridgeConnection(bridgeConnection))
        {
            return false;
        }
        // Once a protocol frame starts, cancellation must not leave a partial frame on a still-connected named pipe.
        // Connection abort or the bridge watchdog releases a stalled full-frame write.
        var delivered = await _bridge.SendAsync(
                bridgeConnection,
                envelope,
                CancellationToken.None)
            .ConfigureAwait(false);
        if (delivered)
        {
            _messageFlowDiagnostics.MarkDelivered(
                MessageFlowDirection.NetworkToBridge,
                envelope.Type);
            BridgeEnvelopeDelivered?.Invoke(envelope);
        }
        else
        {
            _messageFlowDiagnostics.MarkDropped(
                MessageFlowDirection.NetworkToBridge,
                envelope.Type);
        }
        return delivered;
    }

    // Retry the deferred guest reconnect sequence: clear cached state, deliver the local reset, then request the host's replay.
    // The overload receiving a bridge token can use a delivery barrier that its caller already holds.
    private async ValueTask ReplayDeferredGuestReconnectResyncAsync(
        ILanSession network,
        ControlPeerToken peer,
        CancellationToken cancellationToken) =>
        await ReplayDeferredGuestReconnectResyncAsync(
                network,
                peer,
                bridgeConnectionUnderBarrier: null,
                cancellationToken)
            .ConfigureAwait(false);

    // Retry the deferred guest reconnect sequence: clear cached state, deliver the local reset, then request the host's replay.
    // The overload receiving a bridge token can use a delivery barrier that its caller already holds.
    private async ValueTask ReplayDeferredGuestReconnectResyncAsync(
        ILanSession network,
        ControlPeerToken peer,
        BridgePipeConnectionToken? bridgeConnectionUnderBarrier,
        CancellationToken cancellationToken)
    {
        Func<ProtocolEnvelope, CancellationToken, ValueTask<bool>>
            deliverToBridge = bridgeConnectionUnderBarrier is { } connection
                ? (envelope, _) =>
                    DeliverDeferredGuestResyncToBridgeUnderBarrierAsync(
                        connection,
                        envelope)
                : DeliverDeferredGuestResyncToBridgeAsync;
        // Reconnect repair order: clear saved data, tell RDR2 to reset, then ask the host to resend its data.
        // The reset must reach the guest game before we ask the host to send replacements, otherwise new data could be cleared by a late reset.
        var replay = await _guestReconnectResyncGate.ReplayOnceAsync(
                ClearGuestAuthoritativeStateForResync,
                deliverToBridge,
                token => SendPeerControlAsync(
                    network,
                    peer,
                    MessageType.ResyncRequest,
                    ReadOnlyMemory<byte>.Empty,
                    NetworkClock.Tick,
                    token),
                cancellationToken)
            .ConfigureAwait(false);
        if (replay.Disposition ==
            GuestReconnectResyncReplayDisposition.NoPendingRequest)
        {
            return;
        }

        var replayData = new Dictionary<string, object?>
        {
            ["disposition"] = replay.Disposition.ToString(),
            ["bridgeDelivered"] = replay.BridgeDelivered,
            ["peerRequested"] = replay.PeerRequested,
            ["peerGeneration"] = peer.Generation,
            ["ordering"] = "clear-local,deliver-bridge,request-host"
        };
        if (replay.Disposition ==
            GuestReconnectResyncReplayDisposition.Completed)
        {
            await _logger.InfoAsync(
                "network.resync.guest-replayed-after-negotiation",
                "Delivered the deferred guest reset locally before requesting the host reconnect snapshot.",
                replayData,
                cancellationToken).ConfigureAwait(false);
        }
        else
        {
            await _logger.WarningAsync(
                "network.resync.guest-replay-incomplete",
                "Guest reconnect resync did not complete; the request remains armed for a safe retry.",
                replayData,
                cancellationToken).ConfigureAwait(false);
        }
    }

    // Signal a fatal session failure once when movement-mode negotiation fails or times out.
    // The main runtime loop observes that failure and shuts down instead of streaming with incompatible modes.
    private async ValueTask FailMotionModeNegotiationAsync(
        string reason,
        MotionReplicationWireMode? peerMode,
        uint peerRevision,
        CancellationToken cancellationToken)
    {
        var localMode = ToWireMotionMode(_config.MotionReplicationMode);
        var exception = new MotionReplicationModeMismatchException(
            reason,
            localMode,
            peerMode);
        if (Interlocked.CompareExchange(
                ref _motionModeNegotiationFailureShown,
                1,
                0) != 0)
        {
            return;
        }

        _fatalSessionFailure.TrySetResult(exception);
        await ReportMotionModeNegotiationFailureAsync(
                reason,
                localMode,
                peerMode,
                peerRevision,
                exception,
                cancellationToken)
            .ConfigureAwait(false);
    }

    // Signal movement-mode failure only if the peer that caused it is still the current connection.
    // A late failure from an old connection must not shut down its replacement.
    private async ValueTask FailMotionModeNegotiationForPeerAsync(
        string reason,
        MotionReplicationWireMode? peerMode,
        uint peerRevision,
        ILanSession sourceNetwork,
        ControlPeerToken sourcePeer,
        CancellationToken cancellationToken)
    {
        var localMode = ToWireMotionMode(_config.MotionReplicationMode);
        var exception = new MotionReplicationModeMismatchException(
            reason,
            localMode,
            peerMode);
        var reserved = false;
        if (!sourceNetwork.TryRunForControlPeer(sourcePeer, () =>
        {
            if (!ReferenceEquals(_network, sourceNetwork) ||
                Interlocked.CompareExchange(
                    ref _motionModeNegotiationFailureShown,
                    1,
                    0) != 0)
            {
                return;
            }

            reserved = true;
            _fatalSessionFailure.TrySetResult(exception);
        }) || !reserved)
        {
            return;
        }

        await ReportMotionModeNegotiationFailureAsync(
                reason,
                localMode,
                peerMode,
                peerRevision,
                exception,
                cancellationToken)
            .ConfigureAwait(false);
    }

    // Record the mode mismatch and try to show both players' required next step in the local game menu.
    private async ValueTask ReportMotionModeNegotiationFailureAsync(
        string reason,
        MotionReplicationWireMode localMode,
        MotionReplicationWireMode? peerMode,
        uint peerRevision,
        MotionReplicationModeMismatchException exception,
        CancellationToken cancellationToken)
    {
        await _logger.ErrorAsync(
            "network.motion-mode.negotiation-failed",
            reason + " Streaming and the session are stopping fail-fast. " +
            $"localMode={localMode}; peerMode={peerMode?.ToString() ?? "unavailable"}; " +
            $"peerRevision={peerRevision}; protocolVersion={ProtocolConstants.Version}.",
            exception,
            cancellationToken: CancellationToken.None).ConfigureAwait(false);

        if (IsBridgeReady())
        {
            try
            {
                await SendSessionStatusAsync(
                        new SessionMenuStatusPayload(
                            SessionMenuStatusKind.Error,
                            "MOTION MODE MISMATCH: select the same mode on both PCs and restart the session."),
                        CancellationToken.None)
                    .ConfigureAwait(false);
            }
            catch (IOException)
            {
                // The structured log remains the authoritative failure report.
            }
        }
    }

    // Check whether outgoing gameplay must stop at the current negotiation or reconnect boundary and log the first drop.
    // Despite the word defer, this method does not store the message for later delivery.
    private async ValueTask<bool>
        ShouldDeferOutboundUntilMotionModeNegotiatedAsync(
            MessageType messageType,
            CancellationToken cancellationToken)
    {
        var networkConnected = _network is { IsConnected: true };
        var motionModeNegotiated = IsMotionModeNegotiated();
        var guestReconnectResetPending =
            _guestReconnectResyncGate.HasPendingRequest;
        if (!ShouldDeferOutboundForSessionBoundary(
                _config.Role,
                messageType,
                networkConnected,
                motionModeNegotiated,
                guestReconnectResetPending))
        {
            return false;
        }

        if (Interlocked.CompareExchange(
                ref _preNegotiationOutboundDropShown,
                1,
                0) == 0)
        {
            await _logger.WarningAsync(
                "network.motion-mode.pre-negotiation-local-frame-dropped",
                guestReconnectResetPending &&
                _config.Role == SessionRole.Guest
                    ? "Dropped local guest traffic until its bridge reset is delivered and the host replay request succeeds."
                    : "Dropped local game traffic until the authenticated peer confirms the same motion mode.",
                new Dictionary<string, object?>
                {
                    ["messageType"] = messageType.ToString(),
                    ["configuredMode"] =
                        _config.MotionReplicationMode.ToString(),
                    ["motionModeNegotiated"] = motionModeNegotiated,
                    ["guestReconnectResetPending"] =
                        guestReconnectResetPending,
                    ["direction"] = "bridge-to-network"
                },
                cancellationToken: cancellationToken)
                .ConfigureAwait(false);
        }
        return true;
    }

    // Return whether a connected session is still negotiating or a guest still has reconnect cleanup pending.
    // The message type is currently ignored, so this rule does not grant special types an exception.
    internal static bool ShouldDeferOutboundForSessionBoundary(
        SessionRole role,
        MessageType messageType,
        bool networkConnected,
        bool motionModeNegotiated,
        bool guestReconnectResetPending)
    {
        _ = messageType;
        return networkConnected &&
            (!motionModeNegotiated ||
             role == SessionRole.Guest && guestReconnectResetPending);
    }

    // Periodically give the identity publisher a chance to resend our nickname and player ID after negotiation.
    // The publisher decides whether a refresh is due, so this loop does not necessarily send every second.
    private async Task IdentityLoopAsync(
        CancellationToken cancellationToken)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromSeconds(1));
        while (await timer.WaitForNextTickAsync(cancellationToken)
                   .ConfigureAwait(false))
        {
            if (_network is { IsConnected: true } network &&
                IsMotionModeNegotiated())
            {
                await PublishLocalIdentityAsync(
                    network,
                    NetworkClock.Tick,
                    force: false,
                    cancellationToken).ConfigureAwait(false);
            }
        }
    }

    // Ask the identity publisher to send when due or forced, and route the payload through reliable control delivery.
    // Overloads optionally pin the send to a particular peer connection instead of whichever peer is current later.
    private ValueTask PublishLocalIdentityAsync(
        ILanSession network,
        ulong tick,
        bool force,
        CancellationToken cancellationToken) =>
        PublishLocalIdentityAsync(
            network,
            expectedPeer: null,
            tick,
            force,
            cancellationToken);

    // Ask the identity publisher to send when due or forced, and route the payload through reliable control delivery.
    // Overloads optionally pin the send to a particular peer connection instead of whichever peer is current later.
    private ValueTask PublishLocalIdentityAsync(
        ILanSession network,
        ControlPeerToken expectedPeer,
        ulong tick,
        bool force,
        CancellationToken cancellationToken) =>
        PublishLocalIdentityAsync(
            network,
            expectedPeer: (ControlPeerToken?)expectedPeer,
            tick,
            force,
            cancellationToken);

    // Ask the identity publisher to send when due or forced, and route the payload through reliable control delivery.
    // Overloads optionally pin the send to a particular peer connection instead of whichever peer is current later.
    private async ValueTask PublishLocalIdentityAsync(
        ILanSession network,
        ControlPeerToken? expectedPeer,
        ulong tick,
        bool force,
        CancellationToken cancellationToken)
    {
        var result = await _identityPublisher.PublishIfDueAsync(
            () => ReferenceEquals(_network, network) &&
                network.IsConnected &&
                (!expectedPeer.HasValue ||
                 network.IsControlPeerCurrent(expectedPeer.Value)),
            (payload, sendTick, token) => expectedPeer.HasValue
                ? SendPeerControlAsync(
                    network,
                    expectedPeer.Value,
                    MessageType.PlayerIdentity,
                    payload,
                    sendTick,
                    token)
                : SendPeerControlAsync(
                    network,
                    MessageType.PlayerIdentity,
                    payload,
                    sendTick,
                    controlSendGateHeld: false,
                    token),
            tick,
            force,
            cancellationToken).ConfigureAwait(false);
        if (result == IdentityPublishResult.Delivered)
        {
            await _logger.InfoAsync(
                "network.identity.delivered",
                force
                    ? "Delivered local player identity after peer connection."
                    : "Delivered local player identity refresh.",
                new Dictionary<string, object?>
                {
                    ["role"] = _config.Role.ToString(),
                    ["reason"] = force ? "connection" : "first-or-refresh"
                },
                cancellationToken).ConfigureAwait(false);
        }
        else if (result == IdentityPublishResult.Failed)
        {
            await _logger.WarningAsync(
                "network.identity.deferred",
                "Local player identity delivery failed and will be retried.",
                new Dictionary<string, object?>
                {
                    ["role"] = _config.Role.ToString()
                },
                cancellationToken: cancellationToken).ConfigureAwait(false);
        }
    }

    // Record transport counters and the marker's ID, position, and timing when a user diagnostic marker passes this stage.
    // Matching that ID in both players' logs helps compare the same observed incident.
    private ValueTask WriteDiagnosticMarkerSnapshotAsync(
        ProtocolEnvelope envelope,
        string stage,
        CancellationToken cancellationToken)
    {
        var marker = BinaryPayloadCodec.DecodeCommand(envelope.Payload.Span);
        var totals = ReadStreamingCounters();
        var data = CreateStreamingData(
            StreamingCounters.Empty,
            totals,
            _messageFlowDiagnostics.Capture());
        data["markerCorrelationId"] = marker.TargetEntityId.Value;
        data["markerLocalId"] = DiagnosticMarkerLocalId(marker);
        data["markerOriginRole"] =
            DiagnosticMarkerOrigin(marker)?.ToString();
        data["markerStage"] = stage;
        data["markerSenderTick"] = envelope.Tick;
        data["markerObservedTick"] = NetworkClock.Tick;
        data["markerPositionX"] = marker.Position.X;
        data["markerPositionY"] = marker.Position.Y;
        data["markerPositionZ"] = marker.Position.Z;
        data["markerHeading"] = marker.Heading;
        return _logger.InfoAsync(
            "diagnostics.user-marker.snapshot",
            $"Captured diagnostic marker {marker.TargetEntityId.Value} at {stage} with an immediate transport snapshot.",
            data,
            cancellationToken);
    }

    // Regularly inspect message-flow gaps and log interval counts alongside lifetime totals.
    // This observes transport behavior and does not move players or resync NPCs.
    private async Task DiagnosticsLoopAsync(
        CancellationToken cancellationToken)
    {
        var previous = StreamingCounters.Empty;
        var nextAggregateReportMs = 0L;
        using var timer = new PeriodicTimer(
            TimeSpan.FromMilliseconds(Math.Min(
                _config.Network.DiagnosticsIntervalMs,
                1_000)));
        while (await timer.WaitForNextTickAsync(cancellationToken)
                   .ConfigureAwait(false))
        {
            var flow = _messageFlowDiagnostics.Capture();
            await EvaluateTransportGapsAsync(flow, cancellationToken)
                .ConfigureAwait(false);
            var nowMs = Environment.TickCount64;
            if (nextAggregateReportMs != 0 &&
                nowMs < nextAggregateReportMs)
            {
                continue;
            }
            nextAggregateReportMs =
                nowMs + _config.Network.DiagnosticsIntervalMs;
            var current = ReadStreamingCounters();
            var interval = current.Subtract(previous);
            previous = current;
            await _logger.InfoAsync(
                "diagnostics.streaming",
                "Aggregated player-state streaming counters.",
                CreateStreamingData(interval, current, flow),
                cancellationToken).ConfigureAwait(false);
        }
    }

    // Check for movement negotiation timeouts and game-pipe writes that have remained stuck too long.
    // Abort a stalled pipe write using different wait limits for replaceable snapshots and important control messages.
    private async Task BridgeDeliveryWatchdogAsync(
        CancellationToken cancellationToken)
    {
        using var timer = new PeriodicTimer(
            TimeSpan.FromMilliseconds(BridgeDeliveryWatchdogIntervalMs));
        while (await timer.WaitForNextTickAsync(cancellationToken)
                   .ConfigureAwait(false))
        {
            var negotiationStartedAt = Interlocked.Read(
                ref _peerMotionModeNegotiationStartedAt);
            if (_network is { IsConnected: true } &&
                !IsMotionModeNegotiated() &&
                negotiationStartedAt > 0 &&
                Environment.TickCount64 - negotiationStartedAt >=
                    PeerMotionModeNegotiationTimeoutMs)
            {
                await FailMotionModeNegotiationAsync(
                        $"Authenticated peer did not announce a motion mode within {PeerMotionModeNegotiationTimeoutMs} ms.",
                        peerMode: null,
                        peerRevision: 0,
                        cancellationToken)
                    .ConfigureAwait(false);
                continue;
            }

            var pump = _networkBridgePump.ReadSnapshot();
            var replaceableSnapshot =
                IsReplaceableBridgeDeliveryType(pump.ActiveType);
            // The ScriptHook tick can legitimately stop draining the pipe while RDR2 has its native pause screen open.
            // Snapshot state is latest-only/coalesced, so keeping that in-flight write alive avoids a reconnect storm and it will self-heal on resume.
            // A paused game may stop draining snapshots safely because they are coalesced/latest-only.
            // Lifecycle/control frames get a short cap.
            // These are limits for a blocked pipe write, not the normal sending interval for player or NPC updates.
            var thresholdMs = replaceableSnapshot
                ? BridgeSnapshotDeliveryStallAbortMs
                : BridgeCriticalDeliveryStallAbortMs;
            if (!_bridge.AbortStalledSend(
                    thresholdMs,
                    out var pipeGeneration,
                    out var activeMilliseconds))
            {
                continue;
            }

            await _logger.WarningAsync(
                "bridge.delivery-stalled.connection-reset",
                "Reset the game pipe because RDR2 stopped draining a network frame.",
                new Dictionary<string, object?>
                {
                    ["activeType"] = pump.ActiveType?.ToString(),
                    ["activeMs"] = activeMilliseconds,
                    ["pipeGeneration"] = pipeGeneration,
                    ["backlog"] = pump.Backlog,
                    ["maxBacklog"] = pump.MaxBacklog,
                    ["thresholdMs"] = thresholdMs,
                    ["replaceableSnapshot"] = replaceableSnapshot
                },
                cancellationToken: CancellationToken.None).ConfigureAwait(false);
        }
    }

    // Write one final totals snapshot during shutdown so diagnostics include the end of the session.
    private ValueTask WriteFinalStreamingDiagnosticsAsync()
    {
        var totals = ReadStreamingCounters();
        return _logger.InfoAsync(
            "diagnostics.streaming-final",
            "Final aggregated player-state streaming totals.",
            CreateStreamingData(
                totals,
                totals,
                _messageFlowDiagnostics.Capture()),
            cancellationToken: CancellationToken.None);
    }

    // Build the diagnostic fields describing message counts, queue delays, cached world entities, and interaction decisions.
    // Interval counts describe the latest reporting period, while totals cover the runtime's accumulated counts.
    private Dictionary<string, object?> CreateStreamingData(
        StreamingCounters interval,
        StreamingCounters totals,
        IReadOnlyList<MessageFlowStreamSnapshot> messageFlow)
    {
        var pump = _networkBridgePump.ReadSnapshot();
        var worldGraph = _worldGraph.ReadSnapshot();
        var interactions = _interactions.ReadSnapshot();
        var heartbeatAt = Interlocked.Read(ref _lastPeerHeartbeatAtMs);
        var clockDelta = Interlocked.Read(
            ref _lastPeerClockDeltaEstimateMs);
        return new Dictionary<string, object?>
        {
            ["role"] = _network is null
                ? "PendingInGameMenu"
                : _config.Role.ToString(),
            ["sessionFingerprint"] = string.IsNullOrEmpty(
                _sessionFingerprint)
                ? null
                : _sessionFingerprint,
            ["peerConnected"] = _network?.IsConnected ?? false,
            ["bridgePipeConnected"] =
                Volatile.Read(ref _bridgePipeConnected) != 0,
            ["bridgeReady"] = IsBridgeReady(),
            ["peerHeartbeatAgeMs"] = heartbeatAt <= 0
                ? -1
                : Math.Max(0, Environment.TickCount64 - heartbeatAt),
            ["peerClockDeltaEstimateMs"] =
                clockDelta == long.MinValue ? null : clockDelta,
            ["intervalMs"] = _config.Network.DiagnosticsIntervalMs,
            ["intervalBridgeToNetworkObserved"] =
                interval.BridgeToNetworkObserved,
            ["intervalBridgeToNetworkDelivered"] =
                interval.BridgeToNetworkDelivered,
            ["intervalBridgeToNetworkNoPeer"] =
                interval.BridgeToNetworkNoPeer,
            ["intervalBridgeToNetworkDropped"] =
                interval.BridgeToNetworkDropped,
            ["intervalNetworkToBridgeObserved"] =
                interval.NetworkToBridgeObserved,
            ["intervalNetworkToBridgeDelivered"] =
                interval.NetworkToBridgeDelivered,
            ["intervalNetworkToBridgeUnavailable"] =
                interval.NetworkToBridgeUnavailable,
            ["intervalNetworkToBridgeQueued"] =
                interval.NetworkToBridgeQueued,
            ["intervalNetworkToBridgeCoalesced"] =
                interval.NetworkToBridgeCoalesced,
            ["intervalNetworkToBridgeQueueRejected"] =
                interval.NetworkToBridgeQueueRejected,
            ["intervalNetworkToBridgeQueueDequeued"] =
                interval.NetworkToBridgeQueueDequeued,
            ["intervalNetworkToBridgeQueueDelivered"] =
                interval.NetworkToBridgeQueueDelivered,
            ["intervalNetworkToBridgeQueueUnavailable"] =
                interval.NetworkToBridgeQueueUnavailable,
            ["totalBridgeToNetworkObserved"] =
                totals.BridgeToNetworkObserved,
            ["totalBridgeToNetworkDelivered"] =
                totals.BridgeToNetworkDelivered,
            ["totalBridgeToNetworkNoPeer"] =
                totals.BridgeToNetworkNoPeer,
            ["totalBridgeToNetworkDropped"] =
                totals.BridgeToNetworkDropped,
            ["totalNetworkToBridgeObserved"] =
                totals.NetworkToBridgeObserved,
            ["totalNetworkToBridgeDelivered"] =
                totals.NetworkToBridgeDelivered,
            ["totalNetworkToBridgeUnavailable"] =
                totals.NetworkToBridgeUnavailable,
            ["totalNetworkToBridgeQueued"] =
                totals.NetworkToBridgeQueued,
            ["totalNetworkToBridgeCoalesced"] =
                totals.NetworkToBridgeCoalesced,
            ["totalNetworkToBridgeQueueRejected"] =
                totals.NetworkToBridgeQueueRejected,
            ["totalNetworkToBridgeQueueDequeued"] =
                totals.NetworkToBridgeQueueDequeued,
            ["totalNetworkToBridgeQueueDelivered"] =
                totals.NetworkToBridgeQueueDelivered,
            ["totalNetworkToBridgeQueueUnavailable"] =
                totals.NetworkToBridgeQueueUnavailable,
            // Backlog counts messages still waiting, while ActiveMs measures how long the current delivery has been active.
            ["networkToBridgeBacklog"] = pump.Backlog,
            ["networkToBridgeMaxBacklog"] = pump.MaxBacklog,
            ["networkToBridgeActiveType"] = pump.ActiveType?.ToString(),
            ["networkToBridgeActiveMs"] = pump.ActiveMilliseconds,
            ["entityGraphVersion"] = 10,
            ["entityGraphNodes"] = worldGraph.Nodes,
            ["entityGraphEdges"] = worldGraph.Edges,
            ["entityGraphRevision"] = worldGraph.Revision,
            ["entityGraphApplied"] = worldGraph.Applied,
            ["entityGraphDuplicate"] = worldGraph.Duplicate,
            ["entityGraphStale"] = worldGraph.Stale,
            ["entityGraphCapacityRejected"] =
                worldGraph.CapacityRejected,
            ["entityGraphCascadedDespawns"] =
                worldGraph.CascadedDespawns,
            ["interactionAuthorityVersion"] = 1,
            ["interactionActive"] = interactions.ActiveInteractions,
            ["interactionRestrainedPlayers"] =
                interactions.RestrainedPlayers,
            ["interactionAccepted"] = interactions.Accepted,
            ["interactionCompleted"] = interactions.Completed,
            ["interactionRejected"] = interactions.Rejected,
            ["interactionDuplicate"] = interactions.Duplicate,
            ["interactionStale"] = interactions.Stale,
            ["interactionCancelled"] = interactions.Cancelled,
            ["messageFlow"] = messageFlow.Select(static stream =>
                new Dictionary<string, object?>
                {
                    ["direction"] = stream.Direction.ToString(),
                    ["messageType"] = stream.MessageType.ToString(),
                    ["observed"] = stream.Observed,
                    ["delivered"] = stream.Delivered,
                    ["dropped"] = stream.Dropped,
                    ["coalesced"] = stream.Coalesced,
                    ["lastObservedAgeMs"] =
                        stream.LastObservedAgeMs,
                    ["averageGapMs"] =
                        Math.Round(stream.AverageGapMs, 2),
                    ["p95GapMs"] = stream.P95GapMs,
                    ["maximumGapMs"] = stream.MaximumGapMs
                }).ToArray()
        };
    }

    // Choose which message streams should currently be active and warn when one has not been observed for too long.
    // These thresholds only trigger diagnostic messages and are not NPC despawn timers or automatic resync commands.
    private async ValueTask EvaluateTransportGapsAsync(
        IReadOnlyList<MessageFlowStreamSnapshot> messageFlow,
        CancellationToken cancellationToken)
    {
        if (_network is not { IsConnected: true } ||
            !IsBridgeReady())
        {
            _openTransportGaps.Clear();
            return;
        }

        // These are diagnostic expectations, not recovery commands.
        // They make a missing realtime stream visible without triggering a resync loop.
        // Build expectations for the streams needed in the current gameplay state instead of assuming every message type sends continuously.
        var expectations = new List<(
            MessageFlowDirection Direction,
            MessageType Type,
            int ThresholdMs,
            string Signal)>
        {
            (MessageFlowDirection.BridgeToNetwork,
                MessageType.PlayerState,
                750,
                "local-player-snapshot"),
            (MessageFlowDirection.NetworkToBridge,
                MessageType.PlayerState,
                750,
                "remote-player-snapshot")
        };
        if (_config.MotionReplicationMode ==
            MotionReplicationMode.AnimGraphReplica)
        {
            expectations.Add((
                MessageFlowDirection.BridgeToNetwork,
                MessageType.PlayerAnimationState,
                1_000,
                "local-animation-snapshot"));
            expectations.Add((
                MessageFlowDirection.NetworkToBridge,
                MessageType.PlayerAnimationState,
                1_000,
                "remote-animation-snapshot"));
        }

        if (_config.Role == SessionRole.Host)
        {
            expectations.Add((
                MessageFlowDirection.BridgeToNetwork,
                MessageType.MissionState,
                2_000,
                "local-mission-authority"));
        }
        else
        {
            expectations.Add((
                MessageFlowDirection.NetworkToBridge,
                MessageType.MissionState,
                2_000,
                "host-mission-authority"));
        }

        var graph = _worldGraph.ReadSnapshot();
        if (_config.Role == SessionRole.Guest && graph.Nodes > 0)
        {
            expectations.Add((
                MessageFlowDirection.NetworkToBridge,
                MessageType.EntityUpdate,
                1_500,
                "host-world-graph"));
        }

        var missionEnvelope = _missionStateCache.Capture();
        MissionPhase? missionPhase = null;
        if (missionEnvelope is not null)
        {
            missionPhase = BinaryPayloadCodec.DecodeMissionState(
                missionEnvelope.Payload.Span).Phase;
        }
        var cinematicEnvelope = _missionCinematicStateCache.Capture();
        MissionCinematicStatePayload? cinematicState = null;
        if (cinematicEnvelope is not null)
        {
            cinematicState = BinaryPayloadCodec.DecodeMissionCinematicState(
                cinematicEnvelope.Payload.Span);
        }
        var cinematicPresentationActive = cinematicState?.Phase is
            MissionCinematicPhase.Playing or
            MissionCinematicPhase.Loading or
            MissionCinematicPhase.PrepareResume;
        if (_config.Role == SessionRole.Guest &&
            cinematicState?.Phase == MissionCinematicPhase.Playing &&
            cinematicState.Value.Flags.HasFlag(
                MissionCinematicStateFlags.CameraExpected))
        {
            expectations.Add((
                MessageFlowDirection.NetworkToBridge,
                MessageType.MissionCameraState,
                500,
                "host-cutscene-camera"));
        }
        if (_config.Role == SessionRole.Guest &&
            !cinematicPresentationActive &&
            missionPhase is not (MissionPhase.Cutscene or
                MissionPhase.Loading or MissionPhase.Recovery or
                MissionPhase.SoloOverride))
        {
            expectations.Add((
                MessageFlowDirection.NetworkToBridge,
                MessageType.WorldState,
                1_500,
                "host-world-authority"));
        }
        if (_config.Role == SessionRole.Guest &&
            !cinematicPresentationActive &&
            missionPhase == MissionPhase.Active &&
            graph.Applied == 0)
        {
            expectations.Add((
                MessageFlowDirection.NetworkToBridge,
                MessageType.EntitySpawn,
                5_000,
                "host-world-graph-first-frame"));
        }

        var currentKeys = expectations
            .Select(static item => (item.Direction, item.Type))
            .ToHashSet();
        _openTransportGaps.RemoveWhere(key => !currentKeys.Contains(key));
        var byStream = messageFlow.ToDictionary(
            static stream => (stream.Direction, stream.MessageType));
        // Give a newly negotiated stream its allowed startup time before reporting that its first message never arrived.
        var streamingStartedAt = Interlocked.Read(
            ref _peerStreamingStartedAt);
        var nowMs = Environment.TickCount64;
        foreach (var expectation in expectations)
        {
            var key = (expectation.Direction, expectation.Type);
            if (!byStream.TryGetValue(key, out var stream) ||
                stream.Observed == 0)
            {
                if (streamingStartedAt <= 0 ||
                    nowMs - streamingStartedAt <=
                        expectation.ThresholdMs)
                {
                    continue;
                }
                stream = new MessageFlowStreamSnapshot(
                    expectation.Direction,
                    expectation.Type,
                    0,
                    0,
                    0,
                    0,
                    nowMs - streamingStartedAt,
                    0.0,
                    0,
                    0);
            }

            // Log only a transition into a gap and its later recovery, rather than producing a warning every diagnostics interval during loss.
            if (stream.LastObservedAgeMs > expectation.ThresholdMs)
            {
                // Log a gap once when it begins and wait for recovery before reporting another gap for the same stream.
                if (!_openTransportGaps.Add(key))
                {
                    continue;
                }
                await _logger.WarningAsync(
                    "diagnostics.transport-gap",
                    "A required realtime stream stopped arriving within its diagnostic budget.",
                    CreateTransportGapData(
                        expectation.Direction,
                        expectation.Type,
                        expectation.Signal,
                        expectation.ThresholdMs,
                        stream,
                        missionPhase,
                        graph.Nodes,
                        recovered: false),
                    cancellationToken).ConfigureAwait(false);
                continue;
            }

            if (!_openTransportGaps.Remove(key))
            {
                continue;
            }
            await _logger.InfoAsync(
                "diagnostics.transport-recovered",
                "The required realtime stream resumed after a detected gap.",
                CreateTransportGapData(
                    expectation.Direction,
                    expectation.Type,
                    expectation.Signal,
                    expectation.ThresholdMs,
                    stream,
                    missionPhase,
                    graph.Nodes,
                    recovered: true),
                cancellationToken).ConfigureAwait(false);
        }
    }

    // Attach stream timing and delivery-queue state to a gap or recovery report.
    // Queue backlog and active-write age help distinguish incoming silence from slow delivery to the game.
    private Dictionary<string, object?> CreateTransportGapData(
        MessageFlowDirection direction,
        MessageType type,
        string signal,
        int thresholdMs,
        MessageFlowStreamSnapshot stream,
        MissionPhase? missionPhase,
        int graphNodes,
        bool recovered)
    {
        var pump = _networkBridgePump.ReadSnapshot();
        return new Dictionary<string, object?>
        {
            ["role"] = _config.Role.ToString(),
            ["sessionFingerprint"] = string.IsNullOrEmpty(
                _sessionFingerprint)
                ? null
                : _sessionFingerprint,
            ["direction"] = direction.ToString(),
            ["messageType"] = type.ToString(),
            ["signal"] = signal,
            ["recovered"] = recovered,
            ["lastObservedAgeMs"] = stream.LastObservedAgeMs,
            ["thresholdMs"] = thresholdMs,
            ["averageGapMs"] = Math.Round(stream.AverageGapMs, 2),
            ["p95GapMs"] = stream.P95GapMs,
            ["maximumGapMs"] = stream.MaximumGapMs,
            ["observed"] = stream.Observed,
            ["delivered"] = stream.Delivered,
            ["dropped"] = stream.Dropped,
            ["coalesced"] = stream.Coalesced,
            ["missionPhase"] = missionPhase?.ToString(),
            ["entityGraphNodes"] = graphNodes,
            ["bridgeBacklog"] = pump.Backlog,
            ["bridgeMaxBacklog"] = pump.MaxBacklog,
            ["bridgeActiveType"] = pump.ActiveType?.ToString(),
            ["bridgeActiveMs"] = pump.ActiveMilliseconds
        };
    }

    // Take a thread-safe reading of player-message counters and include the delivery pump's current totals.
    private StreamingCounters ReadStreamingCounters()
    {
        var pump = _networkBridgePump.ReadSnapshot();
        return new StreamingCounters(
            Interlocked.Read(ref _bridgeToNetworkObserved),
            Interlocked.Read(ref _bridgeToNetworkDelivered),
            Interlocked.Read(ref _bridgeToNetworkNoPeer),
            Interlocked.Read(ref _bridgeToNetworkDropped),
            Interlocked.Read(ref _networkToBridgeObserved),
            Interlocked.Read(ref _networkToBridgeDelivered),
            Interlocked.Read(ref _networkToBridgeUnavailable),
            pump.Queued,
            pump.Coalesced,
            pump.Rejected,
            pump.Dequeued,
            pump.Delivered,
            pump.Unavailable);
    }

    private readonly record struct StreamingCounters(
        long BridgeToNetworkObserved,
        long BridgeToNetworkDelivered,
        long BridgeToNetworkNoPeer,
        long BridgeToNetworkDropped,
        long NetworkToBridgeObserved,
        long NetworkToBridgeDelivered,
        long NetworkToBridgeUnavailable,
        long NetworkToBridgeQueued,
        long NetworkToBridgeCoalesced,
        long NetworkToBridgeQueueRejected,
        long NetworkToBridgeQueueDequeued,
        long NetworkToBridgeQueueDelivered,
        long NetworkToBridgeQueueUnavailable)
    {
        public static StreamingCounters Empty =>
            new(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

        // Subtract an earlier total from the current total to get counts for just the latest diagnostic interval.
        public StreamingCounters Subtract(StreamingCounters previous) =>
            new(
                BridgeToNetworkObserved - previous.BridgeToNetworkObserved,
                BridgeToNetworkDelivered - previous.BridgeToNetworkDelivered,
                BridgeToNetworkNoPeer - previous.BridgeToNetworkNoPeer,
                BridgeToNetworkDropped - previous.BridgeToNetworkDropped,
                NetworkToBridgeObserved - previous.NetworkToBridgeObserved,
                NetworkToBridgeDelivered - previous.NetworkToBridgeDelivered,
                NetworkToBridgeUnavailable -
                previous.NetworkToBridgeUnavailable,
                NetworkToBridgeQueued - previous.NetworkToBridgeQueued,
                NetworkToBridgeCoalesced - previous.NetworkToBridgeCoalesced,
                NetworkToBridgeQueueRejected -
                previous.NetworkToBridgeQueueRejected,
                NetworkToBridgeQueueDequeued -
                previous.NetworkToBridgeQueueDequeued,
                NetworkToBridgeQueueDelivered -
                previous.NetworkToBridgeQueueDelivered,
                NetworkToBridgeQueueUnavailable -
                previous.NetworkToBridgeQueueUnavailable);
    }

    // Create the guest profile if neither the primary file nor its backup exists, otherwise load the saved profile.
    // The store can recover from a backup when the primary profile cannot be loaded.
    private async Task EnsureGuestProfileAsync(CancellationToken cancellationToken)
    {
        if (_config.Role != SessionRole.Guest)
        {
            return;
        }

        using var store = new GuestProfileStore();
        var path = _config.ExpandedProfilePath;
        if (!File.Exists(path) && !File.Exists(GuestProfileStore.GetBackupPath(path)))
        {
            await store.SaveAsync(
                path,
                GuestProfile.Create(Guid.NewGuid(), "Guest"),
                cancellationToken).ConfigureAwait(false);
            await _logger.InfoAsync(
                "profile.created",
                $"Created guest profile '{path}'.",
                cancellationToken: cancellationToken).ConfigureAwait(false);
            return;
        }

        var result = await store.LoadAsync(path, cancellationToken).ConfigureAwait(false);
        await _logger.InfoAsync(
            result.RecoveredFromBackup ? "profile.recovered" : "profile.loaded",
            result.RecoveredFromBackup
                ? "Loaded guest profile from backup after primary-file failure."
                : $"Loaded guest profile {result.Profile.GuestId:N}.",
            cancellationToken: cancellationToken).ConfigureAwait(false);
    }

    // Avoid repeating successful mission logs for the same epoch and revision, and filter routine action refreshes.
    // This affects logging only and does not prevent the message from being sent.
    private bool ShouldLogOutboundControlSuccess(
        ProtocolEnvelope envelope,
        MissionStatePayload? missionState)
    {
        if (missionState.HasValue)
        {
            var state = missionState.Value;
            if (Volatile.Read(ref _hasLoggedMissionTxVersion) != 0 &&
                state.MissionEpoch == _lastLoggedMissionTxEpoch &&
                state.Revision == _lastLoggedMissionTxRevision)
            {
                return false;
            }
            _lastLoggedMissionTxEpoch = state.MissionEpoch;
            _lastLoggedMissionTxRevision = state.Revision;
            Volatile.Write(ref _hasLoggedMissionTxVersion, 1);
            return true;
        }
        return IsSemanticControlTransition(envelope);
    }

    // Log meaningful incoming control changes while skipping unchanged mission refreshes and routine action samples.
    private static bool ShouldLogInboundControlSuccess(
        ProtocolEnvelope envelope,
        MissionStateCacheUpdate? missionStateUpdate) =>
        missionStateUpdate.HasValue
            ? missionStateUpdate.Value.Disposition !=
                MissionStateCacheDisposition.Refreshed
            : IsSemanticControlTransition(envelope);

    // Decide which successful game-pipe control writes need an individual log entry rather than only counters.
    private static bool ShouldLogBridgeDeliverySuccess(
        ProtocolEnvelope envelope) =>
        envelope.Type != MessageType.MissionState &&
        IsSemanticControlTransition(envelope);

    // For logging, distinguish action starts and endings from repeated Active, Sustain, or Snapshot reports.
    // Non-action control messages are treated as worth describing by this helper.
    private static bool IsSemanticControlTransition(
        ProtocolEnvelope envelope)
    {
        if (envelope.Type != MessageType.PlayerAction)
        {
            return true;
        }
        try
        {
            var action = BinaryPayloadCodec.DecodePlayerAction(
                envelope.Payload.Span);
            return action.Phase is not (
                PlayerActionPhase.Active or
                PlayerActionPhase.Sustain or
                PlayerActionPhase.Snapshot);
        }
        catch (ProtocolException)
        {
            return true;
        }
    }

    // Describe an animation-scene definition's identity, scene resource, and role count for diagnostics.
    // Extra fields let the caller add whether it was cached, sent, or rejected.
    private Dictionary<string, object?> CreateAnimSceneDefinitionDiagnosticsData(
        ProtocolEnvelope envelope,
        AnimSceneDefinitionPayload definition,
        string direction,
        bool? delivered,
        params (string Key, object? Value)[] extras)
    {
        var data = new Dictionary<string, object?>
        {
            ["control"] = DescribeControlEnvelope(envelope),
            ["direction"] = direction,
            ["messageType"] = envelope.Type.ToString(),
            ["sequence"] = envelope.Sequence,
            ["senderTick"] = envelope.Tick,
            ["delivered"] = delivered,
            ["sessionFingerprint"] = string.IsNullOrEmpty(
                _sessionFingerprint)
                ? null
                : _sessionFingerprint,
            ["hostEntityId"] = definition.HostEntityId.Value,
            ["missionEpoch"] = definition.MissionEpoch,
            ["cinematicGeneration"] = definition.CinematicGeneration,
            ["definitionRevision"] = definition.DefinitionRevision,
            ["dictionaryHash"] = $"0x{definition.DictionaryHash:X8}",
            ["definitionFingerprint"] =
                $"{definition.FingerprintHigh:X16}{definition.FingerprintLow:X16}",
            ["resourceName"] = definition.ResourceName,
            ["playbackList"] = definition.PlaybackList,
            ["roleCount"] = definition.Roles.Length,
            ["durationSeconds"] = definition.DurationSeconds
        };
        foreach (var (key, value) in extras)
        {
            data[key] = value;
        }
        return data;
    }

    // Build readable log fields for the particular control payload, including IDs and revisions needed to match related events.
    // A decode failure is marked in the diagnostic record instead of replacing the original event with a logging error.
    private Dictionary<string, object?> CreateControlDiagnosticsData(
        ProtocolEnvelope envelope,
        string control,
        string direction,
        bool? delivered,
        params (string Key, object? Value)[] extras)
    {
        var data = new Dictionary<string, object?>
        {
            ["control"] = control,
            ["direction"] = direction,
            ["messageType"] = envelope.Type.ToString(),
            ["sequence"] = envelope.Sequence,
            ["senderTick"] = envelope.Tick,
            ["delivered"] = delivered,
            ["sessionFingerprint"] = string.IsNullOrEmpty(
                _sessionFingerprint)
                ? null
                : _sessionFingerprint
        };
        try
        {
            // Each case adds the fields that identify this kind of event so related messages can be compared across logs.
            switch (envelope.Type)
            {
                case MessageType.PlayerAction:
                {
                    var action = BinaryPayloadCodec.DecodePlayerAction(
                        envelope.Payload.Span);
                    data["actionId"] = action.ActionId;
                    data["actionRevision"] = action.Revision;
                    data["actionKind"] = action.Kind.ToString();
                    data["actionPhase"] = action.Phase.ToString();
                    data["actorEntityId"] = action.ActorEntityId.Value;
                    data["targetEntityId"] = action.TargetEntityId.Value;
                    data["actorSlot"] = action.ActorSlot;
                    data["authoritySlot"] = action.AuthoritySlot;
                    data["actionFlags"] = action.Flags.ToString();
                    data["weaponHash"] = $"0x{action.WeaponHash:X8}";
                    data["durationMs"] = action.DurationMilliseconds;
                    data["phaseElapsedMs"] =
                        action.PhaseElapsedMilliseconds;
                    break;
                }
                case MessageType.Command:
                {
                    var command = BinaryPayloadCodec.DecodeCommand(
                        envelope.Payload.Span);
                    data["commandOpcode"] = command.Opcode.ToString();
                    data["commandTargetEntityId"] =
                        command.TargetEntityId.Value;
                    if (command.Opcode == CommandOpcode.DiagnosticMarker)
                    {
                        data["markerCorrelationId"] =
                            command.TargetEntityId.Value;
                        data["markerLocalId"] =
                            DiagnosticMarkerLocalId(command);
                        data["markerOriginRole"] =
                            DiagnosticMarkerOrigin(command)?.ToString();
                        data["markerPositionX"] = command.Position.X;
                        data["markerPositionY"] = command.Position.Y;
                        data["markerPositionZ"] = command.Position.Z;
                        data["markerHeading"] = command.Heading;
                    }
                    break;
                }
                case MessageType.PlayerMountState:
                {
                    var mount = BinaryPayloadCodec.DecodePlayerMountState(
                        envelope.Payload.Span);
                    data["playerEntityId"] = mount.PlayerEntityId.Value;
                    data["mountEntityId"] = mount.MountEntityId.Value;
                    data["mountGeneration"] = mount.Generation;
                    data["mountFlags"] = mount.Flags.ToString();
                    data["mountSlot"] = mount.Slot;
                    data["mountModelHash"] = $"0x{mount.ModelHash:X8}";
                    break;
                }
                case MessageType.PlayerTraversal:
                {
                    var traversal = BinaryPayloadCodec.DecodePlayerTraversal(
                        envelope.Payload.Span);
                    data["traversalActionId"] = traversal.ActionId;
                    data["traversalRevision"] = traversal.Revision;
                    data["locomotionEpoch"] = traversal.LocomotionEpoch;
                    data["traversalKind"] = traversal.Kind.ToString();
                    data["traversalFlags"] = traversal.Flags.ToString();
                    data["entityId"] = traversal.EntityId.Value;
                    break;
                }
                case MessageType.MissionCinematicState:
                {
                    var state =
                        BinaryPayloadCodec.DecodeMissionCinematicState(
                            envelope.Payload.Span);
                    data["hostEntityId"] = state.HostEntityId.Value;
                    data["missionEpoch"] = state.MissionEpoch;
                    data["cinematicGeneration"] =
                        state.CinematicGeneration;
                    data["cinematicRevision"] = state.Revision;
                    data["cinematicPhase"] = state.Phase.ToString();
                    data["cinematicFlags"] = state.Flags.ToString();
                    data["checkpointGeneration"] =
                        state.CheckpointGeneration;
                    break;
                }
                case MessageType.MissionCinematicAction:
                {
                    var action =
                        BinaryPayloadCodec.DecodeMissionCinematicAction(
                            envelope.Payload.Span);
                    data["hostEntityId"] = action.HostEntityId.Value;
                    data["missionEpoch"] = action.MissionEpoch;
                    data["cinematicGeneration"] =
                        action.CinematicGeneration;
                    data["cinematicActionId"] = action.ActionId;
                    data["cinematicActionKind"] = action.Kind.ToString();
                    data["senderSlot"] = action.SenderSlot;
                    data["cinematicActionFlags"] = action.Flags.ToString();
                    break;
                }
                case MessageType.PlayerAppearanceState:
                {
                    var appearance =
                        BinaryPayloadCodec.DecodePlayerAppearanceState(
                            envelope.Payload.Span);
                    data["entityId"] = appearance.EntityId.Value;
                    data["appearanceSlot"] = appearance.Slot;
                    data["appearanceRevision"] = appearance.Revision;
                    data["modelHash"] = $"0x{appearance.ModelHash:X8}";
                    data["componentCount"] =
                        appearance.ComponentHashes.Length;
                    data["appearanceFingerprint"] =
                        appearance.Fingerprint;
                    break;
                }
                case MessageType.AnimSceneReplicaState:
                {
                    var scene =
                        BinaryPayloadCodec.DecodeAnimSceneReplicaState(
                            envelope.Payload.Span);
                    data["hostEntityId"] = scene.HostEntityId.Value;
                    data["missionEpoch"] = scene.MissionEpoch;
                    data["cinematicGeneration"] =
                        scene.CinematicGeneration;
                    data["animSceneRevision"] = scene.Revision;
                    data["dictionaryHash"] =
                        $"0x{scene.DictionaryHash:X8}";
                    data["phase"] = scene.Phase;
                    data["durationSeconds"] = scene.DurationSeconds;
                    data["animSceneFlags"] = scene.Flags.ToString();
                    break;
                }
                case MessageType.AnimSceneDefinition:
                {
                    var definition = BinaryPayloadCodec.DecodeAnimSceneDefinition(
                        envelope.Payload.Span);
                    data["hostEntityId"] = definition.HostEntityId.Value;
                    data["missionEpoch"] = definition.MissionEpoch;
                    data["cinematicGeneration"] =
                        definition.CinematicGeneration;
                    data["definitionRevision"] =
                        definition.DefinitionRevision;
                    data["dictionaryHash"] =
                        $"0x{definition.DictionaryHash:X8}";
                    data["definitionFingerprint"] =
                        $"{definition.FingerprintHigh:X16}{definition.FingerprintLow:X16}";
                    data["resourceName"] = definition.ResourceName;
                    data["playbackList"] = definition.PlaybackList;
                    data["roleCount"] = definition.Roles.Length;
                    break;
                }
                case MessageType.AnimSceneControl:
                {
                    var animSceneControl =
                        BinaryPayloadCodec.DecodeAnimSceneControl(
                            envelope.Payload.Span);
                    data["hostEntityId"] =
                        animSceneControl.HostEntityId.Value;
                    data["missionEpoch"] = animSceneControl.MissionEpoch;
                    data["cinematicGeneration"] =
                        animSceneControl.CinematicGeneration;
                    data["definitionRevision"] =
                        animSceneControl.DefinitionRevision;
                    data["animSceneActionId"] = animSceneControl.ActionId;
                    data["animSceneControlKind"] =
                        animSceneControl.Kind.ToString();
                    data["senderSlot"] = animSceneControl.SenderSlot;
                    data["animSceneControlReason"] =
                        animSceneControl.Reason.ToString();
                    data["animSceneControlFlags"] =
                        animSceneControl.Flags.ToString();
                    data["playAtHostTick"] =
                        animSceneControl.PlayAtHostTick;
                    break;
                }
                case MessageType.EntitySpawn:
                case MessageType.EntityUpdate:
                {
                    var entity = BinaryPayloadCodec.DecodeWorldEntityState(
                        envelope.Payload.Span);
                    data["entityId"] = entity.EntityId.Value;
                    data["modelHash"] = $"0x{entity.ModelHash:X8}";
                    data["parentEntityId"] = entity.ParentEntityId.Value;
                    data["entityFlags"] = entity.Flags.ToString();
                    data["taskKind"] = entity.TaskKind.ToString();
                    break;
                }
                case MessageType.EntityDespawn:
                    data["entityId"] =
                        BinaryPayloadCodec.DecodeEntityDespawn(
                            envelope.Payload.Span).EntityId.Value;
                    break;
                case MessageType.DamageIntent:
                {
                    var damage = BinaryPayloadCodec.DecodeDamageIntent(
                        envelope.Payload.Span);
                    data["attackerEntityId"] = damage.AttackerId.Value;
                    data["targetEntityId"] = damage.TargetId.Value;
                    data["shotSequence"] = damage.ShotSequence;
                    data["weaponHash"] = $"0x{damage.WeaponHash:X8}";
                    break;
                }
                case MessageType.InteractionIntent:
                {
                    var intent = BinaryPayloadCodec.DecodeInteractionIntent(
                        envelope.Payload.Span);
                    data["interactionId"] = intent.InteractionId;
                    data["interactionRevision"] = intent.Revision;
                    data["interactionKind"] = intent.Kind.ToString();
                    data["interactionPhase"] = intent.Phase.ToString();
                    data["actorEntityId"] = intent.ActorEntityId.Value;
                    data["targetEntityId"] = intent.TargetEntityId.Value;
                    break;
                }
                case MessageType.InteractionResult:
                {
                    var result = BinaryPayloadCodec.DecodeInteractionResult(
                        envelope.Payload.Span);
                    data["interactionId"] = result.InteractionId;
                    data["interactionRevision"] = result.Revision;
                    data["interactionKind"] = result.Kind.ToString();
                    data["interactionStatus"] = result.Status.ToString();
                    data["interactionRejectReason"] =
                        result.RejectReason.ToString();
                    data["progressMs"] = result.ProgressMilliseconds;
                    data["requiredMs"] = result.RequiredDurationMilliseconds;
                    break;
                }
                case MessageType.RestraintState:
                {
                    var restraint = BinaryPayloadCodec.DecodeRestraintState(
                        envelope.Payload.Span);
                    data["subjectEntityId"] = restraint.SubjectEntityId.Value;
                    data["ownerEntityId"] = restraint.OwnerEntityId.Value;
                    data["restraintState"] = restraint.State.ToString();
                    data["restraintRevision"] = restraint.Revision;
                    break;
                }
            }
        }
        catch (ProtocolException)
        {
            data["structuredDecode"] = "invalid";
        }

        foreach (var (key, value) in extras)
        {
            data[key] = value;
        }
        return data;
    }

    // Turns a control packet into safe diagnostic text.
    // It describes identities and revisions without dumping raw payload bytes into normal logs.
    // Choose a short label describing the control message's kind and relevant IDs or phase for log messages.
    // This describes a message rather than executing the command or changing its payload.
    internal static string DescribeControlEnvelope(ProtocolEnvelope envelope)
    {
        if (envelope.Type == MessageType.PauseVote)
        {
            try
            {
                var pause = BinaryPayloadCodec.DecodePauseVote(
                    envelope.Payload.Span);
                return $"{envelope.Type}/{pause.Kind}";
            }
            catch (ProtocolException)
            {
                return $"{envelope.Type}/Invalid";
            }
        }

        if (envelope.Type == MessageType.CampaignCapability)
        {
            try
            {
                var capability = BinaryPayloadCodec.DecodeCampaignCapability(envelope.Payload.Span);
                return $"{envelope.Type}/{capability.Kind}/0x{capability.RecordHash:X8}/event={capability.HostEventId}";
            }
            catch (ProtocolException) { return $"{envelope.Type}/Invalid"; }
        }
        if (envelope.Type == MessageType.CampaignCapabilityAck)
        {
            var acknowledgement = BinaryPayloadCodec.DecodeCampaignCapabilityAck(envelope.Payload.Span);
            return $"{envelope.Type}/{acknowledgement.Kind}/0x{acknowledgement.RecordHash:X8}/event={acknowledgement.HostEventId}";
        }

        if (envelope.Type == MessageType.PlayerAction)
        {
            try
            {
                var action = BinaryPayloadCodec.DecodePlayerAction(
                    envelope.Payload.Span);
                return $"{envelope.Type}/{action.Kind}/{action.Phase}";
            }
            catch (ProtocolException)
            {
                return $"{envelope.Type}/Invalid";
            }
        }

        if (envelope.Type == MessageType.InteractionIntent)
        {
            try
            {
                var intent = BinaryPayloadCodec.DecodeInteractionIntent(
                    envelope.Payload.Span);
                return $"{envelope.Type}/{intent.Kind}/{intent.Phase}";
            }
            catch (ProtocolException)
            {
                return $"{envelope.Type}/Invalid";
            }
        }

        if (envelope.Type == MessageType.InteractionResult)
        {
            try
            {
                var result = BinaryPayloadCodec.DecodeInteractionResult(
                    envelope.Payload.Span);
                return $"{envelope.Type}/{result.Kind}/{result.Status}";
            }
            catch (ProtocolException)
            {
                return $"{envelope.Type}/Invalid";
            }
        }

        if (envelope.Type == MessageType.MissionState)
        {
            try
            {
                var mission = BinaryPayloadCodec.DecodeMissionState(
                    envelope.Payload.Span);
                return $"{envelope.Type}/{mission.Phase}/" +
                    $"epoch={mission.MissionEpoch}/" +
                    $"revision={mission.Revision}/" +
                    $"checkpoint={mission.CheckpointGeneration}/" +
                    $"flags=0x{(byte)mission.Flags:X2}";
            }
            catch (ProtocolException)
            {
                return $"{envelope.Type}/Invalid";
            }
        }

        if (envelope.Type == MessageType.MissionCinematicState)
        {
            try
            {
                var state = BinaryPayloadCodec.DecodeMissionCinematicState(
                    envelope.Payload.Span);
                return $"{envelope.Type}/{state.Phase}/" +
                    $"epoch={state.MissionEpoch}/" +
                    $"generation={state.CinematicGeneration}/" +
                    $"revision={state.Revision}/" +
                    $"flags=0x{(ushort)state.Flags:X4}";
            }
            catch (ProtocolException)
            {
                return $"{envelope.Type}/Invalid";
            }
        }

        if (envelope.Type == MessageType.MissionCinematicAction)
        {
            try
            {
                var action = BinaryPayloadCodec.DecodeMissionCinematicAction(
                    envelope.Payload.Span);
                return $"{envelope.Type}/{action.Kind}/" +
                    $"generation={action.CinematicGeneration}/" +
                    $"action={action.ActionId}";
            }
            catch (ProtocolException)
            {
                return $"{envelope.Type}/Invalid";
            }
        }

        if (envelope.Type == MessageType.AnimSceneDefinition)
        {
            try
            {
                var definition = BinaryPayloadCodec.DecodeAnimSceneDefinition(
                    envelope.Payload.Span);
                return $"{envelope.Type}/" +
                    $"generation={definition.CinematicGeneration}/" +
                    $"revision={definition.DefinitionRevision}/" +
                    $"roles={definition.Roles.Length}";
            }
            catch (ProtocolException)
            {
                return $"{envelope.Type}/Invalid";
            }
        }

        if (envelope.Type == MessageType.AnimSceneControl)
        {
            try
            {
                var control = BinaryPayloadCodec.DecodeAnimSceneControl(
                    envelope.Payload.Span);
                return $"{envelope.Type}/{control.Kind}/" +
                    $"generation={control.CinematicGeneration}/" +
                    $"revision={control.DefinitionRevision}/" +
                    $"action={control.ActionId}";
            }
            catch (ProtocolException)
            {
                return $"{envelope.Type}/Invalid";
            }
        }

        if (envelope.Type == MessageType.PlayerAppearanceState)
        {
            try
            {
                var appearance =
                    BinaryPayloadCodec.DecodePlayerAppearanceState(
                        envelope.Payload.Span);
                return $"{envelope.Type}/slot={appearance.Slot}/" +
                    $"revision={appearance.Revision}/" +
                    $"components={appearance.ComponentHashes.Length}";
            }
            catch (ProtocolException)
            {
                return $"{envelope.Type}/Invalid";
            }
        }

        if (envelope.Type != MessageType.Command)
        {
            return envelope.Type.ToString();
        }

        try
        {
            var command = BinaryPayloadCodec.DecodeCommand(envelope.Payload.Span);
            return $"{envelope.Type}/{command.Opcode}";
        }
        catch (ProtocolException)
        {
            return $"{envelope.Type}/Invalid";
        }
    }

    // Confirms an incoming control payload has the exact known binary layout before any role/state code attempts to read fields from it.
    // Decode the listed binary control types solely to run their payload-format and value checks.
    // Discarding the decoded record does not undo those checks, and routing or role permission is checked separately.
    internal static void ValidateBinaryControlPayload(ProtocolEnvelope envelope)
    {
        switch (envelope.Type)
        {
            case MessageType.Command:
                _ = BinaryPayloadCodec.DecodeCommand(envelope.Payload.Span);
                break;
            case MessageType.EntitySpawn:
            case MessageType.EntityUpdate:
                _ = BinaryPayloadCodec.DecodeWorldEntityState(
                    envelope.Payload.Span);
                break;
            case MessageType.EntityDespawn:
                _ = BinaryPayloadCodec.DecodeEntityDespawn(
                    envelope.Payload.Span);
                break;
            case MessageType.DamageIntent:
                _ = BinaryPayloadCodec.DecodeDamageIntent(
                    envelope.Payload.Span);
                break;
            case MessageType.WorldState:
                _ = BinaryPayloadCodec.DecodeWorldState(envelope.Payload.Span);
                break;
            case MessageType.MissionState:
                _ = BinaryPayloadCodec.DecodeMissionState(envelope.Payload.Span);
                break;
            case MessageType.MissionCameraState:
                _ = BinaryPayloadCodec.DecodeMissionCameraState(
                    envelope.Payload.Span);
                break;
            case MessageType.AnimSceneReplicaState:
                _ = BinaryPayloadCodec.DecodeAnimSceneReplicaState(
                    envelope.Payload.Span);
                break;
            case MessageType.AnimSceneDefinition:
                _ = BinaryPayloadCodec.DecodeAnimSceneDefinition(
                    envelope.Payload.Span);
                break;
            case MessageType.AnimSceneControl:
                _ = BinaryPayloadCodec.DecodeAnimSceneControl(
                    envelope.Payload.Span);
                break;
            case MessageType.MissionCinematicState:
                _ = BinaryPayloadCodec.DecodeMissionCinematicState(
                    envelope.Payload.Span);
                break;
            case MessageType.MissionCinematicAction:
                _ = BinaryPayloadCodec.DecodeMissionCinematicAction(
                    envelope.Payload.Span);
                break;
            case MessageType.EquipmentState:
                _ = BinaryPayloadCodec.DecodeEquipmentState(envelope.Payload.Span);
                break;
            case MessageType.CampaignCapability:
                _ = BinaryPayloadCodec.DecodeCampaignCapability(envelope.Payload.Span);
                break;
            case MessageType.CampaignCapabilityAck:
                _ = BinaryPayloadCodec.DecodeCampaignCapabilityAck(envelope.Payload.Span);
                break;
            case MessageType.PickupCollected:
                _ = BinaryPayloadCodec.DecodePickupCollected(envelope.Payload.Span);
                break;
            case MessageType.MissionProgression:
                _ = BinaryPayloadCodec.DecodeMissionProgression(envelope.Payload.Span);
                break;
            case MessageType.MissionObjective:
                _ = BinaryPayloadCodec.DecodeMissionObjective(envelope.Payload.Span);
                break;
            case MessageType.MissionDialogueCue:
                _ = BinaryPayloadCodec.DecodeMissionDialogueCue(envelope.Payload.Span);
                break;
            case MessageType.MissionDialogueReady:
                _ = BinaryPayloadCodec.DecodeMissionDialogueReady(envelope.Payload.Span);
                break;
            case MessageType.AmbientEncounterProposal:
                _ = BinaryPayloadCodec.DecodeAmbientEncounterProposal(envelope.Payload.Span);
                break;
            case MessageType.AmbientEncounterState:
                _ = BinaryPayloadCodec.DecodeAmbientEncounterState(envelope.Payload.Span);
                break;
            case MessageType.PlayerAppearanceState:
                _ = BinaryPayloadCodec.DecodePlayerAppearanceState(
                    envelope.Payload.Span);
                break;
            case MessageType.PauseVote:
                _ = BinaryPayloadCodec.DecodePauseVote(envelope.Payload.Span);
                break;
            case MessageType.PlayerMountState:
                _ = BinaryPayloadCodec.DecodePlayerMountState(
                    envelope.Payload.Span);
                break;
            case MessageType.PlayerTraversal:
                _ = BinaryPayloadCodec.DecodePlayerTraversal(
                    envelope.Payload.Span);
                break;
            case MessageType.PlayerAction:
                _ = BinaryPayloadCodec.DecodePlayerAction(
                    envelope.Payload.Span);
                break;
            case MessageType.PlayerAnimationState:
                _ = AnimationReplicationPayloadCodec.DecodePlayerAnimationState(
                    envelope.Payload.Span);
                break;
            case MessageType.MotionReplicationConfig:
                _ = AnimationReplicationPayloadCodec.DecodeMotionReplicationConfig(
                    envelope.Payload.Span);
                break;
            case MessageType.DownedState:
            case MessageType.SpectatorState:
                _ = BinaryPayloadCodec.DecodeDownedState(envelope.Payload.Span);
                break;
            case MessageType.ReviveRequest:
                _ = BinaryPayloadCodec.DecodeReviveRequest(envelope.Payload.Span);
                break;
            case MessageType.ReviveComplete:
                _ = BinaryPayloadCodec.DecodeReviveComplete(envelope.Payload.Span);
                break;
            case MessageType.InteractionIntent:
                _ = BinaryPayloadCodec.DecodeInteractionIntent(
                    envelope.Payload.Span);
                break;
            case MessageType.InteractionResult:
                _ = BinaryPayloadCodec.DecodeInteractionResult(
                    envelope.Payload.Span);
                break;
            case MessageType.RestraintState:
                _ = BinaryPayloadCodec.DecodeRestraintState(
                    envelope.Payload.Span);
                break;
        }
    }

    // True for high-rate snapshots where only the newest value matters.
    // These can replace older queued copies without losing a required game event.
    // Identify state messages for which a newer sample can stand in for an older pending sample.
    // The watchdog also uses this list when choosing how long a blocked game-pipe write may wait.
    internal static bool IsReplaceableBridgeDeliveryType(
        MessageType? messageType) =>
        messageType is
            MessageType.PlayerState or
            MessageType.PlayerAnimationState or
            MessageType.MissionCameraState or
            MessageType.AnimSceneReplicaState or
            MessageType.PlayerMountState or
            MessageType.WorldState or
            MessageType.EquipmentState or
            MessageType.PlayerIdentity or
            MessageType.PlayerAppearanceState or
            MessageType.EntityUpdate;

    // Expose mission epoch, revision, checkpoint generation, and flags as readable diagnostic fields.
    // These identifiers help spot old mission state being compared with a newer mission or checkpoint.
    private Dictionary<string, object?>
        CreateMissionStateDiagnosticsData(
            ProtocolEnvelope envelope,
            MissionStatePayload state,
            params (string Key, object? Value)[] additional)
    {
        var data = new Dictionary<string, object?>
        {
            ["sequence"] = envelope.Sequence,
            ["tick"] = envelope.Tick,
            ["sessionFingerprint"] = string.IsNullOrEmpty(
                _sessionFingerprint)
                ? null
                : _sessionFingerprint,
            ["hostEntityId"] = state.HostEntityId.Value,
            ["missionEpoch"] = state.MissionEpoch,
            ["revision"] = state.Revision,
            ["checkpointGeneration"] = state.CheckpointGeneration,
            ["phase"] = state.Phase.ToString(),
            ["flags"] = $"0x{(byte)state.Flags:X2}",
            ["missionActive"] =
                state.Flags.HasFlag(MissionStateFlags.MissionActive),
            ["checkpointRecovery"] =
                state.Flags.HasFlag(MissionStateFlags.CheckpointRecovery),
            ["anchorValid"] =
                state.Flags.HasFlag(MissionStateFlags.AnchorValid)
        };
        foreach (var (key, value) in additional)
        {
            data[key] = value;
        }
        return data;
    }

    // Quick sender/type rule: says whether this session role may send this category of message at all before its contents are trusted.
    // Apply the broad incoming-message rule for our local role before examining payload-specific ownership.
    // For example, a host must not accept the guest as the source of the shared NPC list.
    internal static bool IsPeerMessageAuthorized(
        SessionRole localRole,
        MessageType messageType)
    {
        if (messageType == MessageType.CampaignCapability)
        {
            return localRole == SessionRole.Guest;
        }
        if (messageType == MessageType.CampaignCapabilityAck)
        {
            return localRole == SessionRole.Host;
        }
        if (messageType == MessageType.PickupCollected)
        {
            return localRole == SessionRole.Host;
        }
        if (messageType == MessageType.MotionReplicationConfig)
        {
            return false;
        }
        if (messageType == MessageType.AnimSceneDefinition)
        {
            return localRole == SessionRole.Guest;
        }
        if (messageType == MessageType.AnimSceneControl)
        {
            return localRole is SessionRole.Host or SessionRole.Guest;
        }

        return localRole switch
        {
            SessionRole.Host =>
                messageType is not (
                    MessageType.WorldState or
                    MessageType.EntitySpawn or
                    MessageType.EntityUpdate or
                    MessageType.EntityDespawn or
                    MessageType.MissionState or
                    MessageType.MissionCinematicState or
                    MessageType.MissionCameraState or
                    MessageType.AnimSceneReplicaState or
                    MessageType.ReviveComplete or
                    MessageType.SpectatorState or
                    MessageType.InteractionResult or
                    MessageType.RestraintState),
            SessionRole.Guest =>
                messageType is not (
                    MessageType.DamageIntent or
                    MessageType.InteractionIntent or
                    MessageType.MissionCinematicAction),
            _ => false
        };
    }

    // This is the sidecar half of the native capability allowlist.
    // A new campaign record must be added here and in ScriptHookSdkFacade only after its game-native mapping has been independently proven.
    // Allow only the campaign record hashes explicitly verified here for multiplayer grant replay.
    // A recognized enum kind alone is not enough to make an arbitrary game record safe to grant.
    internal static bool IsSupportedCampaignCapability(
        CampaignCapabilityPayload capability) => capability.Kind switch
    {
        CampaignCapabilityKind.WeaponShopEligibility =>
            capability.RecordHash == 1_674_213_418U,
        CampaignCapabilityKind.Recipe =>
            capability.RecordHash == 0x366089E7U,
        _ => false
    };

    // Detailed peer validation.
    // It checks message direction, payload facts, and host-only ownership before a remote message can mutate cached state.
    // Check that an incoming message's claimed sender, action authority, or mission phase fits the peer's role.
    // Passing this permission check does not itself apply damage, start a mission, or confirm a request succeeded.
    internal static bool IsPeerEnvelopeAuthorized(
        SessionRole localRole,
        ProtocolEnvelope envelope)
    {
        if (!IsPeerMessageAuthorized(localRole, envelope.Type))
        {
            return false;
        }

        if (envelope.Type == MessageType.DamageApplied)
        {
            return localRole == SessionRole.Guest;
        }

        if (envelope.Type == MessageType.CampaignCapability)
        {
            return IsSupportedCampaignCapability(
                BinaryPayloadCodec.DecodeCampaignCapability(envelope.Payload.Span));
        }

        if (envelope.Type == MessageType.PauseVote)
        {
            var pause = BinaryPayloadCodec.DecodePauseVote(
                envelope.Payload.Span);
            return localRole switch
            {
                SessionRole.Host =>
                    pause.Kind == PauseVoteKind.RequestState &&
                    pause.VoterSlot == (byte)SessionRole.Guest,
                SessionRole.Guest =>
                    pause.Kind == PauseVoteKind.AuthoritativeState &&
                    pause.VoterSlot == (byte)SessionRole.Host,
                _ => false
            };
        }

        if (envelope.Type == MessageType.PlayerAction)
        {
            var action = BinaryPayloadCodec.DecodePlayerAction(
                envelope.Payload.Span);
            return localRole switch
            {
                SessionRole.Host =>
                    action.Flags.HasFlag(PlayerActionFlags.Intent) &&
                    action.ActorSlot == (byte)SessionRole.Guest &&
                    action.AuthoritySlot == (byte)SessionRole.Guest,
                SessionRole.Guest =>
                    action.Flags.HasFlag(PlayerActionFlags.Authoritative) &&
                    action.AuthoritySlot == (byte)SessionRole.Host,
                _ => false
            };
        }

        if (envelope.Type == MessageType.InteractionIntent)
        {
            var intent = BinaryPayloadCodec.DecodeInteractionIntent(
                envelope.Payload.Span);
            return localRole == SessionRole.Host &&
                intent.ActorSlot == (byte)SessionRole.Guest;
        }

        if (envelope.Type is MessageType.InteractionResult or
            MessageType.RestraintState)
        {
            return localRole == SessionRole.Guest;
        }

        if (envelope.Type == MessageType.MissionCinematicAction)
        {
            var action = BinaryPayloadCodec.DecodeMissionCinematicAction(
                envelope.Payload.Span);
            return localRole == SessionRole.Host &&
                action.SenderSlot == (byte)SessionRole.Guest;
        }

        // The phase chooses which side is allowed to send this particular mission message.
        // This permission check does not automatically advance through the MissionProgressionPhase enum values.
        if (envelope.Type == MessageType.MissionProgression)
        {
            var progression = BinaryPayloadCodec.DecodeMissionProgression(
                envelope.Payload.Span);
            if (progression.Phase == MissionProgressionPhase.Completion &&
                progression.CompletionCashAward != 0)
            {
                return false;
            }
            return localRole switch
            {
                SessionRole.Host => progression.Phase is MissionProgressionPhase.Eligibility or MissionProgressionPhase.Applied or MissionProgressionPhase.GuestInstanceStarted or MissionProgressionPhase.GuestInstanceRejected,
                SessionRole.Guest => progression.Phase is MissionProgressionPhase.Offer or MissionProgressionPhase.Completion or MissionProgressionPhase.StartBarrierOpen or MissionProgressionPhase.StartBarrierReleased or MissionProgressionPhase.StartBarrierAborted,
                _ => false
            };
        }

        if (envelope.Type == MessageType.MissionObjective)
            return localRole == SessionRole.Guest;

        // Dialogue is strictly directional.
        // The cue is a host-owned, read-only observation; the guest may only return readiness for that exact tuple.
        // Neither frame can be used to name arbitrary game audio.
        if (envelope.Type == MessageType.MissionDialogueCue)
            return localRole == SessionRole.Guest;

        if (envelope.Type == MessageType.MissionDialogueReady)
            return localRole == SessionRole.Host;

        // A guest may only propose an encounter; the host alone can publish the adopted instance or a rejection.
        if (envelope.Type == MessageType.AmbientEncounterProposal)
            return localRole == SessionRole.Host;

        if (envelope.Type == MessageType.AmbientEncounterState)
            return localRole == SessionRole.Guest;

        if (envelope.Type == MessageType.PlayerAppearanceState)
        {
            var appearance =
                BinaryPayloadCodec.DecodePlayerAppearanceState(
                    envelope.Payload.Span);
            return appearance.Slot != (byte)localRole;
        }

        if (envelope.Type == MessageType.AnimSceneReplicaState)
        {
            return localRole == SessionRole.Guest;
        }

        if (envelope.Type == MessageType.AnimSceneDefinition)
        {
            return localRole == SessionRole.Guest;
        }

        if (envelope.Type == MessageType.AnimSceneControl)
        {
            var control = BinaryPayloadCodec.DecodeAnimSceneControl(
                envelope.Payload.Span);
            var peerRole = localRole switch
            {
                SessionRole.Host => SessionRole.Guest,
                SessionRole.Guest => SessionRole.Host,
                _ => (SessionRole?)null
            };
            return peerRole.HasValue &&
                IsAnimSceneControlFromRole(control, peerRole.Value);
        }

        if (TryDecodeDiagnosticMarker(envelope, out var diagnosticMarker))
        {
            var expectedOrigin = localRole switch
            {
                SessionRole.Host => SessionRole.Guest,
                SessionRole.Guest => SessionRole.Host,
                _ => (SessionRole?)null
            };
            return expectedOrigin.HasValue &&
                IsValidDiagnosticMarker(
                    diagnosticMarker,
                    expectedOrigin.Value);
        }

        if (envelope.Type != MessageType.Command)
        {
            return true;
        }

        if (localRole != SessionRole.Guest)
        {
            return false;
        }

        var command = BinaryPayloadCodec.DecodeCommand(envelope.Payload.Span);
        return IsHostAuthoritativeCommand(command.Opcode);
    }

    // Equivalent role/type rule for the local RDR2 Bridge.
    // The Bridge cannot bypass Sidecar authority merely because it shares the same computer.
    // Apply the broad outgoing-message rule for the local game's role.
    // These directions are the local-game counterpart of the incoming peer rules.
    internal static bool IsLocalBridgeMessageAuthorized(
        SessionRole localRole,
        MessageType messageType)
    {
        if (messageType == MessageType.CampaignCapability)
        {
            return localRole == SessionRole.Host;
        }
        if (messageType == MessageType.CampaignCapabilityAck)
        {
            return localRole == SessionRole.Guest;
        }
        if (messageType == MessageType.MissionProgression)
        {
            return localRole is SessionRole.Host or SessionRole.Guest;
        }
        if (messageType == MessageType.MotionReplicationConfig)
        {
            return false;
        }
        if (messageType == MessageType.AnimSceneDefinition)
        {
            return localRole == SessionRole.Host;
        }
        if (messageType == MessageType.AnimSceneControl)
        {
            return localRole is SessionRole.Host or SessionRole.Guest;
        }

        return localRole switch
        {
            SessionRole.Host =>
                messageType is not (
                    MessageType.DamageIntent or
                    MessageType.InteractionResult or
                    MessageType.RestraintState or
                    MessageType.MissionCinematicAction),
            SessionRole.Guest =>
                messageType is not (
                    MessageType.WorldState or
                    MessageType.EntitySpawn or
                    MessageType.EntityUpdate or
                    MessageType.EntityDespawn or
                    MessageType.MissionState or
                    MessageType.MissionCinematicState or
                    MessageType.MissionCameraState or
                    MessageType.AnimSceneReplicaState or
                    MessageType.ReviveComplete or
                    MessageType.SpectatorState or
                    MessageType.InteractionResult or
                    MessageType.RestraintState),
            _ => false
        };
    }

    // Detailed validation for a local Bridge message before it is forwarded to a peer or used to change Sidecar's saved authoritative state.
    // Check payload-specific ownership before accepting a message emitted by our own game bridge.
    // A guest can propose actions, but host-approved outcomes must come from the host side.
    internal static bool IsLocalBridgeEnvelopeAuthorized(
        SessionRole localRole,
        ProtocolEnvelope envelope)
    {
        if (!IsLocalBridgeMessageAuthorized(localRole, envelope.Type))
        {
            return false;
        }

        if (envelope.Type == MessageType.DamageApplied)
        {
            return localRole == SessionRole.Host;
        }

        if (envelope.Type == MessageType.CampaignCapability)
        {
            return IsSupportedCampaignCapability(
                BinaryPayloadCodec.DecodeCampaignCapability(envelope.Payload.Span));
        }

        if (envelope.Type == MessageType.PauseVote)
        {
            var pause = BinaryPayloadCodec.DecodePauseVote(
                envelope.Payload.Span);
            return localRole switch
            {
                SessionRole.Host =>
                    pause.Kind == PauseVoteKind.AuthoritativeState &&
                    pause.VoterSlot == (byte)SessionRole.Host,
                SessionRole.Guest =>
                    pause.Kind == PauseVoteKind.RequestState &&
                    pause.VoterSlot == (byte)SessionRole.Guest,
                _ => false
            };
        }

        if (envelope.Type == MessageType.PlayerAction)
        {
            var action = BinaryPayloadCodec.DecodePlayerAction(
                envelope.Payload.Span);
            return localRole switch
            {
                SessionRole.Host =>
                    action.Flags.HasFlag(PlayerActionFlags.Authoritative) &&
                    action.AuthoritySlot == (byte)SessionRole.Host,
                SessionRole.Guest =>
                    action.Flags.HasFlag(PlayerActionFlags.Intent) &&
                    action.ActorSlot == (byte)SessionRole.Guest &&
                    action.AuthoritySlot == (byte)SessionRole.Guest,
                _ => false
            };
        }

        if (envelope.Type == MessageType.InteractionIntent)
        {
            var intent = BinaryPayloadCodec.DecodeInteractionIntent(
                envelope.Payload.Span);
            return intent.ActorSlot == (byte)localRole;
        }

        if (envelope.Type == MessageType.MissionCinematicAction)
        {
            var action = BinaryPayloadCodec.DecodeMissionCinematicAction(
                envelope.Payload.Span);
            return localRole == SessionRole.Guest &&
                action.SenderSlot == (byte)SessionRole.Guest;
        }

        // Outgoing host phases and guest reply phases are checked separately even though they use the same payload type.
        if (envelope.Type == MessageType.MissionProgression)
        {
            var progression = BinaryPayloadCodec.DecodeMissionProgression(
                envelope.Payload.Span);
            if (progression.Phase == MissionProgressionPhase.Completion &&
                progression.CompletionCashAward != 0)
            {
                return false;
            }
            return localRole switch
            {
                SessionRole.Host => progression.Phase is MissionProgressionPhase.Offer or MissionProgressionPhase.Completion or MissionProgressionPhase.StartBarrierOpen or MissionProgressionPhase.StartBarrierReleased or MissionProgressionPhase.StartBarrierAborted,
                SessionRole.Guest => progression.Phase is MissionProgressionPhase.Eligibility or MissionProgressionPhase.Applied or MissionProgressionPhase.GuestInstanceStarted or MissionProgressionPhase.GuestInstanceRejected,
                _ => false
            };
        }

        if (envelope.Type == MessageType.MissionObjective)
            return localRole == SessionRole.Host;

        if (envelope.Type == MessageType.MissionDialogueCue)
            return localRole == SessionRole.Host;

        if (envelope.Type == MessageType.MissionDialogueReady)
            return localRole == SessionRole.Guest;

        if (envelope.Type == MessageType.AmbientEncounterProposal)
            return localRole == SessionRole.Guest;

        if (envelope.Type == MessageType.AmbientEncounterState)
            return localRole == SessionRole.Host;

        if (envelope.Type == MessageType.PlayerAppearanceState)
        {
            var appearance =
                BinaryPayloadCodec.DecodePlayerAppearanceState(
                    envelope.Payload.Span);
            return appearance.Slot == (byte)localRole;
        }

        if (envelope.Type == MessageType.AnimSceneReplicaState)
        {
            return localRole == SessionRole.Host;
        }

        if (envelope.Type == MessageType.AnimSceneDefinition)
        {
            return localRole == SessionRole.Host;
        }

        if (envelope.Type == MessageType.AnimSceneControl)
        {
            var control = BinaryPayloadCodec.DecodeAnimSceneControl(
                envelope.Payload.Span);
            return IsAnimSceneControlFromRole(control, localRole);
        }

        if (TryDecodeDiagnosticMarker(envelope, out var diagnosticMarker))
        {
            return IsValidDiagnosticMarker(diagnosticMarker, localRole);
        }

        if (envelope.Type != MessageType.Command)
        {
            return true;
        }

        if (localRole != SessionRole.Host)
        {
            return false;
        }

        var command = BinaryPayloadCodec.DecodeCommand(envelope.Payload.Span);
        return IsHostAuthoritativeCommand(command.Opcode);
    }

    // Require both the claimed sender slot and the scene-control kind to match the expected role.
    // The host commits playback or aborts it, while the guest reports ready or rejected.
    private static bool IsAnimSceneControlFromRole(
        AnimSceneControlPayload control,
        SessionRole senderRole) =>
        control.SenderSlot == (byte)senderRole &&
        (senderRole switch
        {
            SessionRole.Host =>
                control.Kind is AnimSceneControlKind.HostPlayCommit or
                    AnimSceneControlKind.HostAbort,
            SessionRole.Guest =>
                control.Kind is AnimSceneControlKind.GuestReady or
                    AnimSceneControlKind.GuestRejected,
            _ => false
        });

    // Describe a campaign grant's kind, record hash, host event ID, and time without applying the grant.
    private static Dictionary<string, object?> CreateCapabilityDiagnosticsData(
        ProtocolEnvelope envelope,
        string direction)
    {
        var capability = BinaryPayloadCodec.DecodeCampaignCapability(
            envelope.Payload.Span);
        return new Dictionary<string, object?>
        {
            ["messageType"] = envelope.Type.ToString(),
            ["direction"] = direction,
            ["kind"] = capability.Kind.ToString(),
            ["recordHash"] = $"0x{capability.RecordHash:X8}",
            ["hostEventId"] = capability.HostEventId,
            ["grantedAtUnixMilliseconds"] =
                capability.GrantedAtUnixMilliseconds
        };
    }

    // Lists bridge commands only the host may issue because they change shared world/session state rather than the guest's own local presentation.
    // List gameplay command opcodes that the host is allowed to originate for the guest.
    // Diagnostic markers have their own validation path and are not added to this gameplay-command list.
    private static bool IsHostAuthoritativeCommand(
        CommandOpcode opcode) =>
        opcode is
            CommandOpcode.SpectatorOn or
            CommandOpcode.SpectatorOff or
            CommandOpcode.TeleportGuest or
            CommandOpcode.EnterDowned or
            CommandOpcode.CompleteRevive or
            CommandOpcode.RetryCheckpoint or
            CommandOpcode.SoloOverrideOn or
            CommandOpcode.SoloOverrideOff or
            CommandOpcode.Resync or
            CommandOpcode.ResyncEquipment;

    // Recognize a diagnostic-marker command and return its decoded fields for logging or origin checks.
    private static bool TryDecodeDiagnosticMarker(
        ProtocolEnvelope envelope,
        out CommandPayload command)
    {
        command = default;
        if (envelope.Type != MessageType.Command)
        {
            return false;
        }
        command = BinaryPayloadCodec.DecodeCommand(envelope.Payload.Span);
        return command.Opcode == CommandOpcode.DiagnosticMarker;
    }

    // Check the marker's nonzero IDs and embedded origin role against the endpoint expected to have created it.
    private static bool IsValidDiagnosticMarker(
        CommandPayload command,
        SessionRole expectedOrigin) =>
        command.Opcode == CommandOpcode.DiagnosticMarker &&
        command.TargetEntityId.IsValid &&
        DiagnosticMarkerLocalId(command) != 0 &&
        DiagnosticMarkerOrigin(command) == expectedOrigin;

    // Read the four origin bits starting at bit 48 of the marker ID and translate them into host or guest.
    private static SessionRole? DiagnosticMarkerOrigin(
        CommandPayload command) =>
        ((command.TargetEntityId.Value >> 48) & 0x0FUL) switch
        {
            1UL => SessionRole.Host,
            2UL => SessionRole.Guest,
            _ => null
        };

    // Keep the lowest 24 bits of the marker ID to recover the originating player's local marker number.
    private static uint DiagnosticMarkerLocalId(
        CommandPayload command) =>
        (uint)(command.TargetEntityId.Value & 0x00FF_FFFFUL);

    // Stops safely if RDR2 Online is detected.
    // This project supports Story Mode co-op only and must not send multiplayer control into Online mode.
    // Ask the local bridge to unload when it reports Online mode, then record the safety refusal.
    // The caller stops forwarding player state because this co-op bridge is intended for Story Mode.
    private async Task RefuseOnlineModeAsync(CancellationToken cancellationToken)
    {
        var command = new CommandPayload(
            CommandOpcode.Unload,
            CommandFlags.Force,
            NetEntityId.None,
            System.Numerics.Vector3.Zero,
            Heading: 0,
            Value: 0);
        var envelope = new ProtocolEnvelope(
            MessageType.Command,
            0,
            NetworkClock.Tick,
            BinaryPayloadCodec.EncodeCommand(command));
        if (_bridge.TryCaptureConnection(out var bridgeConnection))
        {
            _ = await _bridge.SendAsync(
                    bridgeConnection,
                    envelope,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        await _logger.ErrorAsync(
            "safety.online-mode-refused",
            "Online mode flag was set by the bridge; requested bridge unload.",
            cancellationToken: cancellationToken).ConfigureAwait(false);
    }

    // Turn an optional disconnect reason into readable text and limit how many payload bytes enter the log.
    private static string DecodeGoodbyeReason(ReadOnlySpan<byte> payload)
    {
        if (payload.IsEmpty)
        {
            return "no reason supplied";
        }

        const int maximumLoggedBytes = 512;
        var length = Math.Min(payload.Length, maximumLoggedBytes);
        var reason = Encoding.UTF8.GetString(payload[..length]).Trim();
        if (string.IsNullOrEmpty(reason))
        {
            return "no reason supplied";
        }

        return payload.Length > maximumLoggedBytes
            ? reason + "..."
            : reason;
    }
}

/// <summary>
/// Proves that the exact remote player entity used by a peer authority request has already crossed the named pipe for the same peer and bridge generation.
/// The lease is intentionally short: normal PlayerState traffic refreshes it, while a stalled or resyncing mapping fails closed.
/// </summary>
// Remembers which remote peer identity is mapped to the currently open RDR2 Bridge generation, preventing queued pre-reconnect messages crossing over.
// Remember recent evidence that a particular guest player was written to a particular host game connection.
// This short-lived permission check blocks actions that would target an actor the current host bridge may not know yet.
internal sealed class RemoteBridgeMappingGate
{
    private sealed record PendingResync(
        ILanSession Network,
        ControlPeerToken Peer);

    private sealed record Lease(
        ILanSession Network,
        ControlPeerToken Peer,
        BridgePipeConnectionToken Bridge,
        NetEntityId EntityId,
        long DeliveredAtMilliseconds);

    private readonly object _sync = new();
    private Lease? _lease;
    private PendingResync? _pendingResync;

    // Remember which guest actor was delivered to which peer and bridge connection, along with the delivery time.
    // Do not accept fresh mapping evidence for that peer while its resync reset is still pending.
    public void MarkDelivered(
        ILanSession network,
        ControlPeerToken peer,
        BridgePipeConnectionToken bridge,
        NetEntityId entityId,
        long deliveredAtMilliseconds)
    {
        ArgumentNullException.ThrowIfNull(network);
        if (!peer.IsValid)
        {
            throw new ArgumentException(
                "Peer mapping token must be valid.",
                nameof(peer));
        }
        if (!bridge.IsValid)
        {
            throw new ArgumentException(
                "Bridge mapping token must be valid.",
                nameof(bridge));
        }
        if (!entityId.IsValid)
        {
            throw new ArgumentException(
                "Remote mapping entity must be valid.",
                nameof(entityId));
        }

        lock (_sync)
        {
            if (_pendingResync is { } pending &&
                ReferenceEquals(pending.Network, network) &&
                pending.Peer == peer)
            {
                return;
            }
            _lease = new Lease(
                network,
                peer,
                bridge,
                entityId,
                deliveredAtMilliseconds);
        }
    }

    // Require matching network, peer, bridge, and entity IDs plus a recent delivery time before trusting this mapping.
    // Pending resync temporarily blocks the mapping even if all the IDs match.
    public bool IsCurrent(
        ILanSession network,
        ControlPeerToken peer,
        BridgePipeConnectionToken bridge,
        NetEntityId entityId,
        long nowMilliseconds,
        long maximumAgeMilliseconds)
    {
        ArgumentNullException.ThrowIfNull(network);
        if (maximumAgeMilliseconds < 0)
        {
            throw new ArgumentOutOfRangeException(
                nameof(maximumAgeMilliseconds));
        }

        lock (_sync)
        {
            return !(_pendingResync is { } pending &&
                    ReferenceEquals(pending.Network, network) &&
                    pending.Peer == peer) &&
                _lease is { } lease &&
                ReferenceEquals(lease.Network, network) &&
                lease.Peer == peer &&
                lease.Bridge == bridge &&
                lease.EntityId == entityId &&
                nowMilliseconds >= lease.DeliveredAtMilliseconds &&
                nowMilliseconds - lease.DeliveredAtMilliseconds <=
                    maximumAgeMilliseconds;
        }
    }

    // Forget a stored mapping only when it belongs to this exact network and peer pair.
    public bool ClearMatching(
        ILanSession network,
        ControlPeerToken peer)
    {
        ArgumentNullException.ThrowIfNull(network);
        lock (_sync)
        {
            if (_lease is not { } lease ||
                !ReferenceEquals(lease.Network, network) ||
                lease.Peer != peer)
            {
                return false;
            }
            _lease = null;
            return true;
        }
    }

    // Mark this peer as resetting and remove its existing mapping evidence so physical actions wait for a rebuild.
    public void BeginResync(
        ILanSession network,
        ControlPeerToken peer)
    {
        ArgumentNullException.ThrowIfNull(network);
        lock (_sync)
        {
            _pendingResync = new PendingResync(network, peer);
            if (_lease is { } lease &&
                ReferenceEquals(lease.Network, network) &&
                lease.Peer == peer)
            {
                _lease = null;
            }
        }
    }

    // End only the matching pending reset and leave the mapping empty until a new player delivery proves it again.
    public bool CompleteResync(
        ILanSession network,
        ControlPeerToken peer)
    {
        ArgumentNullException.ThrowIfNull(network);
        lock (_sync)
        {
            if (_pendingResync is not { } pending ||
                !ReferenceEquals(pending.Network, network) ||
                pending.Peer != peer)
            {
                return false;
            }
            _pendingResync = null;
            if (_lease is { } lease &&
                ReferenceEquals(lease.Network, network) &&
                lease.Peer == peer)
            {
                _lease = null;
            }
            return true;
        }
    }

    // Forget both mapping evidence and any pending reset when the surrounding connection state is cleared.
    public void Clear()
    {
        lock (_sync)
        {
            _lease = null;
            _pendingResync = null;
        }
    }
}

// Pair an approved control-message type with its payload bytes while a host decision is being delivered.
internal readonly record struct PeerAuthorityControlFrame(
    MessageType Type,
    ReadOnlyMemory<byte> Payload);

// Keep the approved player action and any associated restraint change together as one resolution result.
internal readonly record struct PeerGuestPlayerActionAuthorityResolution(
    PlayerActionPayload? Resolved,
    RestraintStatePayload? Restraint);

/// <summary>
/// Apply a host decision triggered by a guest request using the same captured game and peer connections throughout.
/// Prevent the local bridge session token from being rotated while this operation is running.
/// Restore the previous registry state if delivery fails before any control message reaches the local pipe.
/// </summary>
internal static class PeerBridgeAuthorityTransaction
{
    // Treat a guest-triggered host decision and its resulting messages as one connection-bound operation.
    // If nothing reached the local bridge, a failure can restore the old registry state before the peer sees a decision.
    public static async ValueTask<bool>
        RunAsync<TMutation, TControl, TRollback>(
            BridgeSessionGenerationGate bridgeBoundary,
            PeerControlSendGate peerGate,
            ILanSession sourceNetwork,
            ControlPeerToken sourcePeer,
            Func<ILanSession?> captureNetwork,
            Func<ILanSession, ControlPeerToken, bool> isNegotiated,
            Func<BridgePipeConnectionToken?> captureReadyBridge,
            Func<BridgePipeConnectionToken, bool> isReadyBridge,
            Func<
                ILanSession,
                ControlPeerToken,
                BridgePipeConnectionToken,
                bool> isRemoteMappingReady,
            Func<TRollback> captureRollbackState,
            Func<TMutation> mutate,
            Action<TRollback> rollback,
            Func<TMutation, IReadOnlyList<TControl>> createControls,
            Func<
                BridgePipeConnectionToken,
                TControl,
                CancellationToken,
                ValueTask<bool>> deliverBridge,
            Func<
                ILanSession,
                ControlPeerToken,
                TControl,
                CancellationToken,
                ValueTask<bool>> deliverPeer,
            CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(bridgeBoundary);
        ArgumentNullException.ThrowIfNull(peerGate);
        ArgumentNullException.ThrowIfNull(sourceNetwork);
        ArgumentNullException.ThrowIfNull(captureNetwork);
        ArgumentNullException.ThrowIfNull(isNegotiated);
        ArgumentNullException.ThrowIfNull(captureReadyBridge);
        ArgumentNullException.ThrowIfNull(isReadyBridge);
        ArgumentNullException.ThrowIfNull(isRemoteMappingReady);
        ArgumentNullException.ThrowIfNull(captureRollbackState);
        ArgumentNullException.ThrowIfNull(mutate);
        ArgumentNullException.ThrowIfNull(rollback);
        ArgumentNullException.ThrowIfNull(createControls);
        ArgumentNullException.ThrowIfNull(deliverBridge);
        ArgumentNullException.ThrowIfNull(deliverPeer);

        await using var authorityBoundary =
            await bridgeBoundary.EnterBoundaryAsync(cancellationToken)
                .ConfigureAwait(false);
        return await peerGate.RunAsync(
                async token =>
                {
                    if (!ReferenceEquals(captureNetwork(), sourceNetwork) ||
                        !sourceNetwork.IsControlPeerCurrent(sourcePeer) ||
                        !isNegotiated(sourceNetwork, sourcePeer))
                    {
                        return false;
                    }

                    var capturedBridge = captureReadyBridge();
                    if (capturedBridge is not { } bridgeConnection ||
                        !isReadyBridge(bridgeConnection) ||
                        !isRemoteMappingReady(
                            sourceNetwork,
                            sourcePeer,
                            bridgeConnection))
                    {
                        return false;
                    }

                    // Check mapping freshness once at entry rather than letting its timer expire halfway through a related batch.
                    // Continue checking the captured peer and bridge connections between deliveries.
                    bool AuthorityIsCurrent() =>
                        ReferenceEquals(captureNetwork(), sourceNetwork) &&
                        sourceNetwork.IsControlPeerCurrent(sourcePeer) &&
                        isNegotiated(sourceNetwork, sourcePeer) &&
                        isReadyBridge(bridgeConnection);

                    var rollbackState = default(TRollback)!;
                    var rollbackCaptured = false;
                    var mutationApplied = false;
                    var bridgeFramesDelivered = 0;
                    var mutation = default(TMutation)!;
                    IReadOnlyList<TControl> controls = [];

                    // Restore the saved registry snapshot only while rollback is still allowed by the transaction's delivery progress.
                    void RollBackMutation()
                    {
                        if (rollbackCaptured)
                        {
                            rollback(rollbackState);
                            rollbackCaptured = false;
                        }
                    }

                    try
                    {
                        if (!sourceNetwork.TryRunForControlPeer(
                                sourcePeer,
                                () =>
                                {
                                    if (!AuthorityIsCurrent())
                                    {
                                        return;
                                    }

                                    // Save the state before changing it so a failure before the first local delivery can leave no committed decision behind.
                                    rollbackState = captureRollbackState();
                                    rollbackCaptured = true;
                                    mutation = mutate();
                                    controls = createControls(mutation);
                                    mutationApplied = true;
                                }) ||
                            !mutationApplied)
                        {
                            RollBackMutation();
                            return false;
                        }

                        // Send all controls to the captured local game pipe before sending any of them to the guest.
                        // Each control has IDs and revisions that let receiving code identify the intended action state.
                        // Once a local write succeeds, keep the changed registry so later cleanup still knows which restraint may need releasing.
                        foreach (var control in controls)
                        {
                            if (!AuthorityIsCurrent() ||
                                !await deliverBridge(
                                        bridgeConnection,
                                        control,
                                        CancellationToken.None)
                                    .ConfigureAwait(false))
                            {
                                if (bridgeFramesDelivered == 0)
                                {
                                    RollBackMutation();
                                }
                                return false;
                            }
                            bridgeFramesDelivered++;
                            rollbackCaptured = false;
                            if (!AuthorityIsCurrent())
                            {
                                return false;
                            }
                        }

                        // Every control was written to the local pipe, although this does not confirm RDR2 has applied every effect yet.
                        // From here, a peer failure needs connection cleanup or replay rather than forgetting the state already sent locally.
                        foreach (var control in controls)
                        {
                            if (!AuthorityIsCurrent())
                            {
                                return false;
                            }

                            if (!await deliverPeer(
                                    sourceNetwork,
                                    sourcePeer,
                                    control,
                                    token)
                                .ConfigureAwait(false))
                            {
                                return false;
                            }
                            // A successful send still belongs to the captured peer even if a replacement connects immediately afterward.
                            // Do not roll back the registry as if that already-sent decision never existed.
                            if (!AuthorityIsCurrent())
                            {
                                return false;
                            }
                        }

                        rollbackCaptured = false;
                        return true;
                    }
                    catch
                    {
                        if (bridgeFramesDelivered == 0)
                        {
                            RollBackMutation();
                        }
                        throw;
                    }
                },
                cancellationToken)
            .ConfigureAwait(false);
    }
}

// Change state and hand the result to a delivery callback using the peer captured under the control gate.
// Unlike PeerBridgeAuthorityTransaction, this helper does not provide rollback or require a local game-pipe delivery.
internal static class NegotiatedPeerMutationTransaction
{
    // Capture a negotiated peer, perform the supplied state change, and call delivery for that same peer.
    // True means the change ran and the delivery callback returned normally, not that every endpoint applied it.
    public static ValueTask<bool> RunAsync<TMutation>(
        PeerControlSendGate gate,
        Func<ILanSession?> captureNetwork,
        Func<ILanSession, ControlPeerToken, bool> isNegotiated,
        Func<TMutation> mutate,
        Func<
            ILanSession,
            ControlPeerToken,
            TMutation,
            CancellationToken,
            ValueTask> deliver,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(gate);
        ArgumentNullException.ThrowIfNull(captureNetwork);
        ArgumentNullException.ThrowIfNull(isNegotiated);
        ArgumentNullException.ThrowIfNull(mutate);
        ArgumentNullException.ThrowIfNull(deliver);

        return gate.RunAsync(
            async token =>
            {
                var network = captureNetwork();
                if (network is null ||
                    !network.TryCaptureControlPeer(out var peer) ||
                    !ReferenceEquals(captureNetwork(), network) ||
                    !isNegotiated(network, peer))
                {
                    return false;
                }

                var mutationApplied = false;
                var mutation = default(TMutation)!;
                if (!network.TryRunForControlPeer(peer, () =>
                    {
                        if (!ReferenceEquals(captureNetwork(), network) ||
                            !isNegotiated(network, peer))
                        {
                            return;
                        }

                        mutation = mutate();
                        mutationApplied = true;
                    }) ||
                    !mutationApplied)
                {
                    return false;
                }

                await deliver(network, peer, mutation, token)
                    .ConfigureAwait(false);
                return true;
            },
            cancellationToken);
    }
}

public sealed class SafetyViolationException : Exception
{
    // Carry the reason a safety rule stopped the multiplayer runtime, such as detecting Online mode.
    public SafetyViolationException(string message)
        : base(message)
    {
    }
}

public sealed class BridgeShutdownException : Exception
{
    // Preserve the bridge's shutdown reason while also making it available as a normal exception message.
    public BridgeShutdownException(string reason)
        : base($"Game bridge stopped: {reason}")
    {
        Reason = reason;
    }

    public string Reason { get; }
}

public sealed class MotionReplicationModeMismatchException : Exception
{
    // Carry the local and optional peer movement modes along with the reason negotiation failed.
    public MotionReplicationModeMismatchException(
        string reason,
        MotionReplicationWireMode localMode,
        MotionReplicationWireMode? peerMode)
        : base(reason)
    {
        LocalMode = localMode;
        PeerMode = peerMode;
    }

    public MotionReplicationWireMode LocalMode { get; }

    public MotionReplicationWireMode? PeerMode { get; }
}
