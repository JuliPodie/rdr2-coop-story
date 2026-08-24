using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace CoopStory.Launcher;

internal sealed record DiagnosticLogLine(
    string Source,
    string Text,
    int Ordinal);

internal sealed record DiagnosticAnalysisReport(
    string TimelineMarkdown,
    string TimelineJson,
    string AnomaliesMarkdown,
    string AnomaliesJson,
    string MarkerWindowsMarkdown,
    string MarkerWindowsJson);

internal static partial class DiagnosticsTimelineAnalyzer
{
    private const int MaximumTimelineEvents = 10_000;
    private const int MaximumReportedAnomalies = 500;
    private const int CorrelationWindowMilliseconds = 2_000;
    private const int MarkerWindowBeforeMilliseconds = 10_000;
    private const int MarkerWindowAfterMilliseconds = 15_000;
    private const double PlayerPositionBudgetMeters = 1.5;
    private const double PlayerRotationBudgetDegrees = 35.0;
    private const double PlayerGaitPositionBudgetMeters = 0.75;
    private const double PlayerActionFreshnessBudgetMilliseconds = 750.0;
    private const double EntityPositionBudgetMeters = 1.5;

    [GeneratedRegex(
        @"^(?<timestamp>\d{4}-\d{2}-\d{2}T[^\s]+)\s+",
        RegexOptions.CultureInvariant,
        matchTimeoutMilliseconds: 1_000)]
    private static partial Regex PlainTimestampPattern();

    [GeneratedRegex(
        @"(?<key>[A-Za-z][A-Za-z0-9_.-]{1,50})\s*[=:]\s*(?<value>-?\d+(?:[.,]\d+)?)\s*(?<unit>ms|s|m)?",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant,
        matchTimeoutMilliseconds: 1_000)]
    private static partial Regex NumericMetricPattern();

    [GeneratedRegex(
        @"(?:^|[^A-Za-z])(?:role|slot)\s*[=:]\s*(?<role>host|guest)(?:$|[^A-Za-z])",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant,
        matchTimeoutMilliseconds: 1_000)]
    private static partial Regex RolePattern();

    [GeneratedRegex(
        @"\[(?<tag>[A-Za-z][A-Za-z0-9_-]{1,40})\]",
        RegexOptions.CultureInvariant,
        matchTimeoutMilliseconds: 1_000)]
    private static partial Regex TagPattern();

    [GeneratedRegex(
        @"^[0-9A-Fa-f]{12}$",
        RegexOptions.CultureInvariant,
        matchTimeoutMilliseconds: 1_000)]
    private static partial Regex SessionFingerprintPattern();

    internal static IReadOnlyList<string> Classify(string lower)
    {
        var categories = new List<string>(12);
        Add(
            "SESSION_HEALTH",
            ContainsAny(
                lower,
                "[session_health]", "session-health", "session.health",
                "\"event\":\"session.", "\"eventname\":\"session.") ||
            lower.Contains("diagnostics.transport-", StringComparison.Ordinal) ||
            lower.Contains("diagnostics.streaming", StringComparison.Ordinal) ||
            lower.Contains("peerconnected", StringComparison.Ordinal) ||
            lower.Contains("peer connected", StringComparison.Ordinal) ||
            lower.Contains("bridgepipe", StringComparison.Ordinal) ||
            lower.Contains("bridge pipe", StringComparison.Ordinal) ||
            lower.Contains("handshake", StringComparison.Ordinal) ||
            lower.Contains("connection-fault", StringComparison.Ordinal) ||
            lower.Contains("disconnected", StringComparison.Ordinal) ||
            lower.Contains("reconnect", StringComparison.Ordinal));
        Add(
            "MISSION_TIMELINE",
            HasTag(lower, "mission") ||
            lower.Contains("missionstate", StringComparison.Ordinal) ||
            lower.Contains("checkpoint", StringComparison.Ordinal) ||
            lower.Contains("spectator", StringComparison.Ordinal) ||
            lower.Contains("solooverride", StringComparison.Ordinal));
        Add(
            "MISSION_ISOLATION",
            ContainsAny(
                lower,
                "[mission_isolation]", "mission-isolation",
                "mission.isolation"));
        Add(
            "MISSION_ANCHOR",
            ContainsAny(
                lower,
                "[mission_anchor]", "mission-anchor", "mission.anchor"));
        Add(
            "ANIMGRAPH_TRAVERSAL",
            ContainsAny(
                lower,
                "[animgraph_traversal]", "animgraph-traversal",
                "animgraph.traversal"));
        Add(
            "PLAYER_DIVERGENCE",
            ContainsAny(
                lower,
                "[player_divergence]", "player-divergence",
                "player.divergence") ||
            ((lower.Contains("puppet", StringComparison.Ordinal) ||
             lower.Contains("playerstate", StringComparison.Ordinal) ||
             lower.Contains("player state", StringComparison.Ordinal) ||
             lower.Contains("replica", StringComparison.Ordinal) ||
             lower.Contains("animgraph", StringComparison.Ordinal) ||
             lower.Contains("motion", StringComparison.Ordinal)) &&
            ContainsAny(
                lower,
                "desync", "diverg", "error", "offset", "distance",
                "hard-snap", "hard snap", "teleport", "stale", "late",
                "frozen", "t-pose", "tpose", "correction")));
        Add(
            "ENTITY_DIVERGENCE",
            ContainsAny(
                lower,
                "[entity_divergence]", "entity-divergence",
                "entity.divergence") ||
            ((lower.Contains("entity", StringComparison.Ordinal) ||
             lower.Contains("world_pool", StringComparison.Ordinal) ||
             lower.Contains("world pool", StringComparison.Ordinal) ||
             lower.Contains("world_proxy", StringComparison.Ordinal) ||
             lower.Contains("world proxy", StringComparison.Ordinal) ||
             lower.Contains("npc", StringComparison.Ordinal) ||
             lower.Contains("horse", StringComparison.Ordinal)) &&
            ContainsAny(
                lower,
                "desync", "diverg", "error", "offset", "distance",
                "max-error", "correction", "orphan", "duplicate",
                "stale", "rejected", "missing", "despawn", "mismatch")));
        Add(
            "COMBAT_LIFECYCLE",
            HasTag(lower, "action_") ||
            ContainsAny(
                lower,
                "melee", "peer_combat", "peer-combat", "attack",
                "blocking", "grapple", "knockdown", "ragdoll", "downed",
                "revive", "victim_constraint", "victim-constraint"));
        Add(
            "LASSO_LIFECYCLE",
            ContainsAny(
                lower,
                "lasso", "rope", "hogtie", "hogtied", "tied", "binding"));
        Add(
            "MOUNT_LIFECYCLE",
            ContainsAny(
                lower,
                "mount", "dismount", "horse", "saddle", "rider"));
        Add(
            "TRANSPORT_GAP",
            ContainsAny(
                lower,
                "diagnostics.transport-gap", "diagnostics.transport-recovered",
                "[transport_gap]", "[transport-gap]"));
        Add(
            "USER_MARKER",
            lower.Contains("[user_marker]", StringComparison.Ordinal) ||
            lower.Contains("user-marker", StringComparison.Ordinal) ||
            lower.Contains("user.marker", StringComparison.Ordinal) ||
            lower.Contains("diagnosticmarker", StringComparison.Ordinal));
        Add(
            "PROBLEM_SNAPSHOT",
            lower.Contains("[problem_snapshot]", StringComparison.Ordinal) ||
            lower.Contains("problem-snapshot", StringComparison.Ordinal) ||
            lower.Contains("problem.snapshot", StringComparison.Ordinal));
        return categories;

        void Add(string category, bool condition)
        {
            if (condition)
            {
                categories.Add(category);
            }
        }
    }

    internal static DiagnosticAnalysisReport Analyze(
        IReadOnlyList<DiagnosticLogLine> lines,
        LauncherRole localRole)
    {
        var allParsed = lines
            .Select(line => Parse(line, localRole))
            .OrderBy(static item => item.TimestampUtc.HasValue ? 0 : 1)
            .ThenBy(static item => item.TimestampUtc)
            .ThenBy(static item => item.Ordinal)
            .ToList();
        PropagateSessionFingerprints(allParsed);
        var sessionFingerprints = allParsed
            .Select(static item => item.SessionFingerprint)
            .Where(static item => item is not null)
            .Select(static item => item!)
            .Distinct(StringComparer.Ordinal)
            .ToArray();
        var latestSessionFingerprint = allParsed
            .LastOrDefault(static item => item.SessionFingerprint is not null)
            ?.SessionFingerprint;
        var parsed = allParsed
            .Where(static item => item.Categories.Length > 0)
            .ToList();
        var totalRelevantEvents = parsed.Count;
        if (parsed.Count > MaximumTimelineEvents)
        {
            parsed = RetainTimeline(parsed);
        }

        for (var index = 0; index < parsed.Count; index++)
        {
            parsed[index].Id = index + 1;
        }

        var latestSessionEvents = latestSessionFingerprint is null
            ? parsed
            : parsed.Where(item =>
                    string.Equals(
                        item.SessionFingerprint,
                        latestSessionFingerprint,
                        StringComparison.Ordinal))
                .ToList();
        var allAnomalies = parsed
            .Where(IsAnomaly)
            .ToList();
        var anomalies = RetainAnomalies(allAnomalies);
        var firstPlayer = FindFirstDivergence(
            parsed,
            "PLAYER_DIVERGENCE",
            latestSessionFingerprint);
        var firstEntity = FindFirstDivergence(
            parsed,
            "ENTITY_DIVERGENCE",
            latestSessionFingerprint);
        var firstDivergence = new[] { firstPlayer, firstEntity }
            .Where(static item => item is not null)
            .OrderBy(static item => item!.TimestampUtc.HasValue ? 0 : 1)
            .ThenBy(static item => item!.TimestampUtc)
            .ThenBy(static item => item!.Ordinal)
            .FirstOrDefault();
        var statistics = BuildStatistics(latestSessionEvents);
        var anomalyViews = anomalies.Select(
                anomaly => new AnomalyView(
                    anomaly,
                    CorrelatedIds(parsed, anomaly)))
            .ToArray();
        var markerWindows = BuildMarkerWindows(parsed);

        var generatedAtUtc = DateTimeOffset.UtcNow;
        var timelineJson = JsonSerializer.Serialize(
            new
            {
                schemaVersion = 2,
                generatedAtUtc,
                localRole = localRole.ToString(),
                sessionFingerprints,
                latestSessionFingerprint,
                roleSemantics =
                    "role is the emitting archive side unless an explicit Host/Guest role was present in the event",
                correlationWindowMs = CorrelationWindowMilliseconds,
                totalRelevantEvents,
                retainedEvents = parsed.Count,
                events = parsed.Select(ToJsonEvent)
            },
            JsonSupport.Options);
        var anomaliesJson = JsonSerializer.Serialize(
            new
            {
                schemaVersion = 2,
                generatedAtUtc,
                localRole = localRole.ToString(),
                sessionFingerprints,
                latestSessionFingerprint,
                firstDivergence = ToNullableJsonEvent(firstDivergence),
                firstPlayerDivergence = ToNullableJsonEvent(firstPlayer),
                firstEntityDivergence = ToNullableJsonEvent(firstEntity),
                statistics,
                totalDetectedAnomalies = allAnomalies.Count,
                retainedAnomalies = anomalyViews.Length,
                anomalies = anomalyViews.Select(view => new
                {
                    kind = PrimaryCategory(view.Event),
                    eventData = ToJsonEvent(view.Event),
                    correlatedEventIds = view.CorrelatedEventIds
                })
            },
            JsonSupport.Options);

        var markerWindowsJson = JsonSerializer.Serialize(
            new
            {
                schemaVersion = 1,
                generatedAtUtc,
                localRole = localRole.ToString(),
                windowBeforeMs = MarkerWindowBeforeMilliseconds,
                windowAfterMs = MarkerWindowAfterMilliseconds,
                markerCount = markerWindows.Length,
                markers = markerWindows.Select(window => new
                {
                    correlationId = window.CorrelationId,
                    sessionFingerprint = window.Anchor.SessionFingerprint,
                    anchorEventId = window.Anchor.Id,
                    anchorTimestampUtc = window.Anchor.TimestampUtc,
                    markerEventIds = window.MarkerEvents.Select(
                        static item => item.Id),
                    events = window.ContextEvents.Select(ToJsonEvent)
                })
            },
            JsonSupport.Options);

        return new DiagnosticAnalysisReport(
            BuildTimelineMarkdown(
                parsed,
                localRole,
                totalRelevantEvents,
                latestSessionFingerprint,
                generatedAtUtc),
            timelineJson,
            BuildAnomaliesMarkdown(
                anomalyViews,
                parsed,
                localRole,
                firstDivergence,
                firstPlayer,
                firstEntity,
                statistics,
                allAnomalies.Count,
                latestSessionFingerprint,
                generatedAtUtc),
            anomaliesJson,
            BuildMarkerWindowsMarkdown(
                markerWindows,
                localRole,
                generatedAtUtc),
            markerWindowsJson);
    }

    private static TimelineEvent Parse(
        DiagnosticLogLine line,
        LauncherRole fallbackRole)
    {
        DateTimeOffset? timestamp = null;
        var severity = "info";
        var eventName = ExtractPlainEventName(line.Text);
        var message = line.Text;
        var role = fallbackRole.ToString();
        var metrics = ExtractPlainMetrics(line.Text);
        var transportBudgetExceeded = false;
        string? sessionFingerprint = null;

        try
        {
            using var document = JsonDocument.Parse(line.Text);
            var root = document.RootElement;
            timestamp = ReadTimestamp(root);
            severity = ReadString(root, "level") ??
                       ReadString(root, "severity") ??
                       severity;
            eventName = ReadString(root, "event") ??
                        ReadString(root, "eventName") ??
                        eventName;
            message = ReadString(root, "message") ?? message;
            role = ReadRole(root) ?? role;
            sessionFingerprint = ReadSessionFingerprint(root);
            CollectNumericMetrics(root, metrics, depth: 0, path: string.Empty);
            transportBudgetExceeded =
                EvaluateMessageFlowBudgets(root, metrics);
        }
        catch (JsonException)
        {
            var timestampMatch = PlainTimestampPattern().Match(line.Text);
            if (timestampMatch.Success &&
                DateTimeOffset.TryParse(
                    timestampMatch.Groups["timestamp"].Value,
                    CultureInfo.InvariantCulture,
                    DateTimeStyles.AssumeUniversal |
                    DateTimeStyles.AdjustToUniversal,
                    out var parsedTimestamp))
            {
                timestamp = parsedTimestamp;
            }
            var roleMatch = RolePattern().Match(line.Text);
            if (roleMatch.Success)
            {
                role = NormalizeRole(roleMatch.Groups["role"].Value) ?? role;
            }
            severity = InferSeverity(line.Text);
        }

        var lower = line.Text.ToLowerInvariant();
        var categories = DiagnosticsTimelineAnalyzer.Classify(lower).ToList();
        if (transportBudgetExceeded &&
            !categories.Contains("TRANSPORT_GAP"))
        {
            categories.Add("TRANSPORT_GAP");
        }
        return new TimelineEvent(
            line.Ordinal,
            timestamp,
            line.Source,
            role,
            severity.ToLowerInvariant(),
            Truncate(eventName, 100),
            Truncate(message, 600),
            categories.ToArray(),
            metrics,
            transportBudgetExceeded,
            sessionFingerprint);
    }

    private static string? ReadSessionFingerprint(JsonElement root)
    {
        var direct = NormalizeSessionFingerprint(
            ReadString(root, "sessionFingerprint"));
        if (direct is not null)
        {
            return direct;
        }
        if (root.TryGetProperty("data", out var data) &&
            data.ValueKind == JsonValueKind.Object)
        {
            return NormalizeSessionFingerprint(
                ReadString(data, "sessionFingerprint"));
        }
        return null;
    }

    private static string? NormalizeSessionFingerprint(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return null;
        }
        var normalized = value.Trim();
        return SessionFingerprintPattern().IsMatch(normalized)
            ? normalized.ToLowerInvariant()
            : null;
    }

    private static void PropagateSessionFingerprints(
        IReadOnlyList<TimelineEvent> events)
    {
        string? current = null;
        foreach (var item in events)
        {
            if (item.SessionFingerprint is not null)
            {
                current = item.SessionFingerprint;
            }
            else
            {
                item.SessionFingerprint = current;
            }
        }
    }

    private static DateTimeOffset? ReadTimestamp(JsonElement root)
    {
        foreach (var propertyName in new[] { "timestampUtc", "timestamp" })
        {
            var text = ReadString(root, propertyName);
            if (text is not null &&
                DateTimeOffset.TryParse(
                    text,
                    CultureInfo.InvariantCulture,
                    DateTimeStyles.AssumeUniversal |
                    DateTimeStyles.AdjustToUniversal,
                    out var timestamp))
            {
                return timestamp;
            }
        }
        return null;
    }

    private static string? ReadRole(JsonElement root)
    {
        var direct = NormalizeRole(ReadString(root, "role"));
        if (direct is not null)
        {
            return direct;
        }
        if (root.TryGetProperty("data", out var data) &&
            data.ValueKind == JsonValueKind.Object)
        {
            return NormalizeRole(ReadString(data, "role")) ??
                   NormalizeRole(ReadString(data, "slot"));
        }
        return null;
    }

    private static string? NormalizeRole(string? role) =>
        role?.ToLowerInvariant() switch
        {
            "host" => "Host",
            "guest" => "Guest",
            _ => null
        };

    private static string? ReadString(JsonElement element, string name) =>
        element.ValueKind == JsonValueKind.Object &&
        element.TryGetProperty(name, out var property) &&
        property.ValueKind == JsonValueKind.String
            ? property.GetString()
            : null;

    private static Dictionary<string, double> ExtractPlainMetrics(string text)
    {
        var metrics = new Dictionary<string, double>(
            StringComparer.OrdinalIgnoreCase);
        foreach (Match match in NumericMetricPattern().Matches(text))
        {
            var raw = match.Groups["value"].Value.Replace(',', '.');
            if (!double.TryParse(
                    raw,
                    NumberStyles.Float,
                    CultureInfo.InvariantCulture,
                    out var value) ||
                !double.IsFinite(value))
            {
                continue;
            }
            var key = match.Groups["key"].Value;
            var unit = match.Groups["unit"].Value;
            if (unit.Equals("s", StringComparison.OrdinalIgnoreCase) &&
                !key.EndsWith("ms", StringComparison.OrdinalIgnoreCase))
            {
                value *= 1_000.0;
                key += "Ms";
            }
            metrics.TryAdd(key, value);
        }
        return metrics;
    }

    private static void CollectNumericMetrics(
        JsonElement element,
        Dictionary<string, double> metrics,
        int depth,
        string path)
    {
        if (depth > 6)
        {
            return;
        }
        if (element.ValueKind == JsonValueKind.Array)
        {
            var index = 0;
            foreach (var item in element.EnumerateArray())
            {
                CollectNumericMetrics(
                    item,
                    metrics,
                    depth + 1,
                    $"{path}[{index++}]");
            }
            return;
        }
        if (element.ValueKind != JsonValueKind.Object)
        {
            return;
        }
        foreach (var property in element.EnumerateObject())
        {
            var key = string.IsNullOrEmpty(path)
                ? property.Name
                : $"{path}.{property.Name}";
            if (property.Value.ValueKind == JsonValueKind.Number &&
                property.Value.TryGetDouble(out var value) &&
                double.IsFinite(value))
            {
                metrics.TryAdd(key, value);
            }
            else if (property.Value.ValueKind is
                     JsonValueKind.Object or JsonValueKind.Array)
            {
                CollectNumericMetrics(
                    property.Value,
                    metrics,
                    depth + 1,
                    key);
            }
        }
    }

    private static bool EvaluateMessageFlowBudgets(
        JsonElement root,
        Dictionary<string, double> metrics)
    {
        if (!root.TryGetProperty("data", out var data) ||
            data.ValueKind != JsonValueKind.Object ||
            !data.TryGetProperty("messageFlow", out var messageFlow) ||
            messageFlow.ValueKind != JsonValueKind.Array)
        {
            return false;
        }

        var anyExceeded = false;
        var index = 0;
        foreach (var stream in messageFlow.EnumerateArray())
        {
            if (stream.ValueKind != JsonValueKind.Object)
            {
                index++;
                continue;
            }
            var threshold = ReadNumber(stream, "thresholdMs") ??
                MessageFlowBudgetMilliseconds(
                    ReadString(stream, "messageType"));
            var observed = ReadNumber(stream, "observed") ?? 0.0;
            if (threshold is null || threshold <= 0.0 || observed <= 0.0)
            {
                index++;
                continue;
            }
            var exceeded = new[]
                {
                    ReadNumber(stream, "lastObservedAgeMs"),
                    ReadNumber(stream, "averageGapMs"),
                    ReadNumber(stream, "p95GapMs"),
                    ReadNumber(stream, "maximumGapMs")
                }
                .Where(static value => value.HasValue)
                .Any(value => value!.Value > threshold.Value);
            metrics.TryAdd(
                $"data.messageFlow[{index}].diagnosticBudgetMs",
                threshold.Value);
            metrics.TryAdd(
                $"data.messageFlow[{index}].budgetExceeded",
                exceeded ? 1.0 : 0.0);
            anyExceeded |= exceeded;
            index++;
        }
        return anyExceeded;
    }

    private static double? ReadNumber(JsonElement element, string name) =>
        element.TryGetProperty(name, out var property) &&
        property.ValueKind == JsonValueKind.Number &&
        property.TryGetDouble(out var value) &&
        double.IsFinite(value)
            ? value
            : null;

    private static double? MessageFlowBudgetMilliseconds(
        string? messageType) =>
        messageType?.ToLowerInvariant() switch
        {
            "playerstate" => 750.0,
            "playeranimationstate" => 1_000.0,
            "entityupdate" => 1_500.0,
            "missioncamerastate" => 500.0,
            _ => null
        };

    private static string ExtractPlainEventName(string text)
    {
        var tags = TagPattern().Matches(text)
            .Select(static match => match.Groups["tag"].Value)
            .Where(static tag =>
                !tag.Equals("CoopStoryBridge", StringComparison.OrdinalIgnoreCase) &&
                !tag.Equals("INFO", StringComparison.OrdinalIgnoreCase) &&
                !tag.Equals("WARN", StringComparison.OrdinalIgnoreCase) &&
                !tag.Equals("WARNING", StringComparison.OrdinalIgnoreCase) &&
                !tag.Equals("ERROR", StringComparison.OrdinalIgnoreCase))
            .Take(4)
            .ToArray();
        return tags.Length == 0 ? "plain-log" : string.Join('/', tags);
    }

    private static string InferSeverity(string text)
    {
        var lower = text.ToLowerInvariant();
        if (lower.Contains("[error]", StringComparison.Ordinal) ||
            lower.Contains("[fatal]", StringComparison.Ordinal))
        {
            return "error";
        }
        if (lower.Contains("[warn]", StringComparison.Ordinal) ||
            lower.Contains("[warning]", StringComparison.Ordinal))
        {
            return "warning";
        }
        return "info";
    }

    private static object BuildStatistics(IReadOnlyList<TimelineEvent> events)
    {
        var player = MetricSamples(
            events,
            "PLAYER_DIVERGENCE",
            IsDistanceMetric);
        var playerRotation = MetricSamples(
            events,
            "PLAYER_DIVERGENCE",
            IsAngleMetric);
        var playerActionFreshness = MetricSamples(
            events,
            "PLAYER_DIVERGENCE",
            IsActionFreshnessMetric);
        var entity = MetricSamples(
            events,
            "ENTITY_DIVERGENCE",
            IsDistanceMetric);
        var transport = MetricSamples(
            events,
            "TRANSPORT_GAP",
            IsTransportMetric);
        return new
        {
            playerDivergenceMeters = Statistics(player, "m"),
            playerRotationDegrees = Statistics(playerRotation, "deg"),
            playerActionFreshnessMilliseconds =
                Statistics(playerActionFreshness, "ms"),
            entityDivergenceMeters = Statistics(entity, "m"),
            transportGapMilliseconds = Statistics(transport, "ms")
        };
    }

    private static double[] MetricSamples(
        IReadOnlyList<TimelineEvent> events,
        string category,
        Func<string, bool> keyFilter) =>
        events
            .Where(item => item.Categories.Contains(category))
            .SelectMany(static item => item.Metrics)
            .Where(pair => keyFilter(pair.Key))
            .Select(static pair => Math.Abs(pair.Value))
            .Where(static value => double.IsFinite(value))
            .ToArray();

    private static object Statistics(double[] values, string unit)
    {
        Array.Sort(values);
        return new
        {
            available = values.Length > 0,
            sampleCount = values.Length,
            unit,
            min = values.Length > 0 ? values[0] : (double?)null,
            p50 = Percentile(values, 0.50),
            p95 = Percentile(values, 0.95),
            p99 = Percentile(values, 0.99),
            max = values.Length > 0 ? values[^1] : (double?)null
        };
    }

    private static double? Percentile(double[] sorted, double percentile)
    {
        if (sorted.Length == 0)
        {
            return null;
        }
        var rank = Math.Clamp(
            (int)Math.Ceiling(percentile * sorted.Length) - 1,
            0,
            sorted.Length - 1);
        return sorted[rank];
    }

    private static bool IsDistanceMetric(string key)
    {
        var lower = key.ToLowerInvariant();
        if (IsAngleMetric(key) || IsMillisecondsMetric(key))
        {
            return false;
        }
        return lower.Contains("meter", StringComparison.Ordinal) ||
               lower.EndsWith("-m", StringComparison.Ordinal) ||
               lower.EndsWith("_m", StringComparison.Ordinal) ||
               lower.EndsWith(".m", StringComparison.Ordinal) ||
               ContainsAny(
                   lower,
                   "position-error", "positionerror", "distance", "offset");
    }

    private static bool IsAngleMetric(string key)
    {
        var lower = key.ToLowerInvariant();
        return ContainsAny(
            lower,
            "degree", "-deg", "_deg", ".deg", "rotation-error",
            "rotationerror", "heading-error", "headingerror");
    }

    private static bool IsMillisecondsMetric(string key)
    {
        var lower = key.ToLowerInvariant();
        return lower.EndsWith("ms", StringComparison.Ordinal) ||
               lower.Contains("millisecond", StringComparison.Ordinal);
    }

    private static bool IsActionFreshnessMetric(string key)
    {
        var lower = key.ToLowerInvariant();
        return IsMillisecondsMetric(key) &&
               lower.Contains("action", StringComparison.Ordinal) &&
               lower.Contains("fresh", StringComparison.Ordinal);
    }

    private static bool IsTransportMetric(string key)
    {
        var lower = key.ToLowerInvariant();
        return lower.EndsWith("ms", StringComparison.Ordinal) &&
               ContainsAny(
                   lower,
                   "gap", "latency", "rtt", "jitter", "delay", "age",
                   "active");
    }

    private static bool IsAnomaly(TimelineEvent item)
    {
        if (item.Categories.Contains("USER_MARKER"))
        {
            return true;
        }
        if (item.EventName.Contains(
                "transport-recovered",
                StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }
        if (item.EventName.Contains(
                "transport-gap",
                StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }
        if (item.Severity is "error" or "fatal" or "warning" or "warn")
        {
            return true;
        }
        var lower = item.Message.ToLowerInvariant();
        if (item.Categories.Contains("PLAYER_DIVERGENCE") ||
            item.Categories.Contains("ENTITY_DIVERGENCE"))
        {
            var category = item.Categories.Contains("PLAYER_DIVERGENCE")
                ? "PLAYER_DIVERGENCE"
                : "ENTITY_DIVERGENCE";
            if (IsExplicitFirstDivergence(item, category) ||
                ExceedsDivergenceBudget(item, category) ||
                lower.Contains("state=recovered", StringComparison.Ordinal))
            {
                return true;
            }
            return ContainsAny(
                lower,
                "desync", "hard-snap", "hard snap", "t-pose", "tpose",
                "failed", "orphan", "mismatch");
        }
        return item.TransportBudgetExceeded || ContainsAny(
            lower,
            "desync", "diverg", "hard-snap", "hard snap", "timeout",
            "timed out", "disconnected", "connection-fault", "dropped",
            "rejected", "failed", "orphan", "mismatch", "t-pose", "tpose");
    }

    private static List<TimelineEvent> RetainAnomalies(
        List<TimelineEvent> all)
    {
        if (all.Count <= MaximumReportedAnomalies)
        {
            return all;
        }
        var pinnedMarkers = all
            .Where(static item => item.Categories.Contains("USER_MARKER"))
            .Take(100)
            .ToArray();
        var remaining = MaximumReportedAnomalies - pinnedMarkers.Length;
        var fromStart = remaining / 2;
        var fromEnd = remaining - fromStart;
        return pinnedMarkers
            .Concat(all.Take(fromStart))
            .Concat(all.TakeLast(fromEnd))
            .DistinctBy(static item => item.Ordinal)
            .OrderBy(static item => item.TimestampUtc.HasValue ? 0 : 1)
            .ThenBy(static item => item.TimestampUtc)
            .ThenBy(static item => item.Ordinal)
            .Take(MaximumReportedAnomalies)
            .ToList();
    }

    private static List<TimelineEvent> RetainTimeline(
        List<TimelineEvent> all)
    {
        const int oldestSessionEvents = 2_000;
        var pinned = all
            .Where(static item =>
                item.Categories.Contains("USER_MARKER") ||
                (item.EventName + " " + item.Message).Contains(
                    "state=first-divergence",
                    StringComparison.OrdinalIgnoreCase))
            .Take(1_000)
            .ToArray();
        var newestEvents = Math.Max(
            0,
            MaximumTimelineEvents - oldestSessionEvents - pinned.Length);
        return all.Take(oldestSessionEvents)
            .Concat(pinned)
            .Concat(all.TakeLast(newestEvents))
            .DistinctBy(static item => item.Ordinal)
            .OrderBy(static item => item.TimestampUtc.HasValue ? 0 : 1)
            .ThenBy(static item => item.TimestampUtc)
            .ThenBy(static item => item.Ordinal)
            .Take(MaximumTimelineEvents)
            .ToList();
    }

    private static TimelineEvent? FindFirstDivergence(
        IReadOnlyList<TimelineEvent> events,
        string category,
        string? latestSessionFingerprint)
    {
        var scope = latestSessionFingerprint is null
            ? events
            : events.Where(item =>
                    string.Equals(
                        item.SessionFingerprint,
                        latestSessionFingerprint,
                        StringComparison.Ordinal))
                .ToArray();
        return scope.FirstOrDefault(item =>
                   IsExplicitFirstDivergence(item, category)) ??
               scope.FirstOrDefault(item =>
                   ExceedsDivergenceBudget(item, category));
    }

    private static bool IsExplicitFirstDivergence(
        TimelineEvent item,
        string category)
    {
        if (!item.Categories.Contains(category))
        {
            return false;
        }
        var signal = (item.EventName + " " + item.Message).ToLowerInvariant();
        return signal.Contains("state=first-divergence", StringComparison.Ordinal) &&
               (category == "PLAYER_DIVERGENCE"
                   ? signal.Contains("player_divergence", StringComparison.Ordinal) ||
                     signal.Contains("player-divergence", StringComparison.Ordinal) ||
                     signal.Contains("player.divergence", StringComparison.Ordinal)
                   : signal.Contains("entity_divergence", StringComparison.Ordinal) ||
                     signal.Contains("entity-divergence", StringComparison.Ordinal) ||
                     signal.Contains("entity.divergence", StringComparison.Ordinal));
    }

    private static bool ExceedsDivergenceBudget(
        TimelineEvent item,
        string category)
    {
        if (!item.Categories.Contains(category) ||
            (item.EventName + " " + item.Message).Contains(
                "state=recovered",
                StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }
        var positionError = MaximumMetric(item, IsDistanceMetric);
        if (category == "ENTITY_DIVERGENCE")
        {
            return MetricIsPositive(item, "missing") ||
                   MetricIsPositive(item, "divergent") ||
                   positionError >= EntityPositionBudgetMeters;
        }

        var rotationError = MaximumMetric(item, IsAngleMetric);
        var actionFreshness = MaximumMetric(
            item,
            IsActionFreshnessMetric);
        var actionActive = MetricIsPositive(item, "action-active") ||
                           MetricIsPositive(item, "actionactive");
        var gaitMismatch = MetricIsPositive(item, "gait-mismatch") ||
                           MetricIsPositive(item, "gaitmismatch");
        return positionError >= PlayerPositionBudgetMeters ||
               rotationError >= PlayerRotationBudgetDegrees ||
               (gaitMismatch &&
                positionError >= PlayerGaitPositionBudgetMeters) ||
               (actionActive &&
                actionFreshness >=
                    PlayerActionFreshnessBudgetMilliseconds);
    }

    private static double MaximumMetric(
        TimelineEvent item,
        Func<string, bool> predicate) =>
        item.Metrics
            .Where(pair => predicate(pair.Key))
            .Select(static pair => Math.Abs(pair.Value))
            .DefaultIfEmpty(0.0)
            .Max();

    private static bool MetricIsPositive(
        TimelineEvent item,
        string nameFragment) =>
        item.Metrics.Any(pair =>
            pair.Key.Contains(
                nameFragment,
                StringComparison.OrdinalIgnoreCase) &&
            pair.Value > 0.0);

    private static int[] CorrelatedIds(
        IReadOnlyList<TimelineEvent> events,
        TimelineEvent anomaly)
    {
        bool SameSession(TimelineEvent item) =>
            anomaly.SessionFingerprint is null ||
            string.Equals(
                item.SessionFingerprint,
                anomaly.SessionFingerprint,
                StringComparison.Ordinal);

        if (anomaly.TimestampUtc is null)
        {
            var index = 0;
            while (index < events.Count &&
                   !ReferenceEquals(events[index], anomaly))
            {
                index++;
            }
            return events
                .Skip(Math.Max(0, index - 2))
                .Take(5)
                .Where(SameSession)
                .Select(static item => item.Id)
                .ToArray();
        }
        return events
            .Where(item =>
                SameSession(item) &&
                item.TimestampUtc is not null &&
                Math.Abs(
                    (item.TimestampUtc.Value - anomaly.TimestampUtc.Value)
                    .TotalMilliseconds) <= CorrelationWindowMilliseconds)
            .Select(static item => item.Id)
            .ToArray();
    }

    private static MarkerWindowView[] BuildMarkerWindows(
        IReadOnlyList<TimelineEvent> events)
    {
        var markers = events
            .Where(static item => item.Categories.Contains("USER_MARKER"))
            .ToArray();
        return markers
            .GroupBy(MarkerCorrelationKey, StringComparer.Ordinal)
            .Select(group =>
            {
                var markerEvents = group
                    .OrderBy(static item => item.TimestampUtc.HasValue ? 0 : 1)
                    .ThenBy(static item => item.TimestampUtc)
                    .ThenBy(static item => item.Ordinal)
                    .ToArray();
                var anchor = markerEvents[0];
                var context = MarkerContext(events, anchor)
                    .ToArray();
                return new MarkerWindowView(
                    group.Key,
                    anchor,
                    markerEvents,
                    context);
            })
            .OrderBy(static window =>
                window.Anchor.TimestampUtc.HasValue ? 0 : 1)
            .ThenBy(static window => window.Anchor.TimestampUtc)
            .ThenBy(static window => window.Anchor.Ordinal)
            .ToArray();
    }

    private static IEnumerable<TimelineEvent> MarkerContext(
        IReadOnlyList<TimelineEvent> events,
        TimelineEvent anchor)
    {
        bool SameSession(TimelineEvent item) =>
            anchor.SessionFingerprint is null ||
            string.Equals(
                item.SessionFingerprint,
                anchor.SessionFingerprint,
                StringComparison.Ordinal);

        if (anchor.TimestampUtc is null)
        {
            return events
                .Where(item =>
                    SameSession(item) &&
                    Math.Abs(item.Ordinal - anchor.Ordinal) <= 50)
                .OrderBy(static item => item.Ordinal);
        }
        var start = anchor.TimestampUtc.Value.AddMilliseconds(
            -MarkerWindowBeforeMilliseconds);
        var end = anchor.TimestampUtc.Value.AddMilliseconds(
            MarkerWindowAfterMilliseconds);
        return events
            .Where(item =>
                SameSession(item) &&
                item.TimestampUtc.HasValue &&
                item.TimestampUtc.Value >= start &&
                item.TimestampUtc.Value <= end)
            .OrderBy(static item => item.TimestampUtc)
            .ThenBy(static item => item.Ordinal);
    }

    private static string MarkerCorrelationKey(TimelineEvent marker)
    {
        foreach (var metric in marker.Metrics)
        {
            if ((metric.Key.EndsWith(
                     "markerCorrelationId",
                     StringComparison.OrdinalIgnoreCase) ||
                 metric.Key.Equals(
                     "correlation",
                     StringComparison.OrdinalIgnoreCase)) &&
                metric.Value > 0 &&
                metric.Value <= 9_007_199_254_740_991D)
            {
                return Math.Round(metric.Value)
                    .ToString("0", CultureInfo.InvariantCulture);
            }
        }
        return $"legacy-{marker.Id}";
    }

    private static string BuildMarkerWindowsMarkdown(
        IReadOnlyList<MarkerWindowView> windows,
        LauncherRole localRole,
        DateTimeOffset generatedAtUtc)
    {
        var output = new StringBuilder();
        output.AppendLine("# RDR2 Coop Story - F7 marker windows");
        output.AppendLine();
        output.AppendLine(
            $"Export: **{localRole}**, UTC `{generatedAtUtc:o}`.");
        output.AppendLine(
            $"Each marker shows context from {MarkerWindowBeforeMilliseconds / 1_000} s before to {MarkerWindowAfterMilliseconds / 1_000} s after pressing F7.");
        output.AppendLine();
        if (windows.Count == 0)
        {
            output.AppendLine("No F7 markers were found in the preserved logs.");
            return output.ToString();
        }
        foreach (var window in windows)
        {
            output.AppendLine(
                $"## Marker `{window.CorrelationId}` - {window.Anchor.TimestampUtc?.ToString("O") ?? "no timestamp"}");
            output.AppendLine();
            output.AppendLine(
                $"Session: `{window.Anchor.SessionFingerprint ?? "legacy"}`, marker events: {string.Join(", ", window.MarkerEvents.Select(static item => $"#{item.Id}"))}.");
            output.AppendLine();
            output.AppendLine("| UTC | Role | Category | Source | Event |");
            output.AppendLine("|---|---|---|---|---|");
            foreach (var item in window.ContextEvents)
            {
                output.Append('|')
                    .Append(item.TimestampUtc?.ToString("O") ?? "no timestamp")
                    .Append('|').Append(EscapeMarkdown(item.Role))
                    .Append('|').Append(EscapeMarkdown(
                        string.Join(", ", item.Categories)))
                    .Append('|').Append(EscapeMarkdown(item.Source))
                    .Append('|').Append(EscapeMarkdown(
                        $"#{item.Id} {item.EventName}: {item.Message}"))
                    .AppendLine("|");
            }
            output.AppendLine();
        }
        return output.ToString();
    }

    private static object ToJsonEvent(TimelineEvent item) => new
    {
        id = item.Id,
        timestampUtc = item.TimestampUtc,
        role = item.Role,
        source = item.Source,
        sessionFingerprint = item.SessionFingerprint,
        severity = item.Severity,
        eventName = item.EventName,
        categories = item.Categories,
        message = item.Message,
        metrics = item.Metrics,
        transportBudgetExceeded = item.TransportBudgetExceeded
    };

    private static object? ToNullableJsonEvent(TimelineEvent? item) =>
        item is null ? null : ToJsonEvent(item);

    private static string BuildTimelineMarkdown(
        IReadOnlyList<TimelineEvent> events,
        LauncherRole localRole,
        int totalRelevantEvents,
        string? latestSessionFingerprint,
        DateTimeOffset generatedAtUtc)
    {
        var output = new StringBuilder();
        output.AppendLine("# RDR2 Coop Story - diagnostics timeline");
        output.AppendLine();
        output.AppendLine($"- Generated UTC: `{generatedAtUtc:o}`");
        output.AppendLine($"- Exporting side: **{localRole}**");
        output.AppendLine(
            $"- Latest session: `{latestSessionFingerprint ?? "legacy/none"}`.");
        output.AppendLine(
            $"- Events: **{events.Count}** retained out of **{totalRelevantEvents}** matching events.");
        output.AppendLine(
            "- Role identifies the side that logged an event; an explicit `role=Host/Guest` takes precedence.");
        output.AppendLine();
        output.AppendLine("| UTC | Session | Role | Category | Source | Event |");
        output.AppendLine("|---|---|---|---|---|---|");
        foreach (var item in events)
        {
            output.Append('|')
                .Append(item.TimestampUtc?.ToString("O") ?? "no timestamp")
                .Append('|').Append(item.SessionFingerprint ?? "legacy")
                .Append('|').Append(EscapeMarkdown(item.Role))
                .Append('|').Append(EscapeMarkdown(string.Join(", ", item.Categories)))
                .Append('|').Append(EscapeMarkdown(item.Source))
                .Append('|').Append(EscapeMarkdown(
                    $"#{item.Id} {item.EventName}: {item.Message}"))
                .AppendLine("|");
        }
        if (events.Count == 0)
        {
            output.AppendLine("| - | - | - | - | - | No matching events. |");
        }
        return output.ToString();
    }

    private static string BuildAnomaliesMarkdown(
        IReadOnlyList<AnomalyView> anomalies,
        IReadOnlyList<TimelineEvent> timeline,
        LauncherRole localRole,
        TimelineEvent? firstDivergence,
        TimelineEvent? firstPlayer,
        TimelineEvent? firstEntity,
        object statistics,
        int totalDetectedAnomalies,
        string? latestSessionFingerprint,
        DateTimeOffset generatedAtUtc)
    {
        var output = new StringBuilder();
        output.AppendLine("# RDR2 Coop Story - anomalies and correlation");
        output.AppendLine();
        output.AppendLine($"Export: **{localRole}**, UTC `{generatedAtUtc:o}`.");
        output.AppendLine(
            $"Latest session: `{latestSessionFingerprint ?? "legacy/none"}`.");
        output.AppendLine(
            $"Detected **{totalDetectedAnomalies}** anomalies; the report retained **{anomalies.Count}**.");
        output.AppendLine();
        AppendFirst("First divergence", firstDivergence);
        AppendFirst("First player divergence", firstPlayer);
        AppendFirst("First entity divergence", firstEntity);
        output.AppendLine();
        output.AppendLine("## Statistics (when the log contained numbers)");
        output.AppendLine();
        using (var document = JsonDocument.Parse(
                   JsonSerializer.Serialize(statistics, JsonSupport.Options)))
        {
            output.AppendLine("| Metric | Samples | P50 | P95 | P99 | Max |");
            output.AppendLine("|---|---:|---:|---:|---:|---:|");
            foreach (var property in document.RootElement.EnumerateObject())
            {
                var value = property.Value;
                output.Append('|').Append(EscapeMarkdown(property.Name))
                    .Append('|').Append(value.GetProperty("sampleCount").GetInt32())
                    .Append('|').Append(FormatNumber(value.GetProperty("p50")))
                    .Append('|').Append(FormatNumber(value.GetProperty("p95")))
                    .Append('|').Append(FormatNumber(value.GetProperty("p99")))
                    .Append('|').Append(FormatNumber(value.GetProperty("max")))
                    .Append(' ').Append(value.GetProperty("unit").GetString())
                    .AppendLine("|");
            }
        }
        output.AppendLine();
        output.AppendLine("## Anomalies in chronological order");
        output.AppendLine();
        output.AppendLine("| UTC | Session | Role | Type | Event | Correlation (timeline ID) |");
        output.AppendLine("|---|---|---|---|---|---|");
        foreach (var view in anomalies)
        {
            var item = view.Event;
            output.Append('|').Append(item.TimestampUtc?.ToString("O") ?? "no timestamp")
                .Append('|').Append(item.SessionFingerprint ?? "legacy")
                .Append('|').Append(EscapeMarkdown(item.Role))
                .Append('|').Append(EscapeMarkdown(PrimaryCategory(item)))
                .Append('|').Append(EscapeMarkdown($"#{item.Id} {item.EventName}: {item.Message}"))
                .Append('|').Append(string.Join(", ", view.CorrelatedEventIds.Select(
                    static id => $"#{id}")))
                .AppendLine("|");
        }
        if (anomalies.Count == 0)
        {
            output.AppendLine("| - | - | - | - | No anomalies detected. | - |");
        }
        output.AppendLine();
        output.AppendLine(
            $"Correlation includes events from both roles within +/- {CorrelationWindowMilliseconds} ms. Full context is in `TIMELINE.json` ({timeline.Count} events).");
        return output.ToString();

        void AppendFirst(string label, TimelineEvent? item)
        {
            output.Append("- **").Append(label).Append(":** ");
            if (item is null)
            {
                output.AppendLine("no data.");
                return;
            }
            output.Append('`').Append(item.TimestampUtc?.ToString("O") ?? "no timestamp")
                .Append("`, session `")
                .Append(item.SessionFingerprint ?? "legacy")
                .Append("`, ").Append(item.Role).Append(", #")
                .Append(item.Id).Append(' ').Append(item.EventName)
                .AppendLine();
        }
    }

    private static string FormatNumber(JsonElement element) =>
        element.ValueKind == JsonValueKind.Number
            ? element.GetDouble().ToString("0.###", CultureInfo.InvariantCulture)
            : "n/a";

    private static string PrimaryCategory(TimelineEvent item) =>
        item.Categories.FirstOrDefault(static category =>
            category is "USER_MARKER" or "PLAYER_DIVERGENCE" or "ENTITY_DIVERGENCE" or
                "TRANSPORT_GAP" or "MISSION_ISOLATION" or
                "MISSION_ANCHOR" or "ANIMGRAPH_TRAVERSAL" or
                "COMBAT_LIFECYCLE" or
                "LASSO_LIFECYCLE" or "MOUNT_LIFECYCLE" or
                "MISSION_TIMELINE" or "SESSION_HEALTH") ?? "OTHER";

    private static string EscapeMarkdown(string value) =>
        value.Replace("|", "\\|", StringComparison.Ordinal)
            .Replace("\r", " ", StringComparison.Ordinal)
            .Replace("\n", " ", StringComparison.Ordinal);

    private static string Truncate(string value, int maximum) =>
        value.Length <= maximum ? value : value[..maximum] + "...";

    private static bool HasTag(string lower, string stem) =>
        lower.Contains($"[{stem}", StringComparison.Ordinal) ||
        lower.Contains(stem.Replace('_', '-'), StringComparison.Ordinal) ||
        lower.Contains(stem.Replace('_', '.'), StringComparison.Ordinal) ||
        lower.Contains(stem, StringComparison.Ordinal);

    private static bool ContainsAny(string value, params string[] needles) =>
        needles.Any(needle => value.Contains(needle, StringComparison.Ordinal));

    private sealed class TimelineEvent(
        int ordinal,
        DateTimeOffset? timestampUtc,
        string source,
        string role,
        string severity,
        string eventName,
        string message,
        string[] categories,
        Dictionary<string, double> metrics,
        bool transportBudgetExceeded,
        string? sessionFingerprint)
    {
        public int Id { get; set; }
        public int Ordinal { get; } = ordinal;
        public DateTimeOffset? TimestampUtc { get; } = timestampUtc;
        public string Source { get; } = source;
        public string? SessionFingerprint { get; set; } = sessionFingerprint;
        public string Role { get; } = role;
        public string Severity { get; } = severity;
        public string EventName { get; } = eventName;
        public string Message { get; } = message;
        public string[] Categories { get; } = categories;
        public Dictionary<string, double> Metrics { get; } = metrics;
        public bool TransportBudgetExceeded { get; } =
            transportBudgetExceeded;
    }

    private sealed record AnomalyView(
        TimelineEvent Event,
        int[] CorrelatedEventIds);

    private sealed record MarkerWindowView(
        string CorrelationId,
        TimelineEvent Anchor,
        TimelineEvent[] MarkerEvents,
        TimelineEvent[] ContextEvents);
}
