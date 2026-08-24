using System.Net;
using System.Net.Sockets;
using CoopStory.Protocol;
using CoopStory.Sidecar.Configuration;
using CoopStory.Sidecar.Diagnostics;

namespace CoopStory.Sidecar.Networking;

public sealed class LanSessionGuest : ILanSession
{
    private readonly object _peerSync = new();
    private readonly SidecarConfig _config;
    private readonly SessionCredentials _credentials;
    private readonly JsonLineLogger _logger;
    private readonly IPAddress _udpListenAddress;
    private readonly Guid _instanceId = Guid.NewGuid();
    private readonly CancellationTokenSource _stop = new();
    private ControlConnection? _activePeer;
    private UdpClient? _udp;
    private IPEndPoint? _hostUdpEndpoint;
    private int _sequence;
    private ulong _controlPeerGeneration;
    private int _udpPolicyRejections;
    private bool _disposed;

    public LanSessionGuest(
        SidecarConfig config,
        SessionCredentials credentials,
        JsonLineLogger logger,
        IPAddress? udpListenAddress = null)
    {
        _config = (config ?? throw new ArgumentNullException(nameof(config))).Validate();
        if (_config.Role != SessionRole.Guest)
        {
            throw new ArgumentException("Guest service requires role Guest.", nameof(config));
        }

        _credentials = credentials ?? throw new ArgumentNullException(nameof(credentials));
        _logger = logger ?? throw new ArgumentNullException(nameof(logger));
        _udpListenAddress = udpListenAddress ?? IPAddress.Any;
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

    public IPEndPoint? LocalUdpEndpoint
    {
        get
        {
            lock (_peerSync)
            {
                return _udp?.Client.LocalEndPoint is IPEndPoint endpoint
                    ? new IPEndPoint(endpoint.Address, endpoint.Port)
                    : null;
            }
        }
    }

    public async Task RunAsync(CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            _stop.Token);
        var reconnectDelay = _config.Network.ReconnectMinMs;
        var attempt = 0;

        while (!linked.IsCancellationRequested)
        {
            try
            {
                attempt++;
                await _logger.InfoAsync(
                    "network.host.connecting",
                    $"Connecting to {_config.HostAddress}:{_config.TcpPort}.",
                    new Dictionary<string, object?>
                    {
                        ["hostAddress"] = _config.HostAddress,
                        ["tcpPort"] = _config.TcpPort,
                        ["udpPort"] = _config.UdpPort,
                        ["attempt"] = attempt
                    },
                    linked.Token).ConfigureAwait(false);
                await ConnectAndRunAsync(linked.Token).ConfigureAwait(false);
                reconnectDelay = _config.Network.ReconnectMinMs;
            }
            catch (OperationCanceledException) when (linked.IsCancellationRequested)
            {
                break;
            }
            catch (ProtocolException exception)
            {
                await _logger.WarningAsync(
                    "network.host.unavailable",
                    exception.Message,
                    cancellationToken: linked.Token).ConfigureAwait(false);
                await RaiseAuthenticationRejectedAsync(
                    exception.Message,
                    CancellationToken.None).ConfigureAwait(false);
            }
            catch (Exception exception) when (
                exception is IOException or SocketException or TimeoutException)
            {
                await _logger.WarningAsync(
                    "network.host.unavailable",
                    exception.Message,
                    cancellationToken: linked.Token).ConfigureAwait(false);
            }

            await _logger.InfoAsync(
                "network.host.reconnect-scheduled",
                $"Retrying host connection in {reconnectDelay} ms.",
                new Dictionary<string, object?>
                {
                    ["delayMs"] = reconnectDelay,
                    ["nextAttempt"] = attempt + 1
                },
                cancellationToken: linked.Token).ConfigureAwait(false);
            await Task.Delay(reconnectDelay, linked.Token).ConfigureAwait(false);
            reconnectDelay = Math.Min(
                reconnectDelay * 2,
                _config.Network.ReconnectMaxMs);
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
            endpoint = _hostUdpEndpoint;
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
        var peer = GetActivePeer();
        if (peer is not null)
        {
            await peer.DisposeAsync().ConfigureAwait(false);
        }

        _udp?.Dispose();
        _stop.Dispose();
    }

    private async Task ConnectAndRunAsync(CancellationToken cancellationToken)
    {
        var addresses = await Dns.GetHostAddressesAsync(
            _config.HostAddress,
            cancellationToken).ConfigureAwait(false);
        var address = addresses.FirstOrDefault(item =>
            item.AddressFamily == AddressFamily.InterNetwork)
            ?? throw new SocketException((int)SocketError.AddressFamilyNotSupported);

        using var tcp = new TcpClient(AddressFamily.InterNetwork);
        await tcp.ConnectAsync(address, _config.TcpPort, cancellationToken)
            .ConfigureAwait(false);
        await using var peer = new ControlConnection(tcp);
        var authenticated = false;
        var handshake = await HandshakeProtocol.ConnectToHostAsync(
            peer,
            _credentials,
            _instanceId,
            NextSequence,
            cancellationToken).ConfigureAwait(false);
        var hostInstanceId = handshake.InstanceId;
        authenticated = true;

        using var udp = new UdpClient(new IPEndPoint(_udpListenAddress, 0));
        var hostUdpEndpoint = new IPEndPoint(address, _config.UdpPort);
        var hostUdpBinding = new UdpPeerBinding(
            hostUdpEndpoint.Address,
            hostUdpEndpoint.Port,
            handshake.ControlSequence,
            hostInstanceId);
        lock (_peerSync)
        {
            _activePeer = peer;
            _controlPeerGeneration = NextPeerGeneration(
                _controlPeerGeneration);
            _udp = udp;
            _hostUdpEndpoint = hostUdpEndpoint;
        }
        _ = TryCaptureControlPeer(out var peerToken);

        var resync = CreateEnvelope(
            MessageType.ResyncRequest,
            ReadOnlyMemory<byte>.Empty,
            NetworkClock.Tick);
        await peer.SendAsync(resync, cancellationToken).ConfigureAwait(false);
        await RaiseEnvelopeAsync(
                resync,
                peerToken,
                cancellationToken)
            .ConfigureAwait(false);

        await _logger.InfoAsync(
            "network.host.connected",
            $"Authenticated host {hostInstanceId:N}; automatic resync requested.",
            new Dictionary<string, object?>
            {
                ["hostInstanceId"] = hostInstanceId.ToString("N"),
                ["resolvedAddress"] = address.ToString()
            },
            cancellationToken: cancellationToken).ConfigureAwait(false);
        await RaiseConnectionChangedAsync(
            connected: true,
            cancellationToken).ConfigureAwait(false);

        try
        {
            using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            var receive = ReceiveControlLoopAsync(
                peer,
                peerToken,
                linked.Token);
            var udpReceive = UdpReceiveLoopAsync(
                udp,
                hostUdpBinding,
                peerToken,
                linked.Token);
            var heartbeat = HeartbeatLoopAsync(peer, linked.Token);
            var timeout = TimeoutLoopAsync(peer, linked.Token);
            await Task.WhenAny(receive, udpReceive, heartbeat, timeout).ConfigureAwait(false);
            await linked.CancelAsync().ConfigureAwait(false);
            await ObserveCompletionAsync(receive, udpReceive, heartbeat, timeout)
                .ConfigureAwait(false);
        }
        finally
        {
            var disconnectedCurrentPeer = false;
            lock (_peerSync)
            {
                if (ReferenceEquals(_activePeer, peer))
                {
                    _activePeer = null;
                    _udp = null;
                    _hostUdpEndpoint = null;
                    disconnectedCurrentPeer = true;
                }
            }

            if (authenticated && disconnectedCurrentPeer)
            {
                await RaiseConnectionChangedAsync(
                    connected: false,
                    CancellationToken.None).ConfigureAwait(false);
                await _logger.InfoAsync(
                    "network.host.disconnected",
                    "Authenticated host disconnected; reconnect will be attempted.",
                    cancellationToken: CancellationToken.None).ConfigureAwait(false);
            }
        }
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

    private async Task UdpReceiveLoopAsync(
        UdpClient udp,
        UdpPeerBinding hostBinding,
        ControlPeerToken peerToken,
        CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            var received = await udp.ReceiveAsync(cancellationToken).ConfigureAwait(false);
            if (!hostBinding.IsSourceAllowed(received.RemoteEndPoint))
            {
                await LogUdpPolicyRejectionAsync(
                    "host-endpoint",
                    cancellationToken).ConfigureAwait(false);
                continue;
            }

            ProtocolEnvelope envelope;
            try
            {
                envelope = AuthenticatedDatagramCodec.Decode(
                    received.Buffer,
                    _credentials,
                    expectedSenderInstanceId: hostBinding.ExpectedInstanceId
                        ?? throw new ProtocolException(
                            "UDP binding is not associated with an authenticated TCP instance."));
            }
            catch (ProtocolException)
            {
                await LogUdpPolicyRejectionAsync(
                    "authentication-or-frame",
                    cancellationToken).ConfigureAwait(false);
                continue;
            }

            if (!hostBinding.TryAccept(
                    received.RemoteEndPoint,
                    envelope,
                    out var rejectionReason))
            {
                await LogUdpPolicyRejectionAsync(
                    rejectionReason,
                    cancellationToken).ConfigureAwait(false);
                continue;
            }

            await RaiseEnvelopeAsync(
                    envelope,
                    peerToken,
                    cancellationToken)
                .ConfigureAwait(false);
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
                throw new TimeoutException("Host heartbeat timed out.");
            }
        }
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
