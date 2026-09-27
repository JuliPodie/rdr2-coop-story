using System.Net;
using CoopStory.Protocol;

namespace CoopStory.Sidecar.Networking;

// Checks incoming UDP messages against the peer already identified by the TCP connection.
// Peer means the other player's Sidecar, and endpoint means its IP address plus port number.
// UDP can lose, repeat, or reorder packets, so this class checks their source, type, and sequence history.
// The surrounding networking code performs authentication and calls these checks before accepting the message.
// This class does not send packets or decide how to move the remote player.
internal sealed class UdpPeerBinding
{
    private readonly IPAddress _expectedAddress;
    private readonly int? _expectedPort;
    private readonly uint? _controlSequenceFloor;
    // Remember recently accepted sequence numbers so the same UDP packet cannot pass this check repeatedly.
    // The sender shares one sequence across player, world and TCP messages.
    // A host tick can publish 48 world entities, so the default 64-packet
    // window discarded unseen player/entity snapshots after a short reorder.
    // Keep one bounded global replay window, sized for mixed-stream bursts.
    private readonly SequenceReplayWindow _sequences = new(2048);
    // Remember the first accepted source so later packets must come from that same address and port.
    private IPEndPoint? _pinnedEndpoint;

    // Set the expected sender using facts learned while establishing the session.
    // An optional sequence floor rejects packets from before the current connection was established.
    // An optional instance ID identifies the particular running Sidecar expected by the caller.
    public UdpPeerBinding(
        IPAddress expectedAddress,
        int? expectedPort = null,
        uint? controlSequenceFloor = null,
        Guid? expectedInstanceId = null)
    {
        // TCP tells us which remote IP is authenticated; UDP must originate there even before its dynamic source port has been learned/pinned.
        _expectedAddress = expectedAddress
            ?? throw new ArgumentNullException(nameof(expectedAddress));
        if (expectedPort is < IPEndPoint.MinPort or > IPEndPoint.MaxPort)
        {
            throw new ArgumentOutOfRangeException(nameof(expectedPort));
        }

        if (expectedInstanceId == Guid.Empty)
        {
            throw new ArgumentException(
                "Expected UDP peer instance identifier cannot be empty.",
                nameof(expectedInstanceId));
        }

        _expectedPort = expectedPort;
        _controlSequenceFloor = controlSequenceFloor;
        ExpectedInstanceId = expectedInstanceId;
    }

    // Expose the expected running program's ID for the surrounding networking code to check.
    public Guid? ExpectedInstanceId { get; }

    // Return a copy so a caller cannot accidentally change the source address we remembered.
    public IPEndPoint? PinnedEndpoint => _pinnedEndpoint is null
        ? null
        : new IPEndPoint(_pinnedEndpoint.Address, _pinnedEndpoint.Port);

    // Check only the sender's address and port, without changing the remembered packet history.
    // Before a port is pinned, the optional expected port can still restrict the first sender.
    public bool IsSourceAllowed(IPEndPoint source)
    {
        ArgumentNullException.ThrowIfNull(source);
        if (!source.Address.Equals(_expectedAddress) ||
            (_expectedPort.HasValue && source.Port != _expectedPort.Value))
        {
            return false;
        }

        // The first accepted UDP packet pins the port too, stopping a second socket on the same IP from injecting snapshot frames mid-session.
        return _pinnedEndpoint is null ||
            EndpointsEqual(_pinnedEndpoint, source);
    }

    // Try to accept one already-decoded envelope from this UDP sender.
    // Return false with a short reason when a check fails so the caller can log what happened.
    // A true result means these UDP admission checks passed, not that RDR2 has applied the payload.
    public bool TryAccept(
        IPEndPoint source,
        ProtocolEnvelope envelope,
        out string rejectionReason)
    {
        ArgumentNullException.ThrowIfNull(source);
        ArgumentNullException.ThrowIfNull(envelope);
        if (!IsSourceAllowed(source))
        {
            rejectionReason = "source-endpoint";
            return false;
        }

        // Spawn/despawn and resync are TCP-only because losing one of them would leave a different entity graph at each peer.
        if (!IsUdpMessageType(envelope.Type))
        {
            rejectionReason = "message-type";
            return false;
        }

        // Never let an older UDP packet that predates this TCP handshake become the initial snapshot in the newly authenticated session.
        if (_controlSequenceFloor.HasValue &&
            !SequenceNumber.IsNewer(
                envelope.Sequence,
                _controlSequenceFloor.Value))
        {
            rejectionReason = "sequence-floor";
            return false;
        }

        if (!_sequences.TryAccept(envelope.Sequence))
        {
            // Let SequenceReplayWindow decide whether this sequence can still be accepted.
            // Its window handles repeats and packet age; this method does not reorder packets for the game.
            rejectionReason = "sequence-replay";
            return false;
        }

        // Only bind a port after its authenticated, allowable first frame wins.
        _pinnedEndpoint ??= new IPEndPoint(source.Address, source.Port);
        rejectionReason = string.Empty;
        return true;
    }

    // List the message types allowed to arrive through UDP.
    // Frequent position updates can be replaced by newer updates if one is lost.
    // Creating or removing an NPC needs reliable delivery, so those message types are excluded here.
    private static bool IsUdpMessageType(MessageType type) =>
        type switch
        {
            // Definitions and 2PC controls are ordered/reliable-only.
            // Keep this rejection explicit even if the UDP allow-list expands.
            MessageType.AnimSceneDefinition or
                MessageType.AnimSceneControl => false,
            MessageType.Heartbeat or
                MessageType.PlayerState or
                MessageType.PlayerAnimationState or
                MessageType.EntityUpdate or
                MessageType.PlayerMountState or
                MessageType.MissionCameraState or
                MessageType.AnimSceneReplicaState => true,
            _ => false
        };

    // Two senders match only when both their computer address and their UDP port match.
    private static bool EndpointsEqual(IPEndPoint left, IPEndPoint right) =>
        left.Port == right.Port && left.Address.Equals(right.Address);
}
