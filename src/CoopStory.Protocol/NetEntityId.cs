using System.Globalization;

namespace CoopStory.Protocol;

public readonly record struct NetEntityId(ulong Value) : ISpanFormattable
{
    public uint Epoch => (uint)(Value >> 32);

    public uint Counter => (uint)Value;

    public bool IsNone => Value == 0;

    public bool IsValid => Epoch != 0 && Counter != 0;

    public static NetEntityId None => default;

    public static NetEntityId Create(uint epoch, uint counter)
    {
        if (epoch == 0)
        {
            throw new ArgumentOutOfRangeException(nameof(epoch), "Epoch zero is reserved.");
        }

        if (counter == 0)
        {
            throw new ArgumentOutOfRangeException(nameof(counter), "Counter zero is reserved.");
        }

        return new NetEntityId(((ulong)epoch << 32) | counter);
    }

    public static bool TryParse(string? value, out NetEntityId entityId)
    {
        entityId = default;
        if (string.IsNullOrWhiteSpace(value))
        {
            return false;
        }

        var separator = value.IndexOf(':');
        if (separator <= 0 || separator == value.Length - 1)
        {
            return false;
        }

        if (!uint.TryParse(
                value.AsSpan(0, separator),
                NumberStyles.HexNumber,
                CultureInfo.InvariantCulture,
                out var epoch) ||
            !uint.TryParse(
                value.AsSpan(separator + 1),
                NumberStyles.HexNumber,
                CultureInfo.InvariantCulture,
                out var counter) ||
            epoch == 0 ||
            counter == 0)
        {
            return false;
        }

        entityId = Create(epoch, counter);
        return true;
    }

    public override string ToString() => $"{Epoch:X8}:{Counter:X8}";

    public string ToString(string? format, IFormatProvider? formatProvider) =>
        Value.ToString(format, formatProvider);

    public bool TryFormat(
        Span<char> destination,
        out int charsWritten,
        ReadOnlySpan<char> format,
        IFormatProvider? provider) =>
        Value.TryFormat(destination, out charsWritten, format, provider);
}

public sealed class NetEntityIdAllocator
{
    private readonly uint _epoch;
    private uint _counter;

    public NetEntityIdAllocator(uint epoch, uint initialCounter = 0)
    {
        if (epoch == 0)
        {
            throw new ArgumentOutOfRangeException(nameof(epoch), "Epoch zero is reserved.");
        }

        _epoch = epoch;
        _counter = initialCounter;
    }

    public NetEntityId Next()
    {
        var next = unchecked(++_counter);
        if (next == 0)
        {
            throw new InvalidOperationException("NetEntityId counter exhausted for this epoch.");
        }

        return NetEntityId.Create(_epoch, next);
    }
}
