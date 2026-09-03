namespace CoopStory.Protocol;

// Transport-neutral protocol frame. Type selects its payload codec; sequence
// orders that message stream; tick records the sender's monotonic game time.

//This sealed class, will carry a "payload" of bytes, which will be interpreted by the receiving end based on the "Type" field.
//The "Sequence" field is used to maintain the order of messages, and the "Tick" field represents the sender's game time at the moment of sending.
public sealed record ProtocolEnvelope(MessageType Type, uint Sequence, ulong Tick, ReadOnlyMemory<byte> Payload)
{

    public ushort Version { get; init; } = ProtocolConstants.Version;

    //creates and returns a new ProtocolEnvelope instance with the specified parameters.
    public static ProtocolEnvelope Create(MessageType type, uint sequence, ulong tick, ReadOnlyMemory<byte> payload) => new(type, sequence, tick, payload);
}

// A peer/frame validation failure. Networking layers catch this separately from
// ordinary socket failures so they can reject bad data without hiding the cause.

//basically creates a custom exception class called ProtocolException that inherits from the base Exception class.
//This exception is intended to be thrown when there is a validation failure in the protocol, allowing networking layers to catch it separately from ordinary socket failures
//and reject bad data without hiding the cause.
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
