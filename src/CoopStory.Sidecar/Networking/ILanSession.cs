using System.Net;
using CoopStory.Protocol;

namespace CoopStory.Sidecar.Networking;

public delegate ValueTask EnvelopeReceivedHandler(
    ProtocolEnvelope envelope,
    ControlPeerToken peer,
    CancellationToken cancellationToken);

public delegate ValueTask AuthenticationRejectedHandler(
    string reason,
    CancellationToken cancellationToken);

public delegate ValueTask PeerConnectionChangedHandler(
    bool connected,
    CancellationToken cancellationToken);

public readonly record struct ControlPeerToken(
    Guid SessionInstanceId,
    ulong Generation)
{
    public bool IsValid =>
        SessionInstanceId != Guid.Empty && Generation != 0;
}

public interface ILanSession : IAsyncDisposable
{
    bool IsConnected { get; }

    IPAddress? RemoteAddress { get; }

    event EnvelopeReceivedHandler? EnvelopeReceived;

    event AuthenticationRejectedHandler? AuthenticationRejected;

    event PeerConnectionChangedHandler? ConnectionChanged;

    Task RunAsync(CancellationToken cancellationToken = default);

    bool TryCaptureControlPeer(out ControlPeerToken peer);

    bool IsControlPeerCurrent(ControlPeerToken peer);

    bool TryRunForControlPeer(ControlPeerToken peer, Action operation);

    ValueTask<bool> SendControlAsync(
        MessageType type,
        ReadOnlyMemory<byte> payload,
        ulong tick,
        CancellationToken cancellationToken = default);

    ValueTask<bool> SendControlAsync(
        ControlPeerToken peer,
        MessageType type,
        ReadOnlyMemory<byte> payload,
        ulong tick,
        CancellationToken cancellationToken = default);

    ValueTask<bool> SendSnapshotAsync(
        MessageType type,
        ReadOnlyMemory<byte> payload,
        ulong tick,
        CancellationToken cancellationToken = default);
}

internal static class NetworkClock
{
    public static ulong Tick => unchecked((ulong)Environment.TickCount64);
}
