using CoopStory.Protocol;
using CoopStory.Sidecar.Configuration;
using CoopStory.Sidecar.Diagnostics;
using CoopStory.Sidecar.Persistence;
using CoopStory.Sidecar.Session;
using CoopStory.Sidecar.Simulation;
using System.Net.Sockets;

namespace CoopStory.Sidecar;

// Command-line entry point for the C# multiplayer helper.
// It can run a real Sidecar, create/join setup data, run local tests, or export diagnostics.
internal static class Program
{
    public static async Task<int> Main(string[] args)
    {
        try
        {
            if (args.Length == 0 || IsHelp(args[0]))
            {
                PrintHelp();
                return 0;
            }

            return args[0].ToLowerInvariant() switch
            {
                "run" => await RunSidecarAsync(args[1..]).ConfigureAwait(false),
                "local-test" => await RunLocalGameTestAsync(args[1..])
                    .ConfigureAwait(false),
                "simulate" => await RunSimulationAsync(args[1..]).ConfigureAwait(false),
                "init-config" => await InitializeConfigAsync(args[1..]).ConfigureAwait(false),
                "create-session" => await CreateSessionAsync(args[1..])
                    .ConfigureAwait(false),
                "export-diagnostics" => await ExportDiagnosticsAsync(args[1..])
                    .ConfigureAwait(false),
                _ => FailUsage($"Unknown command '{args[0]}'.")
            };
        }
        catch (Exception exception) when (
            exception is ConfigurationException or
                         FormatException or
                         IOException or
                         ProtocolException or
                         GuestProfileException or
                         SocketException or
                         UnauthorizedAccessException or
                         TimeoutException or
                         BridgeShutdownException or
                         SafetyViolationException)
        {
            Console.Error.WriteLine($"ERROR: {exception.Message}");
            return 1;
        }
    }

    private static async Task<int> RunSidecarAsync(string[] args)
    {
        var options = CommandLineOptions.Parse(args);
        var configPath = options.Required("config");
        var config = await SidecarConfigStore.LoadAsync(configPath).ConfigureAwait(false);
        if (string.IsNullOrWhiteSpace(config.SessionToken))
        {
            throw new ConfigurationException(
                "run requires a saved sessionToken. Use create-session to generate " +
                "a matched host.config.json and guest.config.json pair.");
        }

        // A guest must target another PC.
        // The host binds its listeners to all IPv4 interfaces, so hostAddress is descriptive for that role and may remain loopback in a locally generated launcher config.
        HostAddressValidator.Validate(
            config.HostAddress,
            requireRemote: config.Role == SessionRole.Guest);
        var credentials = SessionCredentials.ParseToken(config.SessionToken);
        await using var logger = new JsonLineLogger(config.ExpandedLogPath);
        await using var runtime = new SidecarRuntime(config, credentials, logger);
        using var stop = CreateConsoleCancellation();
        await runtime.RunAsync(stop.Token).ConfigureAwait(false);
        return 0;
    }

    private static async Task<int> CreateSessionAsync(string[] args)
    {
        var options = CommandLineOptions.Parse(args);
        var output = options.Required("output");
        var hostAddress = options.Required("host-address");
        var tcpPort = options.GetInt("tcp-port", 43120, 1, 65535);
        var udpPort = options.GetInt("udp-port", 43121, 1, 65535);
        var result = await SessionConfigPairGenerator.CreateAsync(
            output,
            hostAddress,
            tcpPort,
            udpPort).ConfigureAwait(false);
        Console.WriteLine($"SESSION_CONFIGS_READY={result.DirectoryPath}");
        Console.WriteLine($"HOST_CONFIG={result.HostConfigPath}");
        Console.WriteLine($"GUEST_CONFIG={result.GuestConfigPath}");
        Console.WriteLine(
            "The two files contain a private shared credential; send only " +
            "guest.config.json to the invited player.");
        return 0;
    }

    private static async Task<int> ExportDiagnosticsAsync(string[] args)
    {
        var options = CommandLineOptions.Parse(args);
        var result = await DiagnosticsExporter.ExportAsync(
            options.Required("config"),
            options.Required("output")).ConfigureAwait(false);
        Console.WriteLine($"DIAGNOSTICS_READY={result.OutputPath}");
        Console.WriteLine(
            result.IncludedLog
                ? $"LOG_SOURCE_BYTES={result.SourceLogBytes}"
                : "LOG_NOT_FOUND: the archive contains configuration and environment only.");
        Console.WriteLine(
            "The sessionToken was redacted; this ZIP is safe to share for debugging.");
        return 0;
    }

    private static async Task<int> RunLocalGameTestAsync(string[] args)
    {
        var options = CommandLineOptions.Parse(args);
        var configPath = options.Required("config");
        var readyFile = options.Optional("ready-file");
        var motionProfile = ParseLocalTestMotionProfile(
            options.Optional("motion-profile"));
        var waitForF9 = ParseBooleanOption(
            options.Optional("wait-for-f9"),
            "wait-for-f9");
        var ghostRecordingPath = options.Optional("ghost-recording");
        var config = await SidecarConfigStore.LoadAsync(configPath)
            .ConfigureAwait(false);
        if (config.Role != SessionRole.Host)
        {
            throw new ConfigurationException(
                "local-test requires role Host in the configuration file.");
        }

        var credentials = ResolveCredentials(config);
        await using var logger = new JsonLineLogger(config.ExpandedLogPath);
        using var stop = CreateConsoleCancellation();

        Console.WriteLine(
            "LOCAL_TEST_RUNNING: start RDR2 Story Mode and load a save.");
        Console.WriteLine(
            "Waiting for a live player position before sending the synthetic guest.");
        Console.WriteLine(
            motionProfile switch
            {
                LocalGameTestMotionProfile.LiveMirror =>
                    "Live mirror: the co-op replica repeats the real local player at a fixed offset.",
                LocalGameTestMotionProfile.PuppetCourse =>
                    "Synthetic guest course: idle, walk, run, sprint, stop and reverse.",
                _ => "Synthetic guest follows the local player at a fixed offset."
            });
        if (waitForF9)
        {
            Console.WriteLine(
                "In Story Mode open F9 and choose 'Live mirror: start / stop'.");
        }
        Console.WriteLine(
            "Never enter Red Dead Online while the mod is loaded. Press Ctrl+C to stop.");

        var readySignalWritten = 0;
        void ReportStatus(string message)
        {
            Console.WriteLine(message);
            if (readyFile is not null &&
                message.StartsWith(
                    "LOCAL_TEST_PEER_READY",
                    StringComparison.Ordinal) &&
                Interlocked.Exchange(ref readySignalWritten, 1) == 0)
            {
                WriteReadySignal(readyFile);
            }
        }

        var result = await LocalGameTestSession.RunAsync(
            config,
            credentials,
            logger,
            stop.Token,
            ReportStatus,
            motionProfile,
            waitForF9,
            ghostRecordingPath).ConfigureAwait(false);
        Console.WriteLine(
            $"LOCAL_TEST_STOPPED hostSnapshots={result.HostSnapshotsObserved} " +
            $"guestSnapshots={result.GuestSnapshotsSent} " +
            $"bridgeDeliveries={result.GuestSnapshotsDelivered}");
        return result.GuestSnapshotsDelivered > 0 ? 0 : 2;
    }

    private static LocalGameTestMotionProfile ParseLocalTestMotionProfile(
        string? value)
    {
        if (string.IsNullOrWhiteSpace(value) ||
            value.Equals("mirror", StringComparison.OrdinalIgnoreCase) ||
            value.Equals("live-mirror", StringComparison.OrdinalIgnoreCase))
        {
            return LocalGameTestMotionProfile.LiveMirror;
        }

        if (
            value.Equals("puppet", StringComparison.OrdinalIgnoreCase) ||
            value.Equals("course", StringComparison.OrdinalIgnoreCase))
        {
            return LocalGameTestMotionProfile.PuppetCourse;
        }

        if (value.Equals("follow", StringComparison.OrdinalIgnoreCase))
        {
            return LocalGameTestMotionProfile.FollowHost;
        }

        throw new ConfigurationException(
            "local-test --motion-profile must be 'mirror', 'puppet' or 'follow'.");
    }

    private static bool ParseBooleanOption(string? value, string optionName)
    {
        if (value is null)
        {
            return false;
        }
        if (bool.TryParse(value, out var parsed))
        {
            return parsed;
        }
        throw new ConfigurationException(
            $"Option '--{optionName}' must be 'true' or 'false'.");
    }

    private static void WriteReadySignal(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        var fullPath = Path.GetFullPath(path);
        var parent = Path.GetDirectoryName(fullPath)
            ?? throw new ConfigurationException(
                "local-test ready-file path has no parent directory.");
        if (!Directory.Exists(parent))
        {
            throw new ConfigurationException(
                "local-test ready-file parent directory does not exist.");
        }

        var content =
            $"pid={Environment.ProcessId};utc={DateTimeOffset.UtcNow:o}";
        var bytes = System.Text.Encoding.UTF8.GetBytes(content);
        using var stream = new FileStream(
            fullPath,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.Read,
            bufferSize: 4096,
            FileOptions.WriteThrough);
        stream.Write(bytes);
        stream.Flush(flushToDisk: true);
    }

    private static async Task<int> RunSimulationAsync(string[] args)
    {
        var options = CommandLineOptions.Parse(args);
        var config = options.Optional("config") is { } path
            ? await SidecarConfigStore.LoadAsync(path).ConfigureAwait(false)
            : new SidecarConfig();
        config.Validate();

        var durationSeconds = options.GetInt("duration", 5, 1, 3600);
        var profile = new NetworkImpairmentProfile
        {
            LatencyMs = options.GetInt("latency", 0, 0, 5000),
            JitterMs = options.GetInt("jitter", 0, 0, 5000),
            LossRate = options.GetDouble("loss", 0, 0, 1),
            ReorderRate = options.GetDouble("reorder", 0, 0, 1),
            Seed = options.GetInt("seed", 1, int.MinValue, int.MaxValue)
        }.Validate();
        var credentials = string.IsNullOrWhiteSpace(config.SessionToken)
            ? SessionCredentials.Generate()
            : SessionCredentials.ParseToken(config.SessionToken);

        var simulationLog = Path.Combine(
            Path.GetTempPath(),
            "RDR2CoopStory",
            "simulation.jsonl");
        await using var logger = new JsonLineLogger(simulationLog);
        using var stop = CreateConsoleCancellation();
        var result = await SyntheticPeerSimulator.RunAsync(
            config,
            credentials,
            logger,
            TimeSpan.FromSeconds(durationSeconds),
            profile,
            stop.Token).ConfigureAwait(false);

        Console.WriteLine(
            $"SIMULATION_OK attempted={result.AttemptedSnapshots} " +
            $"delivered={result.DeliveredByImpairment} received={result.ReceivedByHost} " +
            $"durationMs={result.Duration.TotalMilliseconds:F0}");
        Console.WriteLine($"log={simulationLog}");
        return result.ReceivedByHost > 0 ? 0 : 2;
    }

    private static async Task<int> InitializeConfigAsync(string[] args)
    {
        var options = CommandLineOptions.Parse(args);
        var output = options.Required("output");
        var roleText = options.Optional("role") ?? nameof(SessionRole.Host);
        if (!Enum.TryParse<SessionRole>(roleText, ignoreCase: true, out var role))
        {
            return FailUsage($"Unknown role '{roleText}'.");
        }

        var credentials = SessionCredentials.Generate();
        var config = new SidecarConfig
        {
            Role = role,
            SessionToken = credentials.ExportToken()
        };
        await SidecarConfigStore.SaveAsync(output, config).ConfigureAwait(false);
        Console.WriteLine($"Created {Path.GetFullPath(output)}");
        Console.WriteLine(
            "Use the same sessionToken on host and guest; keep it private.");
        return 0;
    }

    private static SessionCredentials ResolveCredentials(SidecarConfig config)
    {
        if (!string.IsNullOrWhiteSpace(config.SessionToken))
        {
            return SessionCredentials.ParseToken(config.SessionToken);
        }

        if (config.Role == SessionRole.Guest)
        {
            throw new ConfigurationException(
                "Guest configuration requires the host's sessionToken.");
        }

        return SessionCredentials.Generate();
    }

    private static CancellationTokenSource CreateConsoleCancellation()
    {
        var source = new CancellationTokenSource();
        Console.CancelKeyPress += (_, eventArgs) =>
        {
            eventArgs.Cancel = true;
            source.Cancel();
        };
        return source;
    }

    private static bool IsHelp(string argument) =>
        argument is "-h" or "--help" or "help";

    private static int FailUsage(string message)
    {
        Console.Error.WriteLine(message);
        PrintHelp();
        return 64;
    }

    private static void PrintHelp()
    {
        Console.WriteLine(
            """
            RDR2 Coop Story sidecar

              CoopStory.Sidecar.exe run --config <path>
              CoopStory.Sidecar.exe local-test --config <host-config-path>
                  [--ready-file <create-new-signal-path>]
                  [--motion-profile mirror|puppet|follow]
                  [--wait-for-f9 true|false]
                  [--ghost-recording <persistent-json-path>]
              CoopStory.Sidecar.exe simulate [--config <path>] [--duration <seconds>]
                  [--latency <ms>] [--jitter <ms>] [--loss <0..1>]
                  [--reorder <0..1>] [--seed <integer>]
              CoopStory.Sidecar.exe init-config --output <path> [--role Host|Guest]
              CoopStory.Sidecar.exe create-session --output <new-directory>
                  --host-address <LAN-IPv4-or-DNS-name>
                  [--tcp-port <port>] [--udp-port <port>]
              CoopStory.Sidecar.exe export-diagnostics --config <path>
                  --output <new-zip-path>
            """);
    }

    private sealed class CommandLineOptions
    {
        private readonly Dictionary<string, string> _values;

        private CommandLineOptions(Dictionary<string, string> values)
        {
            _values = values;
        }

        public static CommandLineOptions Parse(string[] args)
        {
            var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            for (var index = 0; index < args.Length; index++)
            {
                var argument = args[index];
                if (!argument.StartsWith("--", StringComparison.Ordinal) ||
                    argument.Length == 2)
                {
                    throw new ConfigurationException($"Invalid option '{argument}'.");
                }

                if (index + 1 >= args.Length ||
                    args[index + 1].StartsWith("--", StringComparison.Ordinal))
                {
                    throw new ConfigurationException($"Option '{argument}' requires a value.");
                }

                var name = argument[2..];
                if (!values.TryAdd(name, args[++index]))
                {
                    throw new ConfigurationException($"Option '--{name}' was supplied twice.");
                }
            }

            return new CommandLineOptions(values);
        }

        public string Required(string name) =>
            Optional(name) ??
            throw new ConfigurationException($"Missing required option '--{name}'.");

        public string? Optional(string name) =>
            _values.GetValueOrDefault(name);

        public int RequiredInt32(string name)
        {
            var text = Required(name);
            if (!int.TryParse(text, out var value) || value < 0)
            {
                throw new ConfigurationException(
                    $"Option '--{name}' must be a non-negative integer.");
            }
            return value;
        }

        public int GetInt(string name, int fallback, int minimum, int maximum)
        {
            var text = Optional(name);
            if (text is null)
            {
                return fallback;
            }

            if (!int.TryParse(text, out var value) ||
                value < minimum ||
                value > maximum)
            {
                throw new ConfigurationException(
                    $"Option '--{name}' must be between {minimum} and {maximum}.");
            }

            return value;
        }

        public double GetDouble(
            string name,
            double fallback,
            double minimum,
            double maximum)
        {
            var text = Optional(name);
            if (text is null)
            {
                return fallback;
            }

            if (!double.TryParse(
                    text,
                    System.Globalization.NumberStyles.Float,
                    System.Globalization.CultureInfo.InvariantCulture,
                    out var value) ||
                value < minimum ||
                value > maximum)
            {
                throw new ConfigurationException(
                    $"Option '--{name}' must be between {minimum} and {maximum}.");
            }

            return value;
        }
    }
}
