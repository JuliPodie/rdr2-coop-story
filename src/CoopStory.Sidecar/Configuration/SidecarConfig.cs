using System.Net;
using System.Net.Sockets;
using System.Text.Json;
using System.Text.Json.Serialization;
using CoopStory.Protocol;

namespace CoopStory.Sidecar.Configuration;

public sealed record SidecarConfig
{
    public const int CurrentSchemaVersion = 1;

    public int SchemaVersion { get; init; } = CurrentSchemaVersion;

    public SessionRole Role { get; init; } = SessionRole.Host;

    public string HostAddress { get; init; } = "127.0.0.1";

    public int TcpPort { get; init; } = 43120;

    public int UdpPort { get; init; } = 43121;

    public string PipeName { get; init; } = "CoopStory.Bridge.v1";

    public string SessionToken { get; init; } = string.Empty;

    public string Nickname { get; init; } = "Player";

    public MotionReplicationMode MotionReplicationMode { get; init; } =
        MotionReplicationMode.AnimGraphReplica;

    public bool AnimSceneStoryVmProbeEnabled { get; init; }

    public bool InGameMenuEnabled { get; init; }

    public HostSaveSelection? HostSave { get; init; }

    public string ProfilePath { get; init; } =
        "%LOCALAPPDATA%\\RDR2CoopStory\\guest-profile.json";

    // Host-only, append-safe state for campaign permissions which may be
    // replayed after a guest reconnects.  This deliberately is not an RDR2
    // save path and must never contain private player inventory or money.
    public string CapabilityJournalPath { get; init; } =
        "%LOCALAPPDATA%\\RDR2CoopStory\\campaign-capabilities.json";

    public string LogPath { get; init; } =
        "%LOCALAPPDATA%\\RDR2CoopStory\\logs\\sidecar.jsonl";

    public BubbleConfig Bubble { get; init; } = new();

    public ReplicationConfig Replication { get; init; } = new();

    public NetworkConfig Network { get; init; } = new();

    public SafetyConfig Safety { get; init; } = new();

    public SidecarConfig Validate()
    {
        if (SchemaVersion != CurrentSchemaVersion)
        {
            throw new ConfigurationException(
                $"Unsupported configuration schema {SchemaVersion}; expected {CurrentSchemaVersion}.");
        }

        if (!Enum.IsDefined(Role))
        {
            throw new ConfigurationException($"Unknown role '{Role}'.");
        }

        if (!Enum.IsDefined(MotionReplicationMode))
        {
            throw new ConfigurationException(
                $"Unknown motionReplicationMode '{MotionReplicationMode}'.");
        }

        HostAddressValidator.Validate(HostAddress);

        try
        {
            _ = PlayerIdentityRules.ValidateNickname(Nickname);
        }
        catch (ArgumentException exception)
        {
            throw new ConfigurationException(
                "nickname must contain 1-24 safe Unicode characters " +
                "and no more than 64 UTF-8 bytes.",
                exception);
        }

        ValidatePort(TcpPort, nameof(TcpPort));
        ValidatePort(UdpPort, nameof(UdpPort));

        if (string.IsNullOrWhiteSpace(PipeName) ||
            PipeName.IndexOfAny(['\\', '/', ':']) >= 0)
        {
            throw new ConfigurationException("pipeName is empty or contains invalid characters.");
        }

        if (Bubble.WarningMeters <= 0 ||
            Bubble.TeleportMeters <= Bubble.WarningMeters)
        {
            throw new ConfigurationException(
                "bubble.teleportMeters must be greater than bubble.warningMeters, and both must be positive.");
        }

        if (Replication.SnapshotRateHz is < 1 or > 60)
        {
            throw new ConfigurationException(
                "replication.snapshotRateHz must be between 1 and 60.");
        }

        if (Replication.InterpolationDelayMs is < 0 or > 2000)
        {
            throw new ConfigurationException(
                "replication.interpolationDelayMs must be between 0 and 2000.");
        }

        if (Network.HeartbeatIntervalMs < 100 ||
            Network.HeartbeatTimeoutMs <= Network.HeartbeatIntervalMs ||
            Network.ReconnectMinMs < 100 ||
            Network.ReconnectMaxMs < Network.ReconnectMinMs ||
            Network.DiagnosticsIntervalMs is < 250 or > 60_000)
        {
            throw new ConfigurationException("Network timing values are inconsistent.");
        }

        if (!Safety.StoryModeOnly || !Safety.RefuseOnlineMode)
        {
            throw new ConfigurationException(
                "Safety guards are mandatory and cannot be disabled.");
        }

        HostSave?.Validate();

        if (string.IsNullOrWhiteSpace(ProfilePath) ||
            string.IsNullOrWhiteSpace(CapabilityJournalPath) ||
            string.IsNullOrWhiteSpace(LogPath))
        {
            throw new ConfigurationException("profilePath and logPath cannot be empty.");
        }

        if (!string.IsNullOrWhiteSpace(SessionToken))
        {
            try
            {
                _ = SessionCredentials.ParseToken(SessionToken);
            }
            catch (FormatException exception)
            {
                throw new ConfigurationException("sessionToken is invalid.", exception);
            }
        }

        return this;
    }

    public string ExpandedProfilePath => ExpandPath(ProfilePath);

    public string ExpandedCapabilityJournalPath => ExpandPath(CapabilityJournalPath);

    public string ExpandedLogPath => ExpandPath(LogPath);

    private static string ExpandPath(string path) =>
        Path.GetFullPath(Environment.ExpandEnvironmentVariables(path));

    private static void ValidatePort(int port, string name)
    {
        if (port is < 1 or > 65535)
        {
            throw new ConfigurationException($"{name} must be between 1 and 65535.");
        }
    }
}

public enum MotionReplicationMode
{
    [JsonStringEnumMemberName("task_navmesh")]
    TaskNavmesh,

    [JsonStringEnumMemberName("animgraph_replica")]
    AnimGraphReplica
}

public sealed record HostSaveSelection
{
    public string FileName { get; init; } = string.Empty;

    public string Sha256 { get; init; } = string.Empty;

    public bool SelectedLocally { get; init; }

    public bool AutomaticGameLoad { get; init; }

    public void Validate()
    {
        if (!FileName.StartsWith(
                "SRDR",
                StringComparison.OrdinalIgnoreCase) ||
            FileName.IndexOfAny(
                Path.GetInvalidFileNameChars()) >= 0 ||
            Sha256.Length != 64 ||
            Sha256.Any(static character =>
                !Uri.IsHexDigit(character)) ||
            !SelectedLocally ||
            AutomaticGameLoad)
        {
            throw new ConfigurationException(
                "hostSave must identify a local SRDR file by SHA-256 and cannot request automatic loading.");
        }
    }
}

public static class HostAddressValidator
{
    private const int MaximumDnsNameLength = 253;

    public static string Validate(string hostAddress, bool requireRemote = false)
    {
        if (string.IsNullOrWhiteSpace(hostAddress))
        {
            throw new ConfigurationException("hostAddress cannot be empty.");
        }

        if (!string.Equals(hostAddress, hostAddress.Trim(), StringComparison.Ordinal) ||
            hostAddress.Length > MaximumDnsNameLength ||
            hostAddress.IndexOfAny(['/', '\\', ':', '@', '?', '#']) >= 0)
        {
            throw new ConfigurationException(
                "hostAddress must be an IPv4 address or DNS host name without a port or URI.");
        }

        if (IPAddress.TryParse(hostAddress, out var address))
        {
            if (address.AddressFamily != AddressFamily.InterNetwork)
            {
                throw new ConfigurationException(
                    "This build accepts only an IPv4 hostAddress.");
            }

            var firstOctet = address.GetAddressBytes()[0];
            if (firstOctet == 0 ||
                address.Equals(IPAddress.Any) ||
                address.Equals(IPAddress.Broadcast) ||
                firstOctet is >= 224 and <= 239)
            {
                throw new ConfigurationException(
                    "hostAddress cannot be unspecified, broadcast, or multicast.");
            }

            if (requireRemote && IPAddress.IsLoopback(address))
            {
                throw new ConfigurationException(
                    "A two-PC session cannot use a loopback hostAddress.");
            }

            return hostAddress;
        }

        if (hostAddress.All(static character =>
                char.IsAsciiDigit(character) || character == '.'))
        {
            throw new ConfigurationException(
                "hostAddress looks like an IPv4 address but contains invalid octets.");
        }

        if (Uri.CheckHostName(hostAddress) != UriHostNameType.Dns ||
            hostAddress.StartsWith(".", StringComparison.Ordinal) ||
            hostAddress.EndsWith(".", StringComparison.Ordinal) ||
            hostAddress.Contains("..", StringComparison.Ordinal))
        {
            throw new ConfigurationException(
                "hostAddress is not a valid IPv4 address or DNS host name.");
        }

        if (requireRemote &&
            (hostAddress.Equals("localhost", StringComparison.OrdinalIgnoreCase) ||
             hostAddress.EndsWith(
                 ".localhost",
                 StringComparison.OrdinalIgnoreCase) ||
             hostAddress.Equals(
                 "localhost.localdomain",
                 StringComparison.OrdinalIgnoreCase)))
        {
            throw new ConfigurationException(
                "A two-PC session cannot use localhost as hostAddress.");
        }

        return hostAddress;
    }
}

public sealed record BubbleConfig
{
    public float WarningMeters { get; init; } = 200f;

    public float TeleportMeters { get; init; } = 250f;
}

public sealed record ReplicationConfig
{
    public int SnapshotRateHz { get; init; } = 20;

    public int InterpolationDelayMs { get; init; } = 100;
}

public sealed record NetworkConfig
{
    public int HeartbeatIntervalMs { get; init; } = 1000;

    public int HeartbeatTimeoutMs { get; init; } = 5000;

    public int ReconnectMinMs { get; init; } = 500;

    public int ReconnectMaxMs { get; init; } = 10000;

    public int DiagnosticsIntervalMs { get; init; } = 5000;
}

public sealed record SafetyConfig
{
    public bool StoryModeOnly { get; init; } = true;

    public bool RefuseOnlineMode { get; init; } = true;
}

public static class SidecarConfigStore
{
    public static async Task<SidecarConfig> LoadAsync(
        string path,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        try
        {
            await using var stream = new FileStream(
                path,
                FileMode.Open,
                FileAccess.Read,
                FileShare.Read,
                16 * 1024,
                FileOptions.Asynchronous | FileOptions.SequentialScan);
            var config = await JsonSerializer.DeserializeAsync<SidecarConfig>(
                stream,
                PayloadJson.Options,
                cancellationToken).ConfigureAwait(false);
            return (config ?? throw new ConfigurationException(
                "Configuration contains JSON null.")).Validate();
        }
        catch (JsonException exception)
        {
            throw new ConfigurationException("Configuration JSON is invalid.", exception);
        }
    }

    public static async Task SaveAsync(
        string path,
        SidecarConfig config,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentNullException.ThrowIfNull(config);
        config.Validate();
        var fullPath = Path.GetFullPath(path);
        Directory.CreateDirectory(
            Path.GetDirectoryName(fullPath)
                ?? throw new ConfigurationException("Configuration path has no directory."));
        await using var stream = new FileStream(
            fullPath,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.None,
            16 * 1024,
            FileOptions.Asynchronous | FileOptions.WriteThrough);
        await JsonSerializer.SerializeAsync(
            stream,
            config,
            PayloadJson.Options,
            cancellationToken).ConfigureAwait(false);
        await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
        stream.Flush(flushToDisk: true);
    }
}

public sealed class ConfigurationException : Exception
{
    public ConfigurationException(string message)
        : base(message)
    {
    }

    public ConfigurationException(string message, Exception innerException)
        : base(message, innerException)
    {
    }
}
