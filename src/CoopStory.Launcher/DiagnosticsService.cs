using System.IO.Compression;
using System.Net;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace CoopStory.Launcher;

public sealed class DiagnosticsService(
    LauncherPaths paths,
    LauncherPolicy policy,
    LauncherLogger logger)
{
    private const long MaxLogBytes = 32L * 1024 * 1024;
    private const int MaximumIndexedLinesPerCategory = 300;
    private const int MaximumExtractedLines = 120;
    private const int MaximumDiagnosticSessions = 2;
    private static readonly Regex NetworkAddressCandidatePattern = new(
        @"(?<![A-Za-z0-9])(?:\d{1,3}(?:\.\d{1,3}){3}|\[[0-9A-Fa-f:.]+\]|(?:[0-9A-Fa-f]{0,4}:){2,7}[0-9A-Fa-f:.]{0,39})(?![A-Za-z0-9])",
        RegexOptions.CultureInvariant,
        TimeSpan.FromSeconds(1));

    public string Export(
        string destination,
        LauncherSettings settings,
        PackageLayout? package)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(destination);
        var target = Path.GetFullPath(destination);
        var parent = Path.GetDirectoryName(target)
            ?? throw new LauncherException("Ścieżka diagnostyki nie ma katalogu.");
        Directory.CreateDirectory(parent);
        var temporaryTarget = Path.Combine(
            parent,
            $".{Path.GetFileName(target)}.{Guid.NewGuid():N}.tmp");

        var gameRoot = Path.GetDirectoryName(settings.GameExePath);
        var exactSecrets = new List<string?>
        {
            settings.SessionToken,
            settings.HostAddress,
            Environment.MachineName,
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            gameRoot,
            package?.Root,
            paths.StateDirectory,
            settings.DiagnosticsExportFolder
        };
        exactSecrets.AddRange(DiscoverNetworkAddresses(gameRoot));
        var redactor = new SecretRedactor(exactSecrets);
        var observedRole = ResolveObservedRole(settings.Role);
        var exportWindow = CreateDiagnosticExportWindow();
        var diagnosticSettings = settings with
        {
            Role = observedRole.Role
        };

        try
        {
            using (var archive = ZipFile.Open(
                       temporaryTarget,
                       ZipArchiveMode.Create))
            {
                var diagnosticIndex = CreateDiagnosticIndex(
                    observedRole.Role,
                    exportWindow);
                AddText(
                    archive,
                    "summary.json",
                    redactor.Redact(JsonSerializer.Serialize(
                        CreateSummary(
                            diagnosticSettings,
                            package,
                            observedRole.Source),
                        JsonSupport.Options)));
                AddText(
                    archive,
                    "settings-redacted.json",
                    redactor.Redact(
                        SidecarConfiguration.RedactedJson(diagnosticSettings)));
                AddText(
                    archive,
                    "DIAGNOSTICS_INDEX.txt",
                    redactor.Redact(diagnosticIndex.Text));
                AddText(
                    archive,
                    "DIAGNOSTICS_SUMMARY.json",
                    redactor.Redact(diagnosticIndex.SummaryJson));
                AddText(
                    archive,
                    "DIAGNOSTICS_ERRORS.txt",
                    redactor.Redact(diagnosticIndex.ErrorsText));
                AddText(
                    archive,
                    "DIAGNOSTICS_WARNINGS.txt",
                    redactor.Redact(diagnosticIndex.WarningsText));
                AddText(
                    archive,
                    "DIAGNOSTICS_RUNTIME_SUMMARIES.txt",
                    redactor.Redact(diagnosticIndex.RuntimeSummariesText));
                AddText(
                    archive,
                    "TIMELINE.md",
                    redactor.Redact(diagnosticIndex.Analysis.TimelineMarkdown));
                AddText(
                    archive,
                    "TIMELINE.json",
                    redactor.Redact(diagnosticIndex.Analysis.TimelineJson));
                AddText(
                    archive,
                    "ANOMALIES.md",
                    redactor.Redact(diagnosticIndex.Analysis.AnomaliesMarkdown));
                AddText(
                    archive,
                    "ANOMALIES.json",
                    redactor.Redact(diagnosticIndex.Analysis.AnomaliesJson));
                AddText(
                    archive,
                    "MARKER_WINDOWS.md",
                    redactor.Redact(
                        diagnosticIndex.Analysis.MarkerWindowsMarkdown));
                AddText(
                    archive,
                    "MARKER_WINDOWS.json",
                    redactor.Redact(
                        diagnosticIndex.Analysis.MarkerWindowsJson));
                AddText(
                    archive,
                    "DIAGNOSTICS_SESSIONS.json",
                    JsonSerializer.Serialize(
                        new
                        {
                            maximumSessions = MaximumDiagnosticSessions,
                            cutoffUtc = exportWindow.CutoffUtc,
                            sessionStartsUtc = exportWindow.SessionStartsUtc
                        },
                        JsonSupport.Options));

                AddFilteredDiagnosticLogs(archive, exportWindow, redactor);
                AddRedactedFileTail(
                    archive,
                    paths.InstallManifestPath,
                    "install-manifest.json",
                    redactor);
                AddRedactedFileTail(
                    archive,
                    Path.Combine(
                        paths.StateDirectory,
                        "recordings",
                        "ghost-last.json"),
                    "recordings/ghost-last.json",
                    redactor);

                if (!string.IsNullOrWhiteSpace(gameRoot))
                {
                    AddRedactedFileTail(
                        archive,
                        Path.Combine(gameRoot, "ScriptHookRDR2.log"),
                        "logs/ScriptHookRDR2.log",
                        redactor);
                }
            }

            if (File.Exists(target))
            {
                File.Replace(
                    temporaryTarget,
                    target,
                    destinationBackupFileName: null,
                    ignoreMetadataErrors: true);
            }
            else
            {
                File.Move(temporaryTarget, target);
            }
        }
        finally
        {
            if (File.Exists(temporaryTarget))
            {
                File.Delete(temporaryTarget);
            }
        }

        logger.Info(
            "diagnostics.exported",
            $"Wyeksportowano diagnostykę do {target}; sekret uwierzytelniania został zredagowany.");
        return target;
    }

    private DiagnosticIndexReport CreateDiagnosticIndex(
        LauncherRole localRole,
        DiagnosticExportWindow exportWindow)
    {
        var sources = DiagnosticSources();
        var groups = new Dictionary<string, List<string>>(
            StringComparer.Ordinal)
        {
            ["ERRORS"] = [],
            ["WARNINGS"] = [],
            ["NATIVE_CRASH"] = [],
            ["RUNTIME_SUMMARIES"] = [],
            ["ACTION_TX"] = [],
            ["ACTION_FSM"] = [],
            ["ACTION_APPLY"] = [],
            ["ACTION_EPOCH"] = [],
            ["MELEE_VISUAL"] = [],
            ["PEER_DISMOUNT"] = [],
            ["PEER_COMBAT"] = [],
            ["AIM_POSE"] = [],
            ["LASSO_ROPE"] = [],
            ["VICTIM_CONSTRAINT"] = [],
            ["MISSION_FSM"] = [],
            ["MISSION_TX"] = [],
            ["MISSION_RX"] = [],
            ["MISSION_WORLD"] = [],
            ["MISSION_PREFLIGHT"] = [],
            ["MISSION_SPECTATOR"] = [],
            ["MISSION_DAMAGE"] = [],
            ["MISSION_CHECKPOINT"] = [],
            ["MISSION_CAMERA_TX"] = [],
            ["MISSION_CAMERA_RX"] = [],
            ["MISSION_CAMERA_FALLBACK"] = [],
            ["MISSION_OBJECTIVE"] = [],
            ["MISSION_SKIP"] = [],
            ["ANIMSCENE_HYBRID"] = [],
            ["ANIMSCENE_REPLICA"] = [],
            ["MOUNT_TX"] = [],
            ["MOUNT_REGISTRY"] = [],
            ["REMOTE_MOUNT"] = [],
            ["WORLD_POOL"] = [],
            ["WORLD_PROXY_PHYSICS"] = [],
            ["ENTITY_GRAPH"] = [],
            ["PUPPET_PLAYER"] = [],
            ["PLAYER_ACTIONS"] = [],
            ["ANIMGRAPH_REPLICA"] = [],
            ["ANIMGRAPH_SAMPLE"] = [],
            ["NETWORK_MOTION_MODE"] = [],
            ["NETWORK"] = [],
            ["WORLD_NPC_HORSE"] = [],
            ["SOLO_TEST"] = [],
            ["GHOST_RECORD_REPLAY"] = [],
            ["SESSION_HEALTH"] = [],
            ["MISSION_TIMELINE"] = [],
            ["MISSION_ISOLATION"] = [],
            ["MISSION_ANCHOR"] = [],
            ["ANIMGRAPH_TRAVERSAL"] = [],
            ["PLAYER_DIVERGENCE"] = [],
            ["ENTITY_DIVERGENCE"] = [],
            ["COMBAT_LIFECYCLE"] = [],
            ["LASSO_LIFECYCLE"] = [],
            ["MOUNT_LIFECYCLE"] = [],
            ["TRANSPORT_GAP"] = [],
            ["USER_MARKER"] = [],
            ["PROBLEM_SNAPSHOT"] = []
        };
        var matchCounts = groups.Keys.ToDictionary(
            static category => category,
            static _ => 0,
            StringComparer.Ordinal);
        var sourceSummaries = new List<DiagnosticSourceSummary>();
        var diagnosticLines = new List<DiagnosticLogLine>();
        var diagnosticOrdinal = 0;

        foreach (var source in sources)
        {
            if (!File.Exists(source.Path) ||
                PackageLocator.IsReparsePoint(source.Path))
            {
                sourceSummaries.Add(new DiagnosticSourceSummary(
                    source.Name,
                    Available: false,
                    ScannedLines: 0));
                continue;
            }

            string tail;
            try
            {
                tail = ReadDiagnosticSource(source.Path, exportWindow);
            }
            catch (IOException)
            {
                sourceSummaries.Add(new DiagnosticSourceSummary(
                    source.Name,
                    Available: false,
                    ScannedLines: 0));
                continue;
            }
            catch (UnauthorizedAccessException)
            {
                sourceSummaries.Add(new DiagnosticSourceSummary(
                    source.Name,
                    Available: false,
                    ScannedLines: 0));
                continue;
            }

            var sourceLines = tail.Split(
                ['\r', '\n'],
                StringSplitOptions.RemoveEmptyEntries);
            sourceSummaries.Add(new DiagnosticSourceSummary(
                source.Name,
                Available: true,
                ScannedLines: sourceLines.Length));

            foreach (var rawLine in sourceLines)
            {
                var line = rawLine.Trim();
                var lower = line.ToLowerInvariant();
                diagnosticLines.Add(new DiagnosticLogLine(
                    source.Name,
                    line,
                    ++diagnosticOrdinal));
                AddWhen(
                    "ERRORS",
                    IsErrorLine(lower));
                AddWhen(
                    "WARNINGS",
                    IsWarningLine(lower));
                AddWhen(
                    "NATIVE_CRASH",
                    lower.Contains("[seh]", StringComparison.Ordinal) ||
                    lower.Contains("script hook rdr2 error", StringComparison.Ordinal) ||
                    lower.Contains("exception occurred while executing", StringComparison.Ordinal));
                AddWhen(
                    "RUNTIME_SUMMARIES",
                    IsRuntimeSummaryLine(lower));
                AddWhen("ACTION_TX", ContainsDiagnosticTag(lower, "action_tx"));
                AddWhen("ACTION_FSM", ContainsDiagnosticTag(lower, "action_fsm"));
                AddWhen(
                    "ACTION_APPLY",
                    ContainsDiagnosticTag(lower, "action_apply"));
                AddWhen(
                    "ACTION_EPOCH",
                    ContainsDiagnosticTag(lower, "action_epoch"));
                AddWhen(
                    "MELEE_VISUAL",
                    ContainsDiagnosticTag(lower, "melee_visual"));
                AddWhen(
                    "PEER_DISMOUNT",
                    ContainsDiagnosticTag(lower, "peer_dismount"));
                AddWhen(
                    "PEER_COMBAT",
                    ContainsDiagnosticTag(lower, "peer_combat"));
                AddWhen("AIM_POSE", ContainsDiagnosticTag(lower, "aim_pose"));
                AddWhen(
                    "LASSO_ROPE",
                    ContainsDiagnosticTag(lower, "lasso_rope"));
                AddWhen(
                    "VICTIM_CONSTRAINT",
                    ContainsDiagnosticTag(lower, "victim_constraint"));
                AddWhen(
                    "MISSION_FSM",
                    ContainsDiagnosticTag(lower, "mission_fsm"));
                AddWhen(
                    "MISSION_TX",
                    ContainsDiagnosticTag(lower, "mission_tx"));
                AddWhen(
                    "MISSION_RX",
                    ContainsDiagnosticTag(lower, "mission_rx"));
                AddWhen(
                    "MISSION_WORLD",
                    ContainsDiagnosticTag(lower, "mission_world"));
                AddWhen(
                    "MISSION_PREFLIGHT",
                    ContainsDiagnosticTag(lower, "mission_preflight"));
                AddWhen(
                    "MISSION_SPECTATOR",
                    ContainsDiagnosticTag(lower, "mission_spectator"));
                AddWhen(
                    "MISSION_DAMAGE",
                    ContainsDiagnosticTag(lower, "mission_damage"));
                AddWhen(
                    "MISSION_CHECKPOINT",
                    ContainsDiagnosticTag(lower, "mission_checkpoint"));
                var missionCameraLine =
                    ContainsDiagnosticTag(lower, "mission_camera");
                AddWhen(
                    "MISSION_CAMERA_TX",
                    missionCameraLine && lower.Contains("[tx]", StringComparison.Ordinal));
                AddWhen(
                    "MISSION_CAMERA_RX",
                    missionCameraLine && lower.Contains("[rx]", StringComparison.Ordinal));
                AddWhen(
                    "MISSION_CAMERA_FALLBACK",
                    missionCameraLine && lower.Contains("[fallback]", StringComparison.Ordinal));
                AddWhen(
                    "MISSION_OBJECTIVE",
                    ContainsDiagnosticTag(lower, "mission_objective"));
                AddWhen(
                    "MISSION_SKIP",
                    ContainsDiagnosticTag(lower, "mission_skip"));
                AddWhen(
                    "ANIMSCENE_HYBRID",
                    ContainsDiagnosticTag(lower, "animscene_hybrid"));
                AddWhen(
                    "ANIMSCENE_REPLICA",
                    ContainsDiagnosticTag(lower, "animscene_replica"));
                AddWhen(
                    "MOUNT_TX",
                    ContainsDiagnosticTag(lower, "mount_tx") ||
                    ContainsDiagnosticTag(lower, "mount_local"));
                AddWhen(
                    "MOUNT_REGISTRY",
                    ContainsDiagnosticTag(lower, "mount_registry") ||
                    ContainsDiagnosticTag(lower, "remote_mount_rx") ||
                    ContainsDiagnosticTag(lower, "remote_mount_relation"));
                AddWhen(
                    "REMOTE_MOUNT",
                    ContainsDiagnosticTag(lower, "remote_mount"));
                AddWhen("WORLD_POOL", ContainsDiagnosticTag(lower, "world_pool"));
                AddWhen(
                    "WORLD_PROXY_PHYSICS",
                    ContainsDiagnosticTag(lower, "world_proxy_physics"));
                AddWhen(
                    "ENTITY_GRAPH",
                    ContainsDiagnosticTag(lower, "entity_graph") ||
                    ContainsDiagnosticTag(lower, "entitygraph"));
                AddWhen(
                    "PUPPET_PLAYER",
                    lower.Contains("puppet") ||
                    lower.Contains("motion") ||
                    lower.Contains("replica") ||
                    lower.Contains("player_identity") ||
                    lower.Contains("nickname"));
                AddWhen(
                    "PLAYER_ACTIONS",
                    lower.Contains("puppet_action") ||
                    lower.Contains("local_test_action") ||
                    lower.Contains("local-test.action") ||
                    lower.Contains("aim-target") ||
                    lower.Contains("fire-events") ||
                    lower.Contains("visual-zero-damage-shots") ||
                    lower.Contains("reloading"));
                AddWhen(
                    "ANIMGRAPH_REPLICA",
                    lower.Contains("[animgraph_replica]") ||
                    lower.Contains("animgraph replica") ||
                    lower.Contains("animgraph_replica") ||
                    lower.Contains("animgraph-replica") ||
                    lower.Contains("animgraph-direct"));
                AddWhen(
                    "ANIMGRAPH_SAMPLE",
                    lower.Contains("[animgraph_sample]") ||
                    lower.Contains("animgraph sample") ||
                    lower.Contains("animgraph_sample") ||
                    lower.Contains("playeranimationstate") ||
                    lower.Contains("player_animation_state"));
                AddWhen(
                    "NETWORK_MOTION_MODE",
                    lower.Contains("network.motion-mode") ||
                    lower.Contains("motionreplicationmode") ||
                    lower.Contains("motion replication configured") ||
                    lower.Contains("peermodenegotiated"));
                AddWhen(
                    "NETWORK",
                    lower.Contains("[network", StringComparison.Ordinal) ||
                    lower.Contains("\"event\":\"network.", StringComparison.Ordinal) ||
                    lower.Contains("\"eventname\":\"network.", StringComparison.Ordinal) ||
                    lower.Contains("network.", StringComparison.Ordinal) ||
                    lower.Contains("network-", StringComparison.Ordinal) ||
                    lower.Contains("network_", StringComparison.Ordinal) ||
                    lower.Contains("diagnostics.transport-", StringComparison.Ordinal) ||
                    lower.Contains("diagnostics.streaming", StringComparison.Ordinal) ||
                    lower.Contains("handshake") ||
                    lower.Contains("udp") ||
                    lower.Contains("tcp"));
                AddWhen(
                    "WORLD_NPC_HORSE",
                    lower.Contains("world mirror") ||
                    lower.Contains("world-mirror") ||
                    lower.Contains("entity_graph") ||
                    lower.Contains("entity-graph") ||
                    lower.Contains("entitygraph") ||
                    lower.Contains("worldentity") ||
                    lower.Contains("mount") ||
                    lower.Contains("horse") ||
                    lower.Contains("npc"));
                AddWhen(
                    "SOLO_TEST",
                    lower.Contains("solo_test") ||
                    lower.Contains("solo-test") ||
                    lower.Contains("local_test") ||
                    lower.Contains("local-test") ||
                    lower.Contains("solo-marker"));
                AddWhen(
                    "GHOST_RECORD_REPLAY",
                    lower.Contains("ghost-record") ||
                    lower.Contains("ghost-replay") ||
                    lower.Contains("ghost_record") ||
                    lower.Contains("ghost_replay"));
                foreach (var category in
                         DiagnosticsTimelineAnalyzer.Classify(lower))
                {
                    AddWhen(category, condition: true);
                }

                void AddWhen(string category, bool condition)
                {
                    if (!condition)
                    {
                        return;
                    }

                    matchCounts[category]++;
                    // Keep the newest matches. The previous cap kept the first
                    // 300 lines from a 32 MB tail, so a fresh Ghost Replay was
                    // absent from the sorted index even though the full log was
                    // correctly present in the ZIP.
                    if (groups[category].Count >=
                        MaximumIndexedLinesPerCategory)
                    {
                        groups[category].RemoveAt(0);
                    }
                    groups[category].Add($"[{source.Name}] {line}");
                }
            }
        }

        var analysis = DiagnosticsTimelineAnalyzer.Analyze(
            diagnosticLines,
            localRole);

        var output = new StringBuilder();
        output.AppendLine("RDR2 COOP STORY - POSORTOWANY INDEKS DIAGNOSTYKI");
        output.AppendLine($"Wygenerowano UTC: {DateTimeOffset.UtcNow:o}");
        output.AppendLine(
            "To indeks pomocniczy. Pelne, zredagowane logi sa w katalogu logs/ tego ZIP-a.");
        foreach (var group in groups)
        {
            output.AppendLine();
            output.AppendLine(
                $"===== {group.Key} (matches={matchCounts[group.Key]}, newest={group.Value.Count}) =====");
            if (group.Value.Count == 0)
            {
                output.AppendLine("(brak dopasowanych wpisow)");
                continue;
            }
            foreach (var line in group.Value)
            {
                output.AppendLine(line);
            }
        }

        var generatedAtUtc = DateTimeOffset.UtcNow;
        var summaryJson = JsonSerializer.Serialize(
            new
            {
                generatedAtUtc,
                sourceFiles = sourceSummaries,
                categories = groups.ToDictionary(
                    static group => group.Key,
                    group => new
                    {
                        matched = matchCounts[group.Key],
                        retained = group.Value.Count
                    },
                    StringComparer.Ordinal),
                latestErrors = Newest(groups["ERRORS"]),
                latestWarnings = Newest(groups["WARNINGS"]),
                latestRuntimeSummaries = Newest(groups["RUNTIME_SUMMARIES"])
            },
            JsonSupport.Options);

        return new DiagnosticIndexReport(
            output.ToString(),
            summaryJson,
            CreateExtractedText(
                "RDR2 COOP STORY - NAJNOWSZE BLEDY",
                groups["ERRORS"]),
            CreateExtractedText(
                "RDR2 COOP STORY - NAJNOWSZE OSTRZEZENIA",
                groups["WARNINGS"]),
            CreateExtractedText(
                "RDR2 COOP STORY - NAJNOWSZE PODSUMOWANIA RUNTIME",
                groups["RUNTIME_SUMMARIES"]),
            analysis);
    }

    private static string[] Newest(List<string> lines) =>
        lines.TakeLast(MaximumExtractedLines).ToArray();

    private static string CreateExtractedText(
        string heading,
        List<string> lines)
    {
        var output = new StringBuilder();
        output.AppendLine(heading);
        output.AppendLine(
            $"Pokazano {Math.Min(lines.Count, MaximumExtractedLines)} najnowszych wpisow.");
        var selected = Newest(lines);
        if (selected.Length == 0)
        {
            output.AppendLine("(brak dopasowanych wpisow)");
        }
        else
        {
            foreach (var line in selected)
            {
                output.AppendLine(line);
            }
        }
        return output.ToString();
    }

    private static bool ContainsDiagnosticTag(string lower, string tag)
    {
        var underscored = tag.ToLowerInvariant();
        return lower.Contains(underscored, StringComparison.Ordinal) ||
               lower.Contains(
                   underscored.Replace('_', '-'),
                   StringComparison.Ordinal) ||
               lower.Contains(
                   underscored.Replace('_', '.'),
                   StringComparison.Ordinal) ||
               lower.Contains(
                   underscored.Replace('_', ' '),
                   StringComparison.Ordinal);
    }

    private static bool IsErrorLine(string lower) =>
        lower.Contains("\"level\":\"error\"", StringComparison.Ordinal) ||
        lower.Contains("\"level\":\"fatal\"", StringComparison.Ordinal) ||
        lower.Contains("\"severity\":\"error\"", StringComparison.Ordinal) ||
        lower.Contains("[error]", StringComparison.Ordinal) ||
        lower.Contains("[fatal]", StringComparison.Ordinal) ||
        lower.Contains("exception", StringComparison.Ordinal) ||
        lower.Contains("could not", StringComparison.Ordinal) ||
        lower.Contains("access denied", StringComparison.Ordinal) ||
        lower.Contains(" crashed", StringComparison.Ordinal) ||
        (lower.Contains(" failed", StringComparison.Ordinal) &&
         !lower.Contains("failed=0", StringComparison.Ordinal));

    private static bool IsWarningLine(string lower) =>
        lower.Contains("\"level\":\"warning\"", StringComparison.Ordinal) ||
        lower.Contains("\"level\":\"warn\"", StringComparison.Ordinal) ||
        lower.Contains("\"severity\":\"warning\"", StringComparison.Ordinal) ||
        lower.Contains("[warning]", StringComparison.Ordinal) ||
        lower.Contains("[warn]", StringComparison.Ordinal) ||
        lower.Contains("timeout", StringComparison.Ordinal) ||
        lower.Contains("timed out", StringComparison.Ordinal) ||
        lower.Contains("fallback", StringComparison.Ordinal) ||
        lower.Contains("rejected", StringComparison.Ordinal) ||
        lower.Contains("desync", StringComparison.Ordinal) ||
        lower.Contains("degraded", StringComparison.Ordinal) ||
        lower.Contains("hard-snap", StringComparison.Ordinal) ||
        lower.Contains("hard snap", StringComparison.Ordinal) ||
        lower.Contains("orphaned", StringComparison.Ordinal);

    private static bool IsRuntimeSummaryLine(string lower)
    {
        if (lower.Contains("[summary]", StringComparison.Ordinal) ||
            lower.Contains("_summary]", StringComparison.Ordinal) ||
            lower.Contains("-summary", StringComparison.Ordinal) ||
            lower.Contains(".summary", StringComparison.Ordinal) ||
            lower.Contains("\"summary\"", StringComparison.Ordinal))
        {
            return true;
        }

        var isRuntimeCategory =
            ContainsDiagnosticTag(lower, "action_tx") ||
            ContainsDiagnosticTag(lower, "action_fsm") ||
            ContainsDiagnosticTag(lower, "action_apply") ||
            ContainsDiagnosticTag(lower, "aim_pose") ||
            ContainsDiagnosticTag(lower, "lasso_rope") ||
            ContainsDiagnosticTag(lower, "victim_constraint") ||
            ContainsDiagnosticTag(lower, "mission_fsm") ||
            ContainsDiagnosticTag(lower, "mission_tx") ||
            ContainsDiagnosticTag(lower, "mission_rx") ||
            ContainsDiagnosticTag(lower, "mission_world") ||
            ContainsDiagnosticTag(lower, "mission_preflight") ||
            ContainsDiagnosticTag(lower, "mission_spectator") ||
            ContainsDiagnosticTag(lower, "mission_damage") ||
            ContainsDiagnosticTag(lower, "mount_tx") ||
            ContainsDiagnosticTag(lower, "mount_local") ||
            ContainsDiagnosticTag(lower, "mount_registry") ||
            ContainsDiagnosticTag(lower, "remote_mount_rx") ||
            ContainsDiagnosticTag(lower, "remote_mount_relation") ||
            ContainsDiagnosticTag(lower, "remote_mount") ||
            ContainsDiagnosticTag(lower, "world_pool") ||
            ContainsDiagnosticTag(lower, "world_proxy_physics") ||
            ContainsDiagnosticTag(lower, "entity_graph");
        if (!isRuntimeCategory)
        {
            return false;
        }

        return lower.Contains("frames=", StringComparison.Ordinal) ||
               lower.Contains("count=", StringComparison.Ordinal) ||
               lower.Contains("counts=", StringComparison.Ordinal) ||
               lower.Contains("active=", StringComparison.Ordinal) ||
               lower.Contains("applied=", StringComparison.Ordinal) ||
               lower.Contains("corrections=", StringComparison.Ordinal) ||
               lower.Contains("max-error", StringComparison.Ordinal) ||
               lower.Contains("maxerror", StringComparison.Ordinal) ||
               lower.Contains("p95", StringComparison.Ordinal) ||
               lower.Contains("window=", StringComparison.Ordinal);
    }

    private object CreateSummary(
        LauncherSettings settings,
        PackageLayout? package,
        string roleSource)
    {
        string? HashOrNull(string? path) =>
            !string.IsNullOrWhiteSpace(path) && File.Exists(path)
                ? Hashing.FileSha256(path)
                : null;

        return new
        {
            exportedAtUtc = DateTimeOffset.UtcNow,
            os = Environment.OSVersion.VersionString,
            dotnet = Environment.Version.ToString(),
            processArchitecture = System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture.ToString(),
            role = settings.Role.ToString(),
            roleSource,
            motionReplicationMode = settings.MotionReplicationMode.ToString(),
            hostAddress = settings.HostAddress,
            sessionToken = "<REDACTED>",
            gameExecutable = Path.GetFileName(settings.GameExePath),
            gameExeSha256 = HashOrNull(settings.GameExePath),
            expectedGameSha256 = policy.GameSha256,
            bridgeSha256 = HashOrNull(package?.BridgePath),
            sidecarSha256 = HashOrNull(package?.SidecarExePath),
            manifestPresent = File.Exists(paths.InstallManifestPath)
        };
    }

    internal ObservedLauncherRole ResolveObservedRole(
        LauncherRole fallbackRole)
    {
        if (!File.Exists(paths.SidecarLogPath) ||
            PackageLocator.IsReparsePoint(paths.SidecarLogPath))
        {
            return ObservedLauncherRole.FromFallback(fallbackRole);
        }

        try
        {
            var text = ReadFileTail(paths.SidecarLogPath);
            foreach (var line in text.Split(
                         ['\r', '\n'],
                         StringSplitOptions.RemoveEmptyEntries)
                     .Reverse())
            {
                try
                {
                    using var document = JsonDocument.Parse(line);
                    var root = document.RootElement;
                    if (!root.TryGetProperty("event", out var eventElement) ||
                        eventElement.ValueKind != JsonValueKind.String ||
                        !string.Equals(
                            eventElement.GetString(),
                            "session.in-game-selected",
                            StringComparison.Ordinal) ||
                        !root.TryGetProperty("data", out var dataElement) ||
                        dataElement.ValueKind != JsonValueKind.Object ||
                        !dataElement.TryGetProperty("role", out var roleElement) ||
                        roleElement.ValueKind != JsonValueKind.String ||
                        !Enum.TryParse<LauncherRole>(
                            roleElement.GetString(),
                            ignoreCase: false,
                            out var role))
                    {
                        continue;
                    }

                    return new ObservedLauncherRole(
                        role,
                        "sidecar.session.in-game-selected");
                }
                catch (JsonException)
                {
                    // A tail can begin with a partial JSONL record. Ignore it.
                }
            }
        }
        catch (IOException)
        {
            // Diagnostics should still be exportable while the sidecar exits.
        }
        catch (UnauthorizedAccessException)
        {
            // Fall back to the persisted launcher setting.
        }

        return ObservedLauncherRole.FromFallback(fallbackRole);
    }

    private (string Name, string Path, string ArchiveEntry)[]
        DiagnosticSources() =>
    [
        ("launcher", paths.LauncherLogPath, "logs/launcher.jsonl"),
        ("sidecar.3", paths.SidecarLogPath + ".3", "logs/sidecar.jsonl.3"),
        ("sidecar.2", paths.SidecarLogPath + ".2", "logs/sidecar.jsonl.2"),
        ("sidecar.1", paths.SidecarLogPath + ".1", "logs/sidecar.jsonl.1"),
        ("sidecar", paths.SidecarLogPath, "logs/sidecar.jsonl"),
        ("sidecar-console", paths.SidecarConsoleLogPath, "logs/sidecar-console.log"),
        ("bridge.3", Path.Combine(paths.LogDirectory, "bridge.log.3"), "logs/bridge.log.3"),
        ("bridge.2", Path.Combine(paths.LogDirectory, "bridge.log.2"), "logs/bridge.log.2"),
        ("bridge.1", Path.Combine(paths.LogDirectory, "bridge.log.1"), "logs/bridge.log.1"),
        ("bridge", Path.Combine(paths.LogDirectory, "bridge.log"), "logs/bridge.log")
    ];

    private DiagnosticExportWindow CreateDiagnosticExportWindow()
    {
        var sessionStarts = new SortedSet<DateTimeOffset>();
        foreach (var source in DiagnosticSources().Where(static source =>
                     source.Name.StartsWith("sidecar", StringComparison.Ordinal) &&
                     !string.Equals(
                         source.Name,
                         "sidecar-console",
                         StringComparison.Ordinal)))
        {
            if (!File.Exists(source.Path) ||
                PackageLocator.IsReparsePoint(source.Path))
            {
                continue;
            }

            string text;
            try
            {
                text = ReadFileTail(source.Path);
            }
            catch (IOException)
            {
                continue;
            }
            catch (UnauthorizedAccessException)
            {
                continue;
            }

            foreach (var rawLine in text.Split(
                         ['\r', '\n'],
                         StringSplitOptions.RemoveEmptyEntries))
            {
                try
                {
                    using var document = JsonDocument.Parse(rawLine);
                    var root = document.RootElement;
                    if (!root.TryGetProperty("event", out var eventElement) ||
                        eventElement.ValueKind != JsonValueKind.String ||
                        !string.Equals(
                            eventElement.GetString(),
                            "runtime.started",
                            StringComparison.Ordinal) ||
                        !TryReadTimestamp(root, out var timestamp))
                    {
                        continue;
                    }
                    sessionStarts.Add(timestamp);
                }
                catch (JsonException)
                {
                    // A rotated tail can begin with one partial JSONL record.
                }
            }
        }

        var selected = sessionStarts
            .Reverse()
            .Take(MaximumDiagnosticSessions)
            .Order()
            .ToArray();
        return new DiagnosticExportWindow(
            selected.Length == 0 ? null : selected[0],
            selected);
    }

    private void AddFilteredDiagnosticLogs(
        ZipArchive archive,
        DiagnosticExportWindow exportWindow,
        SecretRedactor redactor)
    {
        AddCombinedLog(
            archive,
            "logs/launcher.jsonl",
            DiagnosticSources().Where(static source =>
                string.Equals(source.Name, "launcher", StringComparison.Ordinal)),
            exportWindow,
            redactor);
        AddCombinedLog(
            archive,
            "logs/sidecar.jsonl",
            DiagnosticSources().Where(static source =>
                source.Name.StartsWith("sidecar", StringComparison.Ordinal) &&
                !string.Equals(
                    source.Name,
                    "sidecar-console",
                    StringComparison.Ordinal)),
            exportWindow,
            redactor);
        AddCombinedLog(
            archive,
            "logs/sidecar-console.log",
            DiagnosticSources().Where(static source =>
                string.Equals(
                    source.Name,
                    "sidecar-console",
                    StringComparison.Ordinal)),
            exportWindow,
            redactor);
        AddCombinedLog(
            archive,
            "logs/bridge.log",
            DiagnosticSources().Where(static source =>
                source.Name.StartsWith("bridge", StringComparison.Ordinal)),
            exportWindow,
            redactor);
    }

    private static void AddCombinedLog(
        ZipArchive archive,
        string entryName,
        IEnumerable<(string Name, string Path, string ArchiveEntry)> sources,
        DiagnosticExportWindow exportWindow,
        SecretRedactor redactor)
    {
        var combined = new StringBuilder();
        foreach (var source in sources)
        {
            if (!File.Exists(source.Path) ||
                PackageLocator.IsReparsePoint(source.Path))
            {
                continue;
            }

            string text;
            try
            {
                text = ReadDiagnosticSource(source.Path, exportWindow);
            }
            catch (IOException)
            {
                continue;
            }
            catch (UnauthorizedAccessException)
            {
                continue;
            }

            if (string.IsNullOrWhiteSpace(text))
            {
                continue;
            }
            if (combined.Length > 0 && combined[^1] != '\n')
            {
                combined.AppendLine();
            }
            combined.Append(text);
        }

        if (combined.Length > 0)
        {
            AddText(archive, entryName, redactor.Redact(combined.ToString()));
        }
    }

    private static string ReadDiagnosticSource(
        string sourcePath,
        DiagnosticExportWindow exportWindow)
    {
        var text = ReadFileTail(sourcePath);
        if (exportWindow.CutoffUtc is not { } cutoff)
        {
            return text;
        }

        var selected = new StringBuilder();
        foreach (var rawLine in text.Split(
                     ['\r', '\n'],
                     StringSplitOptions.RemoveEmptyEntries))
        {
            if (!TryReadTimestamp(rawLine, out var timestamp) ||
                timestamp < cutoff)
            {
                continue;
            }
            selected.AppendLine(rawLine);
        }
        return selected.ToString();
    }

    private static bool TryReadTimestamp(
        JsonElement root,
        out DateTimeOffset timestamp)
    {
        foreach (var name in new[] { "timestamp", "timestampUtc" })
        {
            if (root.TryGetProperty(name, out var timestampElement) &&
                timestampElement.ValueKind == JsonValueKind.String &&
                DateTimeOffset.TryParse(
                    timestampElement.GetString(),
                    System.Globalization.CultureInfo.InvariantCulture,
                    System.Globalization.DateTimeStyles.AssumeUniversal |
                    System.Globalization.DateTimeStyles.AdjustToUniversal,
                    out timestamp))
            {
                return true;
            }
        }
        timestamp = default;
        return false;
    }

    private static bool TryReadTimestamp(
        string line,
        out DateTimeOffset timestamp)
    {
        var trimmed = line.TrimStart();
        if (trimmed.StartsWith('{'))
        {
            try
            {
                using var document = JsonDocument.Parse(trimmed);
                if (TryReadTimestamp(document.RootElement, out timestamp))
                {
                    return true;
                }
            }
            catch (JsonException)
            {
                // Fall through to the plain bridge-log timestamp parser.
            }
        }

        var separator = trimmed.IndexOf(' ');
        var candidate = separator < 0 ? trimmed : trimmed[..separator];
        return DateTimeOffset.TryParse(
            candidate,
            System.Globalization.CultureInfo.InvariantCulture,
            System.Globalization.DateTimeStyles.AssumeUniversal |
            System.Globalization.DateTimeStyles.AdjustToUniversal,
            out timestamp);
    }

    private IEnumerable<string> DiscoverNetworkAddresses(string? gameRoot)
    {
        var discovered = new HashSet<string>(
            StringComparer.OrdinalIgnoreCase);
        var sourcePaths = DiagnosticSources()
            .Select(static source => source.Path)
            .Concat(
            [
                paths.InstallManifestPath,
                Path.Combine(
                    paths.StateDirectory,
                    "recordings",
                    "ghost-last.json")
            ]);
        if (!string.IsNullOrWhiteSpace(gameRoot))
        {
            sourcePaths = sourcePaths.Append(
                Path.Combine(gameRoot, "ScriptHookRDR2.log"));
        }
        foreach (var sourcePath in sourcePaths)
        {
            if (!File.Exists(sourcePath) ||
                PackageLocator.IsReparsePoint(sourcePath))
            {
                continue;
            }

            string tail;
            try
            {
                tail = ReadFileTail(sourcePath);
            }
            catch (IOException)
            {
                continue;
            }
            catch (UnauthorizedAccessException)
            {
                continue;
            }

            foreach (Match match in NetworkAddressCandidatePattern.Matches(tail))
            {
                var literal = match.Value;
                var candidate = literal.Trim('[', ']');
                if (!IPAddress.TryParse(candidate, out var address))
                {
                    continue;
                }
                if (address.AddressFamily ==
                        System.Net.Sockets.AddressFamily.InterNetworkV6 &&
                    !candidate.Contains("::", StringComparison.Ordinal) &&
                    candidate.Count(static character => character == ':') < 4)
                {
                    // Avoid treating the HH:mm:ss portion of an ISO timestamp
                    // as an IPv6 address candidate.
                    continue;
                }
                discovered.Add(candidate);
            }
        }
        return discovered;
    }

    private static void AddText(
        ZipArchive archive,
        string entryName,
        string content)
    {
        var entry = archive.CreateEntry(entryName, CompressionLevel.Optimal);
        using var output = entry.Open();
        using var writer = new StreamWriter(output, new UTF8Encoding(false));
        writer.Write(content);
    }

    private static void AddRedactedFileTail(
        ZipArchive archive,
        string sourcePath,
        string entryName,
        SecretRedactor redactor)
    {
        if (!File.Exists(sourcePath) || PackageLocator.IsReparsePoint(sourcePath))
        {
            return;
        }

        var text = ReadFileTail(sourcePath);
        AddText(archive, entryName, redactor.Redact(text));
    }

    private static string ReadFileTail(string sourcePath)
    {
        byte[] bytes;
        using var input = new FileStream(
            sourcePath,
            FileMode.Open,
            FileAccess.Read,
            FileShare.ReadWrite | FileShare.Delete);
        if (input.Length > MaxLogBytes)
        {
            input.Seek(-MaxLogBytes, SeekOrigin.End);
        }

        using (var buffer = new MemoryStream())
        {
            input.CopyTo(buffer);
            bytes = buffer.ToArray();
        }

        return Encoding.UTF8.GetString(bytes);
    }

    internal sealed record ObservedLauncherRole(
        LauncherRole Role,
        string Source)
    {
        public static ObservedLauncherRole FromFallback(LauncherRole role) =>
            new(role, "launcher-settings-fallback");
    }

    private sealed record DiagnosticIndexReport(
        string Text,
        string SummaryJson,
        string ErrorsText,
        string WarningsText,
        string RuntimeSummariesText,
        DiagnosticAnalysisReport Analysis);

    private sealed record DiagnosticSourceSummary(
        string Name,
        bool Available,
        int ScannedLines);

    private sealed record DiagnosticExportWindow(
        DateTimeOffset? CutoffUtc,
        IReadOnlyList<DateTimeOffset> SessionStartsUtc);
}
