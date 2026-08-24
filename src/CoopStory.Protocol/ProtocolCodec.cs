using System.Buffers.Binary;

namespace CoopStory.Protocol;

public static class ProtocolCodec
{
    public static byte[] Encode(ProtocolEnvelope envelope)
    {
        ArgumentNullException.ThrowIfNull(envelope);
        ValidateEnvelope(envelope);

        var bytes = new byte[ProtocolConstants.HeaderSize + envelope.Payload.Length];
        var header = bytes.AsSpan(0, ProtocolConstants.HeaderSize);
        BinaryPrimitives.WriteUInt32LittleEndian(header, ProtocolConstants.Magic);
        BinaryPrimitives.WriteUInt16LittleEndian(header[4..], envelope.Version);
        BinaryPrimitives.WriteUInt16LittleEndian(header[6..], (ushort)envelope.Type);
        BinaryPrimitives.WriteUInt32LittleEndian(header[8..], envelope.Sequence);
        BinaryPrimitives.WriteUInt64LittleEndian(header[12..], envelope.Tick);
        BinaryPrimitives.WriteUInt32LittleEndian(header[20..], (uint)envelope.Payload.Length);
        envelope.Payload.Span.CopyTo(bytes.AsSpan(ProtocolConstants.HeaderSize));
        return bytes;
    }

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

    public static async ValueTask<ProtocolEnvelope?> ReadAsync(
        Stream stream,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(stream);

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

    private static ProtocolEnvelope DecodeParts(
        ReadOnlySpan<byte> header,
        ReadOnlyMemory<byte> payload)
    {
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

    private static int ValidateAndReadPayloadLength(ReadOnlySpan<byte> header)
    {
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

    private static async ValueTask<bool> ReadExactlyAsync(
        Stream stream,
        Memory<byte> destination,
        bool allowCleanEndOfStream,
        CancellationToken cancellationToken)
    {
        var offset = 0;
        while (offset < destination.Length)
        {
            var read = await stream.ReadAsync(destination[offset..], cancellationToken)
                .ConfigureAwait(false);
            if (read == 0)
            {
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
