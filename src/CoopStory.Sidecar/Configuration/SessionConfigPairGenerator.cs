using CoopStory.Protocol;

namespace CoopStory.Sidecar.Configuration;

public sealed record SessionConfigPairResult(
    string DirectoryPath,
    string HostConfigPath,
    string GuestConfigPath);

public static class SessionConfigPairGenerator
{
    public const string HostFileName = "host.config.json";
    public const string GuestFileName = "guest.config.json";

    public static async Task<SessionConfigPairResult> CreateAsync(
        string outputDirectory,
        string hostAddress,
        int tcpPort = 43120,
        int udpPort = 43121,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(outputDirectory);
        hostAddress = HostAddressValidator.Validate(
            hostAddress,
            requireRemote: true);

        var output = Path.GetFullPath(outputDirectory);
        if (File.Exists(output) || Directory.Exists(output))
        {
            throw new ConfigurationException(
                "Session output path already exists; choose a new directory.");
        }

        var parent = Path.GetDirectoryName(output)
            ?? throw new ConfigurationException(
                "Session output path has no parent directory.");
        Directory.CreateDirectory(parent);

        var credentials = SessionCredentials.Generate();
        var common = new SidecarConfig
        {
            HostAddress = hostAddress,
            TcpPort = tcpPort,
            UdpPort = udpPort,
            SessionToken = credentials.ExportToken()
        };
        var hostConfig = (common with
        {
            Role = SessionRole.Host,
            Nickname = "Host",
            LogPath =
                "%LOCALAPPDATA%\\RDR2CoopStory\\logs\\host-sidecar.jsonl"
        }).Validate();
        var guestConfig = (common with
        {
            Role = SessionRole.Guest,
            Nickname = "Guest",
            LogPath =
                "%LOCALAPPDATA%\\RDR2CoopStory\\logs\\guest-sidecar.jsonl"
        }).Validate();

        var staging = Path.Combine(
            parent,
            $".coopstory-session-{Guid.NewGuid():N}.tmp");
        Directory.CreateDirectory(staging);
        try
        {
            var stagedHost = Path.Combine(staging, HostFileName);
            var stagedGuest = Path.Combine(staging, GuestFileName);
            await SidecarConfigStore.SaveAsync(
                stagedHost,
                hostConfig,
                cancellationToken).ConfigureAwait(false);
            await SidecarConfigStore.SaveAsync(
                stagedGuest,
                guestConfig,
                cancellationToken).ConfigureAwait(false);

            // The same-volume move publishes both credentials atomically.
            Directory.Move(staging, output);
        }
        catch
        {
            DeleteOwnedStagingDirectory(staging, parent);
            throw;
        }

        return new SessionConfigPairResult(
            output,
            Path.Combine(output, HostFileName),
            Path.Combine(output, GuestFileName));
    }

    private static void DeleteOwnedStagingDirectory(
        string staging,
        string expectedParent)
    {
        var resolvedParent = Path.GetFullPath(expectedParent).TrimEnd(
            Path.DirectorySeparatorChar,
            Path.AltDirectorySeparatorChar);
        var resolvedStaging = Path.GetFullPath(staging);
        if (!string.Equals(
                Path.GetDirectoryName(resolvedStaging)?.TrimEnd(
                    Path.DirectorySeparatorChar,
                    Path.AltDirectorySeparatorChar),
                resolvedParent,
                StringComparison.OrdinalIgnoreCase) ||
            !Path.GetFileName(resolvedStaging).StartsWith(
                ".coopstory-session-",
                StringComparison.Ordinal) ||
            !Path.GetFileName(resolvedStaging).EndsWith(
                ".tmp",
                StringComparison.Ordinal))
        {
            return;
        }

        if (Directory.Exists(resolvedStaging))
        {
            Directory.Delete(resolvedStaging, recursive: true);
        }
    }
}
