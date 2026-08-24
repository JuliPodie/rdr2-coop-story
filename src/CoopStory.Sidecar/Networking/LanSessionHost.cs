using System.Net;
using System.Net.Sockets;
using CoopStory.Protocol;
using CoopStory.Sidecar.Configuration;
using CoopStory.Sidecar.Diagnostics;

namespace CoopStory.Sidecar.Networking;

public sealed class LanSessionHost : ILanSession
{
    private readonly object _peerSync = new();
    private readonly SemaphoreSlim _peerActivationGate = new(1, 1);
    private readonly SidecarConfig _config;
    private readonly SessionCredentials _credentials;
    private readonly JsonLineLogger _logger;
    private readonly IPAddress _listenAddress;
    private readonly Guid _instanceId = Guid.NewGuid();
    private readonly CancellationTokenSource _stop = new();
    private TcpListener? _listener;
    private UdpClient? _udp;
    private ControlConnection? _activePeer;
    private UdpPeerBinding? _guestUdpBinding;
    private CancellationTokenSource? _guestUdpStop;
    private int _sequence;
    private ulong _controlPeerGeneration;
    private int _udpPolicyRejections;
    private bool _disposed;

    public LanSessionHost(
        SidecarConfig config,
        SessionCredentials credentials,
        JsonLineLogger logger,
        IPAddress? listenAddress = null)
    {
        _config = (config ?? throw new ArgumentNullException(nameof(config))).Validate();
        if (_config.Role != SessionRole.Host)
        {
            throw new ArgumentException("Host service requires role Host.", nameof(config));
        }

        _credentials = credentials ?? throw new ArgumentNullException(nameof(credentials));
        _logger = logger ?? throw new ArgumentNullException(nameof(logger));
        _listenAddress = listenAddress ?? IPAddress.Any;
    }

    public bool IsConnected
    {
        get
        {
            lock (_peerSync)
            {
                return _activePeer is not null;
            }
        }
    }

    public IPAddress? RemoteAddress
    {
        get
        {
            lock (_peerSync)
            {
                return _activePeer?.RemoteEndPoint.Address;
            }
        }
    }

    public IPAddress ListenAddress => _listenAddress;

    public event EnvelopeReceivedHandler? EnvelopeReceived;

    public event AuthenticationRejectedHandler? AuthenticationRejected;

    public event PeerConnectionChangedHandler? ConnectionChanged;

    public bool TryCaptureControlPeer(out ControlPeerToken peer)
    {
        lock (_peerSync)
        {
            if (_activePeer is null || _controlPeerGeneration == 0)
            {
                peer = default;
                return false;
            }

            peer = new ControlPeerToken(
                _instanceId,
                _controlPeerGeneration);
            return true;
        }
    }

    public bool IsControlPeerCurrent(ControlPeerToken peer)
    {
        lock (_peerSync)
        {
            return peer.IsValid &&
                peer.SessionInstanceId == _instanceId &&
                peer.Generation == _controlPeerGeneration &&
                _activePeer is not null;
        }
    }

    public bool TryRunForControlPeer(
        ControlPeerToken peer,
        Action operation)
    {
        ArgumentNullException.ThrowIfNull(operation);
        lock (_peerSync)
        {
            if (!peer.IsValid ||
                peer.SessionInstanceId != _instanceId ||
                peer.Generation != _controlPeerGeneration ||
                _activePeer is null)
            {
                return false;
            }

            operation();
            return true;
        }
    }

    public async Task RunAsync(CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            _stop.Token);
        _listener = new TcpListener(_listenAddress, _config.TcpPort);
        _udp = new UdpClient(new IPEndPoint(_listenAddress, _config.UdpPort));
        _listener.Start(backlog: 2);

        await _logger.InfoAsync(
            "network.host.started",
            $"Listening on {_listenAddress} TCP {_config.TcpPort} and " +
            $"UDP {_config.UdpPort}.",
            new Dictionary<string, object?>
            {
                ["listenAddress"] = _listenAddress.ToString(),
                ["listenMode"] = IPAddress.IsLoopback(_listenAddress)
                    ? "loopback-only"
                    : "lan",
                ["tcpPort"] = _config.TcpPort,
                ["udpPort"] = _config.UdpPort
            },
            cancellationToken: linked.Token).ConfigureAwait(false);

        try
        {
            await Task.WhenAll(
                AcceptLoopAsync(linked.Token),
                UdpReceiveLoopAsync(linked.Token)).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (linked.IsCancellationRequested)
        {
        }
        finally
        {
            _listener.Stop();
            _udp.Dispose();
        }
    }

    public async ValueTask<bool> SendControlAsync(
        MessageType type,
        ReadOnlyMemory<byte> payload,
        ulong tick,
        CancellationToken cancellationToken = default)
    {
        if (!TryCaptureControlPeer(out var peer))
        {
            return false;
        }

        return await SendControlAsync(
                peer,
                type,
                payload,
                tick,
                cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask<bool> SendControlAsync(
        ControlPeerToken peerToken,
        MessageType type,
        ReadOnlyMemory<byte> payload,
        ulong tick,
        CancellationToken cancellationToken = default)
    {
        ControlConnection? peer;
        lock (_peerSync)
        {
            if (!peerToken.IsValid ||
                peerToken.SessionInstanceId != _instanceId ||
                peerToken.Generation != _controlPeerGeneration ||
                _activePeer is null)
            {
                return false;
            }

            peer = _activePeer;
        }

        try
        {
            await peer.SendAsync(
                CreateEnvelope(type, payload, tick),
                cancellationToken).ConfigureAwait(false);
            return true;
        }
        catch (Exception exception) when (
            exception is IOException or SocketException or ObjectDisposedException)
        {
            return false;
        }
    }

    public async ValueTask<bool> SendSnapshotAsync(
        MessageType type,
        ReadOnlyMemory<byte> payload,
        ulong tick,
        CancellationToken cancellationToken = default)
    {
        UdpClient? udp;
        IPEndPoint? endpoint;
        lock (_peerSync)
        {
            udp = _udp;
            endpoint = _guestUdpBinding?.PinnedEndpoint;
        }

        if (udp is null || endpoint is null)
        {
            return false;
        }

        var datagram = AuthenticatedDatagramCodec.Encode(
            CreateEnvelope(type, payload, tick),
            _credentials,
            _instanceId);
        try
        {
            await udp.SendAsync(datagram, endpoint, cancellationToken).ConfigureAwait(false);
            return true;
        }
        catch (Exception exception) when (
            exception is SocketException or ObjectDisposedException)
        {
            return false;
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        await _stop.CancelAsync().ConfigureAwait(false);
        _listener?.Stop();
        _udp?.Dispose();
        var peer = GetActivePeer();
        if (peer is not null)
        {
            await peer.DisposeAsync().ConfigureAwait(false);
        }
        CancelGuestUdpBinding();

        _stop.Dispose();
    }

    private async Task AcceptLoopAsync(CancellationToken cancellationToken)
    {
        var listener = _listener
            ?? throw new InvalidOperationException("TCP listener is not initialized.");
        while (!cancellationToken.IsCancellationRequested)
        {
            var client = await listener.AcceptTcpClientAsync(cancellationToken)
                .ConfigureAwait(false);
            _ = HandlePeerSafelyAsync(client, cancellationToken);
        }
    }

    private async Task HandlePeerSafelyAsync(
        TcpClient client,
        CancellationToken cancellationToken)
    {
        await using var peer = new ControlConnection(client);
        var authenticated = false;
        try
        {
            var handshake = await HandshakeProtocol.AcceptGuestAsync(
                peer,
                _credentials,
                _instanceId,
                NextSequence,
                cancellationToken).ConfigureAwait(false);
            var guestInstance = handshake.InstanceId;
            authenticated = true;

            var guestUdpStop = new CancellationTokenSource();
            var peerToken = default(ControlPeerToken);
            await _peerActivationGate.WaitAsync(cancellationToken)
                .ConfigureAwait(false);
            try
            {
                ControlConnection? replaced;
                CancellationTokenSource? replacedUdpStop;
                lock (_peerSync)
                {
                    replaced = _activePeer;
                    replacedUdpStop = _guestUdpStop;
                    _activePeer = peer;
                    _controlPeerGeneration = NextPeerGeneration(
                        _controlPeerGeneration);
                    peerToken = new ControlPeerToken(
                        _instanceId,
                        _controlPeerGeneration);
                    _guestUdpBinding = new UdpPeerBinding(
                        peer.RemoteEndPoint.Address,
                        controlSequenceFloor: handshake.ControlSequence,
                        expectedInstanceId: guestInstance);
                    _guestUdpStop = guestUdpStop;
                }

                CancelAndDispose(replacedUdpStop);
                if (replaced is not null && !ReferenceEquals(replaced, peer))
                {
                    await replaced.DisposeAsync().ConfigureAwait(false);
                    // Replacement is a real authority boundary even though the
                    // old receive loop must not emit a second stale disconnect.
                    // Runtime releases restraint state before negotiating the
                    // freshly installed peer generation.
                    await RaiseConnectionChangedAsync(
                        connected: false,
                        CancellationToken.None).ConfigureAwait(false);
                }

                await _logger.InfoAsync(
                    "network.guest.connected",
                    $"Authenticated guest {guestInstance:N}.",
                    new Dictionary<string, object?>
                    {
                        ["guestInstanceId"] = guestInstance.ToString("N")
                    },
                    cancellationToken: cancellationToken).ConfigureAwait(false);
                await RaiseConnectionChangedAsync(
                    connected: true,
                    cancellationToken).ConfigureAwait(false);
            }
            finally
            {
                _peerActivationGate.Release();
            }
            await RunConnectedPeerAsync(
                    peer,
                    peerToken,
                    guestUdpStop.Token,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception exception) when (
            exception is IOException or
                         SocketException or
                         ProtocolException or
                         TimeoutException or
                         ObjectDisposedException)
        {
            await _logger.WarningAsync(
                authenticated
                    ? "network.guest.connection-fault"
                    : "network.guest.rejected",
                exception.Message,
                cancellationToken: CancellationToken.None).ConfigureAwait(false);
            if (!authenticated && exception is ProtocolException)
            {
                await RaiseAuthenticationRejectedAsync(
                    exception.Message,
                    CancellationToken.None).ConfigureAwait(false);
            }
        }
        finally
        {
            await _peerActivationGate.WaitAsync(CancellationToken.None)
                .ConfigureAwait(false);
            try
            {
                CancellationTokenSource? guestUdpStop = null;
                var disconnectedCurrentPeer = false;
                lock (_peerSync)
                {
                    if (ReferenceEquals(_activePeer, peer))
                    {
                        _activePeer = null;
                        _guestUdpBinding = null;
                        guestUdpStop = _guestUdpStop;
                        _guestUdpStop = null;
                        disconnectedCurrentPeer = true;
                    }
                }
                CancelAndDispose(guestUdpStop);

                if (authenticated && disconnectedCurrentPeer)
                {
                    await RaiseConnectionChangedAsync(
                        connected: false,
                        CancellationToken.None).ConfigureAwait(false);
                    await _logger.InfoAsync(
                        "network.guest.disconnected",
                        "Authenticated guest disconnected.",
                        cancellationToken: CancellationToken.None).ConfigureAwait(false);
                }
            }
            finally
            {
                _peerActivationGate.Release();
            }
        }
    }

    private async Task RunConnectedPeerAsync(
        ControlConnection peer,
        ControlPeerToken peerToken,
        CancellationToken peerCancellationToken,
        CancellationToken cancellationToken)
    {
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            peerCancellationToken);
        var receive = ReceiveControlLoopAsync(
            peer,
            peerToken,
            linked.Token);
        var heartbeat = HeartbeatLoopAsync(peer, linked.Token);
        var timeout = TimeoutLoopAsync(peer, linked.Token);
        await Task.WhenAny(receive, heartbeat, timeout).ConfigureAwait(false);
        await linked.CancelAsync().ConfigureAwait(false);
        await ObserveCompletionAsync(receive, heartbeat, timeout).ConfigureAwait(false);
    }

    private async Task ReceiveControlLoopAsync(
        ControlConnection peer,
        ControlPeerToken peerToken,
        CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            var envelope = await peer.ReceiveAsync(cancellationToken).ConfigureAwait(false);
            if (envelope is null)
            {
                return;
            }

            if (!IsControlPeerCurrent(peerToken))
            {
                return;
            }

            await RaiseEnvelopeAsync(
                    envelope,
                    peerToken,
                    cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private async Task HeartbeatLoopAsync(
        ControlConnection peer,
        CancellationToken cancellationToken)
    {
        using var timer = new PeriodicTimer(
            TimeSpan.FromMilliseconds(_config.Network.HeartbeatIntervalMs));
        while (await timer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false))
        {
            var heartbeat = new HeartbeatPayload(
                DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
                NetworkClock.Tick);
            await peer.SendAsync(
                CreateEnvelope(
                    MessageType.Heartbeat,
                    PayloadJson.Serialize(heartbeat),
                    NetworkClock.Tick),
                cancellationToken).ConfigureAwait(false);
        }
    }

    private async Task TimeoutLoopAsync(
        ControlConnection peer,
        CancellationToken cancellationToken)
    {
        using var timer = new PeriodicTimer(
            TimeSpan.FromMilliseconds(_config.Network.HeartbeatIntervalMs));
        while (await timer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false))
        {
            if (Environment.TickCount64 - peer.LastReceivedTimestamp >
                _config.Network.HeartbeatTimeoutMs)
            {
                throw new TimeoutException("Guest heartbeat timed out.");
            }
        }
    }

    private async Task UdpReceiveLoopAsync(CancellationToken cancellationToken)
    {
        var udp = _udp ?? throw new InvalidOperationException("UDP socket is not initialized.");
        while (!cancellationToken.IsCancellationRequested)
        {
            var received = await udp.ReceiveAsync(cancellationToken).ConfigureAwait(false);
            UdpPeerBinding? initialBinding;
            lock (_peerSync)
            {
                initialBinding = _guestUdpBinding;
            }

            if (initialBinding is null)
            {
                await LogUdpPolicyRejectionAsync(
                    "no-authenticated-tcp-peer",
                    cancellationToken).ConfigureAwait(false);
                continue;
            }

            if (!initialBinding.IsSourceAllowed(received.RemoteEndPoint))
            {
                await LogUdpPolicyRejectionAsync(
                    "tcp-source-or-pinned-endpoint",
                    cancellationToken).ConfigureAwait(false);
                continue;
            }

            ProtocolEnvelope envelope;
            try
            {
                var expectedGuestInstanceId = initialBinding.ExpectedInstanceId
                    ?? throw new ProtocolException(
                        "UDP binding is not associated with an authenticated TCP instance.");
                envelope = AuthenticatedDatagramCodec.Decode(
                    received.Buffer,
                    _credentials,
                    expectedGuestInstanceId);
            }
            catch (ProtocolException)
            {
                await LogUdpPolicyRejectionAsync(
                    "authentication-or-frame",
                    cancellationToken).ConfigureAwait(false);
                continue;
            }

            CancellationToken peerCancellation;
            ControlPeerToken peerToken;
            string rejectionReason;
            var accepted = false;
            lock (_peerSync)
            {
                if (ReferenceEquals(_guestUdpBinding, initialBinding) &&
                    _activePeer is not null &&
                    _guestUdpStop is not null &&
                    initialBinding.TryAccept(
                        received.RemoteEndPoint,
                        envelope,
                        out rejectionReason))
                {
                    peerCancellation = _guestUdpStop.Token;
                    peerToken = new ControlPeerToken(
                        _instanceId,
                        _controlPeerGeneration);
                    accepted = true;
                }
                else
                {
                    peerCancellation = new CancellationToken(canceled: true);
                    peerToken = default;
                    rejectionReason = ReferenceEquals(
                        _guestUdpBinding,
                        initialBinding)
                        ? "sequence-or-message-type"
                        : "stale-tcp-peer";
                }
            }

            if (!accepted)
            {
                await LogUdpPolicyRejectionAsync(
                    rejectionReason,
                    cancellationToken).ConfigureAwait(false);
                continue;
            }

            try
            {
                await RaiseEnvelopeAsync(
                        envelope,
                        peerToken,
                        peerCancellation)
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (
                peerCancellation.IsCancellationRequested)
            {
                // The authenticated TCP peer was replaced while this
                // datagram was being dispatched.
            }
        }
    }

    private async ValueTask LogUdpPolicyRejectionAsync(
        string reason,
        CancellationToken cancellationToken)
    {
        var count = Interlocked.Increment(ref _udpPolicyRejections);
        if (count > 3 && (count & (count - 1)) != 0)
        {
            return;
        }

        await _logger.WarningAsync(
            "network.udp.policy-rejected",
            $"Dropped UDP datagram ({reason}); rejection count {count}.",
            new Dictionary<string, object?>
            {
                ["reason"] = reason,
                ["rejectionCount"] = count
            },
            cancellationToken).ConfigureAwait(false);
    }

    private void CancelGuestUdpBinding()
    {
        CancellationTokenSource? stop;
        lock (_peerSync)
        {
            _guestUdpBinding = null;
            stop = _guestUdpStop;
            _guestUdpStop = null;
        }

        CancelAndDispose(stop);
    }

    private static void CancelAndDispose(CancellationTokenSource? source)
    {
        if (source is null)
        {
            return;
        }

        source.Cancel();
        source.Dispose();
    }

    private async ValueTask RaiseEnvelopeAsync(
        ProtocolEnvelope envelope,
        ControlPeerToken peer,
        CancellationToken cancellationToken)
    {
        if (EnvelopeReceived is { } handler &&
            IsControlPeerCurrent(peer))
        {
            await handler(envelope, peer, cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private async ValueTask RaiseAuthenticationRejectedAsync(
        string reason,
        CancellationToken cancellationToken)
    {
        if (AuthenticationRejected is not { } handlers)
        {
            return;
        }

        foreach (AuthenticationRejectedHandler handler in handlers.GetInvocationList())
        {
            await handler(reason, cancellationToken).ConfigureAwait(false);
        }
    }

    private async ValueTask RaiseConnectionChangedAsync(
        bool connected,
        CancellationToken cancellationToken)
    {
        if (ConnectionChanged is not { } handlers)
        {
            return;
        }

        foreach (PeerConnectionChangedHandler handler in handlers.GetInvocationList())
        {
            await handler(connected, cancellationToken).ConfigureAwait(false);
        }
    }

    private ControlConnection? GetActivePeer()
    {
        lock (_peerSync)
        {
            return _activePeer;
        }
    }

    private ProtocolEnvelope CreateEnvelope(
        MessageType type,
        ReadOnlyMemory<byte> payload,
        ulong tick) =>
        new(type, NextSequence(), tick, payload);

    private uint NextSequence() => unchecked((uint)Interlocked.Increment(ref _sequence));

    private static ulong NextPeerGeneration(ulong generation) =>
        generation == ulong.MaxValue ? 1 : generation + 1;

    private static async Task ObserveCompletionAsync(params Task[] tasks)
    {
        try
        {
            await Task.WhenAll(tasks).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
        }
    }
}
