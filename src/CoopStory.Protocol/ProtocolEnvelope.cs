namespace CoopStory.Protocol;

public sealed record ProtocolEnvelope(
    MessageType Type,
    uint Sequence,
    ulong Tick,
    ReadOnlyMemory<byte> Payload)
{
    public ushort Version { get; init; } = ProtocolConstants.Version;

    public static ProtocolEnvelope Create(
        MessageType type,
        uint sequence,
        ulong tick,
        ReadOnlyMemory<byte> payload) =>
        new(type, sequence, tick, payload);
}

public sealed class ProtocolException : Exception
{
    public ProtocolException(string message)
        : base(message)
    {
    }

    public ProtocolException(string message, Exception innerException)
        : base(message, innerException)
    {
    }
}
