using System.Security.Principal;

namespace CoopStory.Sidecar.Ipc;

// Names a Windows local pipe per user SID.
// The native bridge independently builds the same suffix, preventing another Windows account from joining it.
public static class PipeNameResolver
{
    public static string ResolveForCurrentUser(string baseName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(baseName);
        if (baseName.IndexOfAny(['\\', '/', ':']) >= 0)
        {
            throw new ArgumentException("Pipe base name contains invalid characters.", nameof(baseName));
        }

        // Query only the current token identity; no elevation or cross-user inspection is required to build the sidecar/bridge rendezvous name.
        using var identity = WindowsIdentity.GetCurrent(TokenAccessLevels.Query);
        var sid = identity.User?.Value
            ?? throw new InvalidOperationException("Cannot determine the current Windows user SID.");
        return $"{baseName}.{sid}";
    }
}
