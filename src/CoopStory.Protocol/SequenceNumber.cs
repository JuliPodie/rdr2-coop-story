namespace CoopStory.Protocol;

public static class SequenceNumber
{
    public static bool IsNewer(uint candidate, uint reference) =>
        candidate != reference && unchecked(candidate - reference) < 0x8000_0000u;
}

public sealed class SequenceTracker
{
    private bool _hasValue;
    private uint _latest;

    public bool HasValue => _hasValue;

    public uint Latest => _hasValue
        ? _latest
        : throw new InvalidOperationException("No sequence has been accepted.");

    public bool TryAccept(uint sequence)
    {
        if (!_hasValue)
        {
            _latest = sequence;
            _hasValue = true;
            return true;
        }

        if (!SequenceNumber.IsNewer(sequence, _latest))
        {
            return false;
        }

        _latest = sequence;
        return true;
    }

    public void Reset()
    {
        _hasValue = false;
        _latest = 0;
    }
}

public sealed class SequenceReplayWindow
{
    private readonly int _windowSize;
    private readonly ulong[] _seen;
    private bool _hasValue;
    private uint _latest;

    public SequenceReplayWindow(int windowSize = 64)
    {
        if (windowSize is < 64 or > 16_384 || windowSize % 64 != 0)
        {
            throw new ArgumentOutOfRangeException(
                nameof(windowSize),
                "Replay window must be a multiple of 64 between 64 and 16384.");
        }

        _windowSize = windowSize;
        _seen = new ulong[windowSize / 64];
    }

    public bool HasValue => _hasValue;

    public uint Latest => _hasValue
        ? _latest
        : throw new InvalidOperationException("No sequence has been accepted.");

    public bool TryAccept(uint sequence)
    {
        if (!_hasValue)
        {
            _latest = sequence;
            _seen[0] = 1;
            _hasValue = true;
            return true;
        }

        if (SequenceNumber.IsNewer(sequence, _latest))
        {
            var advance = unchecked(sequence - _latest);
            if (advance >= _windowSize)
            {
                Array.Clear(_seen);
            }
            else
            {
                var wordAdvance = (int)(advance / 64);
                var bitAdvance = (int)(advance % 64);
                // Shift from the oldest word so every source still contains
                // its original bits, including carries over word boundaries.
                for (var index = _seen.Length - 1; index >= 0; index--)
                {
                    var source = index - wordAdvance;
                    var value = source >= 0
                        ? _seen[source] << bitAdvance
                        : 0;
                    if (bitAdvance != 0 && source > 0)
                    {
                        value |= _seen[source - 1] >> (64 - bitAdvance);
                    }
                    _seen[index] = value;
                }
            }
            _seen[0] |= 1;
            _latest = sequence;
            return true;
        }

        var distance = unchecked(_latest - sequence);
        if (distance >= _windowSize)
        {
            return false;
        }

        var word = (int)(distance / 64);
        var bit = 1UL << (int)(distance % 64);
        if ((_seen[word] & bit) != 0)
        {
            return false;
        }

        _seen[word] |= bit;
        return true;
    }

    public void Reset()
    {
        _hasValue = false;
        _latest = 0;
        Array.Clear(_seen);
    }
}
