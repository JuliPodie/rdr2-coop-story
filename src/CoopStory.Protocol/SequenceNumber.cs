namespace CoopStory.Protocol;

// Unsigned sequence arithmetic that still works when a 32-bit counter wraps.
// A candidate less than half the number space ahead is considered newer.
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

    // Strict in-order tracker: useful when receiving an older state would undo the current state.
    // It never accepts a late packet, even if unseen.
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

// Sliding 64-message replay window for UDP.
// It permits a previously unseen late packet within the window, but rejects duplicates and very old traffic.
public sealed class SequenceReplayWindow
{
    private const int WindowSize = 64;
    private bool _hasValue;
    private uint _latest;
    private ulong _seen;

    public bool HasValue => _hasValue;

    public uint Latest => _hasValue
        ? _latest
        : throw new InvalidOperationException("No sequence has been accepted.");

    public bool TryAccept(uint sequence)
    {
        if (!_hasValue)
        {
            _latest = sequence;
            _seen = 1;
            _hasValue = true;
            return true;
        }

        // A newer highest number shifts the seen-bit window forward.
        // Bit zero always represents the newest accepted sequence.
        if (SequenceNumber.IsNewer(sequence, _latest))
        {
            var advance = unchecked(sequence - _latest);
            _seen = advance >= WindowSize
                ? 1
                : (_seen << (int)advance) | 1;
            _latest = sequence;
            return true;
        }

        // A not-newer packet can still be useful only if it is inside the small window and its bit proves it was not delivered already.
        var distance = unchecked(_latest - sequence);
        if (distance >= WindowSize)
        {
            return false;
        }

        var bit = 1UL << (int)distance;
        if ((_seen & bit) != 0)
        {
            return false;
        }

        _seen |= bit;
        return true;
    }

    public void Reset()
    {
        _hasValue = false;
        _latest = 0;
        _seen = 0;
    }
}
