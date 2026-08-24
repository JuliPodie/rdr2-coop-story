using System.Net;
using CoopStory.Protocol;

namespace CoopStory.Sidecar.Networking;

internal sealed class UdpPeerBinding
{
    private readonly IPAddress _expectedAddress;
    private readonly int? _expectedPort;
    private readonly uint? _controlSequenceFloor;
    private readonly SequenceReplayWindow _sequences = new();
    private IPEndPoint? _pinnedEndpoint;

    public UdpPeerBinding(
        IPAddress expectedAddress,
        int? expectedPort = null,
        uint? controlSequenceFloor = null,
        Guid? expectedInstanceId = null)
    {
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

    public Guid? ExpectedInstanceId { get; }

    public IPEndPoint? PinnedEndpoint => _pinnedEndpoint is null
        ? null
        : new IPEndPoint(_pinnedEndpoint.Address, _pinnedEndpoint.Port);

    public bool IsSourceAllowed(IPEndPoint source)
    {
        ArgumentNullException.ThrowIfNull(source);
        if (!source.Address.Equals(_expectedAddress) ||
            (_expectedPort.HasValue && source.Port != _expectedPort.Value))
        {
            return false;
        }

        return _pinnedEndpoint is null ||
            EndpointsEqual(_pinnedEndpoint, source);
    }

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

        if (!IsUdpMessageType(envelope.Type))
        {
            rejectionReason = "message-type";
            return false;
        }

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
            rejectionReason = "sequence-replay";
            return false;
        }

        _pinnedEndpoint ??= new IPEndPoint(source.Address, source.Port);
        rejectionReason = string.Empty;
        return true;
    }

    private static bool IsUdpMessageType(MessageType type) =>
        type switch
        {
            // Definitions and 2PC controls are ordered/reliable-only. Keep
            // this rejection explicit even if the UDP allow-list expands.
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

    private static bool EndpointsEqual(IPEndPoint left, IPEndPoint right) =>
        left.Port == right.Port && left.Address.Equals(right.Address);
}
