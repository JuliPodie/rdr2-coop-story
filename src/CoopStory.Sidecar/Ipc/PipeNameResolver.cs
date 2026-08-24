using System.Security.Principal;

namespace CoopStory.Sidecar.Ipc;

public static class PipeNameResolver
{
    public static string ResolveForCurrentUser(string baseName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(baseName);
        if (baseName.IndexOfAny(['\\', '/', ':']) >= 0)
        {
            throw new ArgumentException("Pipe base name contains invalid characters.", nameof(baseName));
        }

        using var identity = WindowsIdentity.GetCurrent(TokenAccessLevels.Query);
        var sid = identity.User?.Value
            ?? throw new InvalidOperationException("Cannot determine the current Windows user SID.");
        return $"{baseName}.{sid}";
    }
}
