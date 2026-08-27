using System.Text.Json.Serialization;

namespace CoopStory.Launcher;

public enum LauncherRole
{
    Host,
    Guest
}

public enum LauncherPlatform
{
    Steam,
    Rockstar
}

public enum LauncherMode
{
    Solo,
    Host,
    Guest
}

public enum LauncherMotionReplicationMode
{
    TaskNavmesh,
    AnimGraphReplica
}

public sealed record LauncherLobbySnapshot(
    string LocalNickname,
    LauncherRole LocalRole,
    string RemoteNickname,
    LauncherRole RemoteRole,
    string RemoteAddress,
    bool SidecarRunning,
    bool PeerConnected,
    bool GameBridgeConnected,
    long? PingMilliseconds)
{
    public static LauncherLobbySnapshot Empty { get; } = new(
        "Player",
        LauncherRole.Host,
        string.Empty,
        LauncherRole.Guest,
        string.Empty,
        false,
        false,
        false,
        null);
}

public sealed record LauncherSettings
{
    public int SchemaVersion { get; init; } = 1;

    public string GameExePath { get; init; } = string.Empty;

    public string ScriptHookFolder { get; init; } = string.Empty;

    public string Nickname { get; init; } = "Player";

    [JsonConverter(typeof(JsonStringEnumConverter<LauncherRole>))]
    public LauncherRole Role { get; init; } = LauncherRole.Host;

    [JsonConverter(typeof(JsonStringEnumConverter<LauncherPlatform>))]
    public LauncherPlatform Platform { get; init; } = LauncherPlatform.Steam;

    public string HostAddress { get; init; } = string.Empty;

    public string SessionToken { get; init; } = string.Empty;

    public string HostSavePath { get; init; } = string.Empty;

    public string DiagnosticsExportFolder { get; init; } = string.Empty;

    [JsonConverter(typeof(JsonStringEnumConverter<LauncherMotionReplicationMode>))]
    public LauncherMotionReplicationMode MotionReplicationMode { get; init; } =
        LauncherMotionReplicationMode.AnimGraphReplica;

    public bool AnimSceneStoryVmProbeEnabled { get; init; }

    public LauncherMode? LastMode { get; init; }
}

public sealed record LauncherPaths(
    string StateDirectory,
    string LogDirectory,
    string LauncherLogPath,
    string SidecarLogPath,
    string SidecarConsoleLogPath,
    string SettingsPath,
    string InstallManifestPath,
    string MachineIdPath,
    string SidecarConfigPath,
    string DiagnosticsDirectory)
{
    public static LauncherPaths CreateDefault()
    {
        var localAppData = Environment.GetFolderPath(
            Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrWhiteSpace(localAppData))
        {
            throw new LauncherException(
                "The system did not provide a LocalAppData directory.");
        }

        return CreateUnder(Path.Combine(localAppData, "RDR2CoopStory", "launcher"));
    }

    public static LauncherPaths CreateUnder(string stateDirectory)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(stateDirectory);
        var state = Path.GetFullPath(stateDirectory);
        var logs = Path.Combine(state, "logs");
        return new LauncherPaths(
            state,
            logs,
            Path.Combine(logs, "launcher.jsonl"),
            Path.Combine(logs, "sidecar.jsonl"),
            Path.Combine(logs, "sidecar-console.log"),
            Path.Combine(state, "launcher-settings.json"),
            Path.Combine(state, "install-manifest.json"),
            Path.Combine(state, "machine-id.txt"),
            Path.Combine(state, "sidecar.config.json"),
            Path.Combine(state, "diagnostics"));
    }

    public void EnsureDirectories()
    {
        Directory.CreateDirectory(StateDirectory);
        Directory.CreateDirectory(LogDirectory);
        Directory.CreateDirectory(DiagnosticsDirectory);
    }
}

public sealed record LauncherPolicy
{
    public const string ProductionGameSha256 =
        "B56C9548F670654A9B73BF25DEF3CD73AF12E269F6E47DBA28A34079ADAF465E";

    public const string ProductionScriptHookSha256 =
        "3AC29FBE8C92B664E358F7D4F0AF2EC9F1CA674885975087EF76BD98BF972A4C";

    public const string ProductionDinputSha256 =
        "956FB3765572D00F6C08BCAE11E9856A00A68107464A87B6CCC6C1FFED46B88A";

    public string GameSha256 { get; init; } = ProductionGameSha256;

    public string ScriptHookSha256 { get; init; } = ProductionScriptHookSha256;

    public string DinputSha256 { get; init; } = ProductionDinputSha256;

    public string SupportedGameVersion { get; init; } = "1.0.1491.50";

    public static LauncherPolicy Production { get; } = new();
}

public sealed record PackageLayout(
    string Root,
    string BridgePath,
    string ConfigTemplatePath,
    string SidecarDirectory,
    string SidecarExePath);

public sealed record RuntimeLayout(
    string Root,
    string ScriptHookPath,
    string DinputPath,
    bool TrainerWasPresent);

public sealed record VerificationReport(
    bool IsValid,
    bool IsInstalled,
    IReadOnlyList<string> Messages,
    string? PackageRoot,
    string? GameRoot,
    string? RuntimeRoot)
{
    public string Summary => string.Join(Environment.NewLine, Messages);
}

public sealed record InstallRequest(
    LauncherSettings Settings,
    PackageLayout Package);

public sealed record InstalledFileRecord
{
    public required string RelativePath { get; init; }

    public required string Sha256 { get; init; }

    public required long Length { get; init; }

    public required bool Owned { get; init; }

    public required string InstallStageRelativePath { get; init; }

    public required string UninstallStageRelativePath { get; init; }
}

public enum InstallPhase
{
    Prepared,
    Committed,
    Uninstalling
}

public sealed record InstallManifest
{
    public int SchemaVersion { get; init; } = 1;

    public required Guid InstallId { get; init; }

    public required Guid MachineId { get; init; }

    public required DateTimeOffset CreatedAtUtc { get; init; }

    [JsonConverter(typeof(JsonStringEnumConverter<InstallPhase>))]
    public required InstallPhase Phase { get; init; }

    public required string GameRoot { get; init; }

    public required string GameRootFingerprint { get; init; }

    public required string GameExecutableSha256 { get; init; }

    public required string PackageContentSha256 { get; init; }

    public required IReadOnlyList<InstalledFileRecord> Files { get; init; }
}

public sealed class LauncherException : Exception
{
    public LauncherException(string message)
        : base(message)
    {
    }

    public LauncherException(string message, Exception innerException)
        : base(message, innerException)
    {
    }
}
