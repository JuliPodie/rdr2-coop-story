using System.Text.Json;
using CoopStory.Protocol;

namespace CoopStory.Launcher;

public static class SidecarConfiguration
{
    public static void ValidateSettings(LauncherSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        if (!Enum.IsDefined(settings.MotionReplicationMode))
        {
            throw new LauncherException(
                "The selected motion replication engine is unsupported.");
        }

        try
        {
            _ = PlayerIdentityRules.ValidateNickname(settings.Nickname);
        }
        catch (ArgumentException exception)
        {
            throw new LauncherException(
                "Nickname must contain 1-24 characters and at most 64 UTF-8 bytes, " +
                "with no control characters.",
                exception);
        }
        if (!string.IsNullOrWhiteSpace(settings.SessionToken))
        {
            SessionCodeService.Validate(settings.SessionToken);
        }
        if (!string.IsNullOrWhiteSpace(settings.HostAddress))
        {
            _ = InviteService.ValidateRemoteHost(settings.HostAddress);
        }
        if (!string.IsNullOrWhiteSpace(settings.HostSavePath))
        {
            var savePath = Path.GetFullPath(settings.HostSavePath);
            if (!File.Exists(savePath) ||
                !Path.GetFileName(savePath).StartsWith(
                    "SRDR",
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new LauncherException(
                    "The selected host save must be an existing local SRDR* file.");
            }
        }
    }

    public static byte[] CreateBytes(
        LauncherSettings settings,
        LauncherPaths paths)
    {
        ValidateSettings(settings);
        var token = string.IsNullOrWhiteSpace(settings.SessionToken)
            ? SessionCodeService.Generate()
            : settings.SessionToken.Trim();
        var hostAddress = string.IsNullOrWhiteSpace(settings.HostAddress)
            ? "127.0.0.1"
            : settings.HostAddress.Trim();
        var hostSavePath =
            string.IsNullOrWhiteSpace(settings.HostSavePath)
                ? null
                : Path.GetFullPath(settings.HostSavePath);
        var motionReplicationMode = settings.MotionReplicationMode switch
        {
            LauncherMotionReplicationMode.TaskNavmesh => "task_navmesh",
            LauncherMotionReplicationMode.AnimGraphReplica =>
                "animgraph_replica",
            _ => throw new LauncherException(
                "The selected motion replication engine is unsupported.")
        };
        var launcherSessionReady =
            !string.IsNullOrWhiteSpace(settings.HostAddress) &&
            !string.IsNullOrWhiteSpace(settings.SessionToken);
        var document = new
        {
            schemaVersion = 1,
            role = settings.Role.ToString(),
            hostAddress,
            tcpPort = 43120,
            udpPort = 43121,
            pipeName = "CoopStory.Bridge.v1",
            sessionToken = token,
            nickname = settings.Nickname,
            motionReplicationMode,
            animSceneStoryVmProbeEnabled =
                settings.AnimSceneStoryVmProbeEnabled,
            hostSave = hostSavePath is null
                ? null
                : new
                {
                    fileName = Path.GetFileName(hostSavePath),
                    sha256 = Hashing.FileSha256(hostSavePath),
                    selectedLocally = true,
                    automaticGameLoad = false
                },
            // A complete HOST/JOIN selection made in the launcher starts the
            // network immediately. The in-game F8 bootstrap remains available
            // only for deliberately blank developer configurations.
            inGameMenuEnabled = !launcherSessionReady,
            profilePath = Path.Combine(paths.StateDirectory, "guest-profile.json"),
            logPath = paths.SidecarLogPath,
            bubble = new
            {
                warningMeters = 200.0,
                teleportMeters = 250.0
            },
            replication = new
            {
                snapshotRateHz = 20,
                interpolationDelayMs = 100
            },
            network = new
            {
                heartbeatIntervalMs = 1000,
                heartbeatTimeoutMs = 5000,
                reconnectMinMs = 500,
                reconnectMaxMs = 10000
            },
            safety = new
            {
                storyModeOnly = true,
                refuseOnlineMode = true
            }
        };
        return JsonSerializer.SerializeToUtf8Bytes(document, JsonSupport.Options);
    }

    public static void Save(
        LauncherSettings settings,
        LauncherPaths paths) =>
        AtomicFile.WriteBytes(
            paths.SidecarConfigPath,
            CreateBytes(settings, paths));

    public static string RedactedJson(LauncherSettings settings)
    {
        var token = settings.SessionToken;
        var redacted = settings with
        {
            SessionToken = string.IsNullOrWhiteSpace(token)
                ? string.Empty
                : $"<REDACTED length={token.Length}>"
        };
        return JsonSerializer.Serialize(
            new
            {
                settings = new
                {
                    redacted.SchemaVersion,
                    gameExecutable = Path.GetFileName(redacted.GameExePath),
                    scriptHookFolderSelected =
                        !string.IsNullOrWhiteSpace(redacted.ScriptHookFolder),
                    redacted.Nickname,
                    motionReplicationMode =
                        redacted.MotionReplicationMode.ToString(),
                    redacted.AnimSceneStoryVmProbeEnabled,
                    role = redacted.Role.ToString(),
                    redacted.HostAddress,
                    redacted.SessionToken
                    ,
                    hostSaveSelected =
                        !string.IsNullOrWhiteSpace(
                            redacted.HostSavePath),
                    hostSaveFileName =
                        string.IsNullOrWhiteSpace(
                            redacted.HostSavePath)
                            ? null
                            : Path.GetFileName(
                                redacted.HostSavePath)
                },
                logDirectory = "<LOCALAPPDATA>/RDR2CoopStory/launcher/logs"
            },
            JsonSupport.Options);
    }
}
