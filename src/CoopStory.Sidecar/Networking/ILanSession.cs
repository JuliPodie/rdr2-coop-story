using System.Net;
using CoopStory.Protocol;

namespace CoopStory.Sidecar.Networking;

// Delivers a validated control or snapshot frame together with the connection generation that was current when the sidecar accepted it.
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

// Capability for the currently authenticated TCP peer.
// Queued work must carry it so a reconnect cannot accidentally mutate the replacement peer's state.
public readonly record struct ControlPeerToken(
    Guid SessionInstanceId,
    ulong Generation)
{
    public bool IsValid =>
        SessionInstanceId != Guid.Empty && Generation != 0;
}

// Shared contract for host and guest transport.
// TCP is the reliable control lane; UDP is the lossy/latest-state snapshot lane.
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

    // Runs a synchronous cache change only if this is still the same peer.
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

// Monotonic local time carried in frames for interpolation and age checks, not wall-clock time (which can jump when a computer adjusts its clock).
internal static class NetworkClock
{
    public static ulong Tick => unchecked((ulong)Environment.TickCount64);
}
