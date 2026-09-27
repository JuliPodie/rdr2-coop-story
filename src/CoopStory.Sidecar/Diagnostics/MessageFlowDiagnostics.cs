using CoopStory.Protocol;

namespace CoopStory.Sidecar.Diagnostics;

// Tracks both sides of the session bridge separately: native-game to LAN and LAN back to native-game, which isolates where a visible gap originated.
internal enum MessageFlowDirection
{
    BridgeToNetwork,
    NetworkToBridge
}

internal readonly record struct MessageFlowStreamSnapshot(
    MessageFlowDirection Direction,
    MessageType MessageType,
    long Observed,
    long Delivered,
    long Dropped,
    long Coalesced,
    long LastObservedAgeMs,
    double AverageGapMs,
    long P95GapMs,
    long MaximumGapMs);

/// <summary>
/// Bounded, thread-safe transport telemetry.
/// It deliberately stores only counters and monotonic arrival gaps: never payloads, invite codes, IP addresses or process-local handles.
/// </summary>
internal sealed class MessageFlowDiagnostics
{
    // Retain a bounded recent sample for p95 timing; full payload history would be expensive and could accidentally expose private session material.
    private const int MaximumRecentGaps = 128;
    private readonly object _sync = new();
    private readonly Dictionary<StreamKey, MutableStream> _streams = [];

    public void Observe(
        MessageFlowDirection direction,
        MessageType messageType,
        long nowMs = -1)
    {
        nowMs = ResolveNow(nowMs);
        lock (_sync)
        {
            var stream = GetOrCreate(direction, messageType);
            // Arrival gaps measure actual observation time, including a quiet game bridge or UDP loss, rather than trusting sender timestamps.
            if (stream.LastObservedMs >= 0)
            {
                var gap = Math.Max(0, nowMs - stream.LastObservedMs);
                stream.GapTotalMs += gap;
                stream.GapCount++;
                stream.MaximumGapMs = Math.Max(stream.MaximumGapMs, gap);
                stream.RecentGaps.Enqueue(gap);
                while (stream.RecentGaps.Count > MaximumRecentGaps)
                {
                    _ = stream.RecentGaps.Dequeue();
                }
            }

            stream.LastObservedMs = nowMs;
            stream.Observed++;
        }
    }

    public void MarkDelivered(
        MessageFlowDirection direction,
        MessageType messageType)
    {
        lock (_sync)
        {
            GetOrCreate(direction, messageType).Delivered++;
        }
    }

    public void MarkDropped(
        MessageFlowDirection direction,
        MessageType messageType)
    {
        lock (_sync)
        {
            GetOrCreate(direction, messageType).Dropped++;
        }
    }

    public void MarkCoalesced(
        MessageFlowDirection direction,
        MessageType messageType)
    {
        lock (_sync)
        {
            GetOrCreate(direction, messageType).Coalesced++;
        }
    }

    public IReadOnlyList<MessageFlowStreamSnapshot> Capture(long nowMs = -1)
    {
        nowMs = ResolveNow(nowMs);
        lock (_sync)
        {
            return _streams
                .OrderBy(static pair => pair.Key.Direction)
                .ThenBy(static pair => pair.Key.MessageType)
                .Select(pair => CreateSnapshot(pair.Key, pair.Value, nowMs))
                .ToArray();
        }
    }

    public MessageFlowStreamSnapshot? Capture(
        MessageFlowDirection direction,
        MessageType messageType,
        long nowMs = -1)
    {
        nowMs = ResolveNow(nowMs);
        lock (_sync)
        {
            var key = new StreamKey(direction, messageType);
            return _streams.TryGetValue(key, out var stream)
                ? CreateSnapshot(key, stream, nowMs)
                : null;
        }
    }

    public void Reset()
    {
        lock (_sync)
        {
            _streams.Clear();
        }
    }

    private MutableStream GetOrCreate(
        MessageFlowDirection direction,
        MessageType messageType)
    {
        var key = new StreamKey(direction, messageType);
        if (!_streams.TryGetValue(key, out var stream))
        {
            stream = new MutableStream();
            _streams.Add(key, stream);
        }
        return stream;
    }

    private static MessageFlowStreamSnapshot CreateSnapshot(
        StreamKey key,
        MutableStream stream,
        long nowMs)
    {
        var recent = stream.RecentGaps.Order().ToArray();
        // Sort only the bounded rolling window when capturing diagnostics, then publish p95 alongside average and maximum for jitter investigations.
        var p95 = recent.Length == 0
            ? 0L
            : recent[Math.Min(
                recent.Length - 1,
                (int)Math.Ceiling(recent.Length * 0.95) - 1)];
        return new MessageFlowStreamSnapshot(
            key.Direction,
            key.MessageType,
            stream.Observed,
            stream.Delivered,
            stream.Dropped,
            stream.Coalesced,
            stream.LastObservedMs < 0
                ? -1
                : Math.Max(0, nowMs - stream.LastObservedMs),
            stream.GapCount == 0
                ? 0.0
                : (double)stream.GapTotalMs / stream.GapCount,
            p95,
            stream.MaximumGapMs);
    }

    private static long ResolveNow(long nowMs) =>
        nowMs >= 0 ? nowMs : Environment.TickCount64;

    private readonly record struct StreamKey(
        MessageFlowDirection Direction,
        MessageType MessageType);

    private sealed class MutableStream
    {
        public long Observed;
        public long Delivered;
        public long Dropped;
        public long Coalesced;
        public long LastObservedMs = -1;
        public long GapTotalMs;
        public long GapCount;
        public long MaximumGapMs;
        public Queue<long> RecentGaps { get; } = new();
    }
}
