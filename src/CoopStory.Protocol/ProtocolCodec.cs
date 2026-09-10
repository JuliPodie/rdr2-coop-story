using System.Buffers.Binary;

namespace CoopStory.Protocol;

// Packs and reads one complete multiplayer message using the format both computers agree on.
// Each message starts with a header describing its type, sequence, time, and payload length.
// The payload is the message contents, such as the bytes representing PlayerStatePayload.
// BinaryPayloadCodec and the other payload codecs interpret those contents after this file reads the envelope.
// The same format is used between RDR2 and its local Sidecar, and between the two players' Sidecars.
public static class ProtocolCodec
{
    // Turn an envelope into bytes that the connection can carry.
    // This only builds the bytes; the caller still needs to send them.
    public static byte[] Encode(ProtocolEnvelope envelope)
    {
        ArgumentNullException.ThrowIfNull(envelope);
        ValidateEnvelope(envelope);

        // Reserve space for both the 24-byte header and the already-packed payload.
        // A Span is a view into that array, so writing into header changes bytes directly.
        // Little-endian means a number's least significant byte is stored first.
        // Both the C# and C++ readers must use this same order to get the original numbers back.
        var bytes = new byte[ProtocolConstants.HeaderSize + envelope.Payload.Length];
        var header = bytes.AsSpan(0, ProtocolConstants.HeaderSize);
        // These offsets are byte positions in the agreed header layout, starting at zero.
        // Magic identifies our format, while Version identifies which layout the sender uses.
        BinaryPrimitives.WriteUInt32LittleEndian(header, ProtocolConstants.Magic);
        BinaryPrimitives.WriteUInt16LittleEndian(header[4..], envelope.Version);
        BinaryPrimitives.WriteUInt16LittleEndian(header[6..], (ushort)envelope.Type);
        BinaryPrimitives.WriteUInt32LittleEndian(header[8..], envelope.Sequence);
        BinaryPrimitives.WriteUInt64LittleEndian(header[12..], envelope.Tick);
        BinaryPrimitives.WriteUInt32LittleEndian(header[20..], (uint)envelope.Payload.Length);
        // Append the payload exactly as supplied, without changing positions, health, or other game values.
        envelope.Payload.Span.CopyTo(bytes.AsSpan(ProtocolConstants.HeaderSize));
        return bytes;
    }

    // Read a message when its complete byte array is already available, as with a received UDP datagram.
    // Reject missing or extra bytes instead of letting a damaged message reach multiplayer gameplay code.
    public static ProtocolEnvelope Decode(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length < ProtocolConstants.HeaderSize)
        {
            throw new ProtocolException(
                $"Frame is shorter than the {ProtocolConstants.HeaderSize}-byte header.");
        }

        var payloadLength = ValidateAndReadPayloadLength(bytes[..ProtocolConstants.HeaderSize]);
        var expectedLength = checked(ProtocolConstants.HeaderSize + payloadLength);
        if (bytes.Length != expectedLength)
        {
            throw new ProtocolException(
                $"Frame length mismatch. Expected {expectedLength} bytes, received {bytes.Length}.");
        }

        return DecodeParts(
            bytes[..ProtocolConstants.HeaderSize],
            bytes[ProtocolConstants.HeaderSize..].ToArray());
    }

    // Read one envelope from a stream, such as TCP or the local named pipe to RDR2.
    // Return null if the connection ends cleanly before a new message starts.
    // Ending partway through a message is an error because the missing fields cannot be guessed.
    public static async ValueTask<ProtocolEnvelope?> ReadAsync(
        Stream stream,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(stream);

        // One read is allowed to return only some of the requested bytes.
        // First collect the complete header so we know how many payload bytes to read next.
        // Check that length before allocating an array based on information from the other computer.
        var header = new byte[ProtocolConstants.HeaderSize];
        var hasFrame = await ReadExactlyAsync(
            stream,
            header,
            allowCleanEndOfStream: true,
            cancellationToken).ConfigureAwait(false);
        if (!hasFrame)
        {
            return null;
        }

        var payloadLength = ValidateAndReadPayloadLength(header);
        var payload = new byte[payloadLength];
        if (payloadLength > 0)
        {
            await ReadExactlyAsync(
                stream,
                payload,
                allowCleanEndOfStream: false,
                cancellationToken).ConfigureAwait(false);
        }

        return DecodeParts(header, payload);
    }

    // Package the envelope, write all of its bytes, and ask the stream to flush any buffered data.
    // Completing this write does not prove that the other player's game has applied the message.
    public static async ValueTask WriteAsync(
        Stream stream,
        ProtocolEnvelope envelope,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(stream);
        var bytes = Encode(envelope);
        await stream.WriteAsync(bytes, cancellationToken).ConfigureAwait(false);
        await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
    }

    // Rebuild the envelope after the header and payload have been collected separately.
    // Sequence and Tick are carried to the next layer for its ordering and timing decisions.
    // This method itself does not sort messages or move a player.
    private static ProtocolEnvelope DecodeParts(
        ReadOnlySpan<byte> header,
        ReadOnlyMemory<byte> payload)
    {
        // Message type is validated before construction so callers never switch on an undefined numeric value from a peer.
        var version = BinaryPrimitives.ReadUInt16LittleEndian(header[4..]);
        var typeValue = BinaryPrimitives.ReadUInt16LittleEndian(header[6..]);
        if (!Enum.IsDefined(typeof(MessageType), typeValue))
        {
            throw new ProtocolException($"Unknown message type {typeValue}.");
        }

        return new ProtocolEnvelope(
            (MessageType)typeValue,
            BinaryPrimitives.ReadUInt32LittleEndian(header[8..]),
            BinaryPrimitives.ReadUInt64LittleEndian(header[12..]),
            payload)
        {
            Version = version
        };
    }

    // Check the identifying number, protocol version, and declared payload size.
    // For example, a player running a different protocol version is rejected before its bytes are misread.
    private static int ValidateAndReadPayloadLength(ReadOnlySpan<byte> header)
    {
        // Validate framing/version/size before allocating or decoding payload fields.
        // This is the first protocol boundary against malformed input.
        var magic = BinaryPrimitives.ReadUInt32LittleEndian(header);
        if (magic != ProtocolConstants.Magic)
        {
            throw new ProtocolException($"Invalid frame magic 0x{magic:X8}.");
        }

        var version = BinaryPrimitives.ReadUInt16LittleEndian(header[4..]);
        if (version != ProtocolConstants.Version)
        {
            throw new ProtocolException(
                $"Unsupported protocol version {version}; expected {ProtocolConstants.Version}.");
        }

        var payloadLength = BinaryPrimitives.ReadUInt32LittleEndian(header[20..]);
        if (payloadLength > ProtocolConstants.MaxPayloadSize)
        {
            throw new ProtocolException(
                $"Payload length {payloadLength} exceeds {ProtocolConstants.MaxPayloadSize} bytes.");
        }

        return checked((int)payloadLength);
    }

    // Check our outgoing envelope too, so a local mistake cannot produce a message the other PC cannot read.
    // Payload-specific rules, such as whether a health value is allowed, belong to the payload codec.
    private static void ValidateEnvelope(ProtocolEnvelope envelope)
    {
        if (envelope.Version != ProtocolConstants.Version)
        {
            throw new ProtocolException(
                $"Cannot encode protocol version {envelope.Version}; expected {ProtocolConstants.Version}.");
        }

        if (!Enum.IsDefined(envelope.Type))
        {
            throw new ProtocolException($"Cannot encode unknown message type {(ushort)envelope.Type}.");
        }

        if (envelope.Payload.Length > ProtocolConstants.MaxPayloadSize)
        {
            throw new ProtocolException(
                $"Payload length {envelope.Payload.Length} exceeds {ProtocolConstants.MaxPayloadSize} bytes.");
        }
    }

    // Keep filling the destination until the requested header or payload is complete.
    // offset remembers how many bytes are already present so each read continues in the right place.
    private static async ValueTask<bool> ReadExactlyAsync(
        Stream stream,
        Memory<byte> destination,
        bool allowCleanEndOfStream,
        CancellationToken cancellationToken)
    {
        var offset = 0;
        // Keep reading until a complete frame part arrives; a partial stream read is normal transport behavior, not a partial protocol message.
        while (offset < destination.Length)
        {
            var read = await stream.ReadAsync(destination[offset..], cancellationToken)
                .ConfigureAwait(false);
            if (read == 0)
            {
                // A zero-byte read means the stream ended, rather than a temporary lack of game updates.
                if (allowCleanEndOfStream && offset == 0)
                {
                    return false;
                }

                throw new EndOfStreamException(
                    $"Stream ended after {offset} of {destination.Length} expected bytes.");
            }

            offset += read;
        }

        return true;
    }
}
