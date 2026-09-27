using CoopStory.Protocol;

namespace CoopStory.Sidecar.Session;

// Saved record of one verified mission completion.
// EventId makes a completion unique, so a resend after a disconnect cannot grant/apply it a second time.
public sealed record MissionProgressionCompletionRecord(
    uint MissionId,
    uint MissionEpoch,
    ulong EventId,
    MissionProgressionFlags Flags,
    byte CompletionRating,
    int CompletionCashAward,
    long RecordedAtUnixMilliseconds,
    long? GuestAcknowledgedAtUnixMilliseconds = null)
{
    public MissionProgressionCompletionRecord Validate()
    {
        if (MissionId == 0 || MissionEpoch == 0 || EventId == 0 ||
            Flags != MissionProgressionFlags.VerifiedCompletionMapping ||
            CompletionRating is < 2 or > 5 || CompletionCashAward != 0 ||
            RecordedAtUnixMilliseconds <= 0 ||
            GuestAcknowledgedAtUnixMilliseconds is <= 0)
        {
            throw new ArgumentException("Mission progression completion record is invalid.");
        }
        return this;
    }

    public MissionProgressionPayload ToPayload() => new(
        MissionId,
        MissionEpoch,
        EventId,
        MissionProgressionPhase.Completion,
        Flags,
        CompletionRating,
        CompletionCashAward);
}

public sealed class MissionProgressionJournal
{
    private readonly object _gate = new();
    private readonly Dictionary<ulong, MissionProgressionCompletionRecord> _events = [];

    public bool Record(MissionProgressionPayload payload, long recordedAtUnixMilliseconds)
    {
        if (payload.Phase != MissionProgressionPhase.Completion)
        {
            throw new ArgumentException("Only completion payloads can be journaled.");
        }
        var record = new MissionProgressionCompletionRecord(
            payload.MissionId,
            payload.MissionEpoch,
            payload.EventId,
            payload.Flags,
            payload.CompletionRating,
            payload.CompletionCashAward,
            recordedAtUnixMilliseconds).Validate();
        lock (_gate)
        {
            if (_events.TryGetValue(record.EventId, out var existing))
            {
                if (existing.MissionId != record.MissionId ||
                    existing.MissionEpoch != record.MissionEpoch ||
                    existing.Flags != record.Flags ||
                    existing.CompletionRating != record.CompletionRating ||
                    existing.CompletionCashAward != record.CompletionCashAward)
                {
                    throw new ArgumentException(
                        "Mission progression event identity was reused for different data.");
                }
                return false;
            }
            _events.Add(record.EventId, record);
            return true;
        }
    }

    public bool Acknowledge(
        MissionProgressionPayload payload,
        long acknowledgedAtUnixMilliseconds)
    {
        if (payload.Phase != MissionProgressionPhase.Applied ||
            acknowledgedAtUnixMilliseconds <= 0)
        {
            throw new ArgumentException("Mission progression acknowledgement is invalid.");
        }
        lock (_gate)
        {
            if (!_events.TryGetValue(payload.EventId, out var record) ||
                record.MissionId != payload.MissionId ||
                record.MissionEpoch != payload.MissionEpoch ||
                record.GuestAcknowledgedAtUnixMilliseconds.HasValue)
            {
                return false;
            }
            _events[payload.EventId] = record with
            {
                GuestAcknowledgedAtUnixMilliseconds = acknowledgedAtUnixMilliseconds
            };
            return true;
        }
    }

    public IReadOnlyList<MissionProgressionCompletionRecord> CapturePending()
    {
        lock (_gate)
        {
            return _events.Values
                .Where(static record => !record.GuestAcknowledgedAtUnixMilliseconds.HasValue)
                .OrderBy(static record => record.RecordedAtUnixMilliseconds)
                .ToArray();
        }
    }

    public IReadOnlyList<MissionProgressionCompletionRecord> CaptureState()
    {
        lock (_gate) return _events.Values.OrderBy(static record => record.EventId).ToArray();
    }

    public void Restore(IEnumerable<MissionProgressionCompletionRecord> records)
    {
        ArgumentNullException.ThrowIfNull(records);
        lock (_gate)
        {
            _events.Clear();
            foreach (var record in records.OrderBy(static record => record.EventId))
            {
                var validated = record.Validate();
                if (!_events.TryAdd(validated.EventId, validated))
                {
                    throw new ArgumentException(
                        "Mission progression journal contains a duplicate event.");
                }
            }
        }
    }
}
