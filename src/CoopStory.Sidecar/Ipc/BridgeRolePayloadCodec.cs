using CoopStory.Protocol;

namespace CoopStory.Sidecar.Ipc;

// Tiny local-pipe role negotiation payload.
// It is deliberately binary and exact: only Host (0) or Guest (1) may activate gameplay forwarding.
public static class BridgeRolePayloadCodec
{
    public const int PayloadSize = 1;

    public static byte[] Encode(SessionRole role) =>
        role switch
        {
            SessionRole.Host => [0],
            SessionRole.Guest => [1],
            _ => throw new ProtocolException(
                $"Unsupported bridge role value {(int)role}; expected Host=0 or Guest=1.")
        };

    // Reject length/value ambiguity before the bridge role influences session authority or chooses which messages it is allowed to send.
    public static SessionRole Decode(ReadOnlySpan<byte> payload)
    {
        if (payload.Length != PayloadSize)
        {
            throw new ProtocolException(
                $"Bridge role payload must contain exactly {PayloadSize} byte.");
        }

        return payload[0] switch
        {
            0 => SessionRole.Host,
            1 => SessionRole.Guest,
            _ => throw new ProtocolException(
                $"Unsupported bridge role byte {payload[0]}; expected 0 or 1.")
        };
    }
}
