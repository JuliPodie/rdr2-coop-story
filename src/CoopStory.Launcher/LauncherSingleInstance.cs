using System.Security.Principal;

namespace CoopStory.Launcher;

public sealed class LauncherSingleInstance : IDisposable
{
    private readonly Mutex _mutex;
    private bool _owned;

    private LauncherSingleInstance(Mutex mutex)
    {
        _mutex = mutex;
        _owned = true;
    }

    public static LauncherSingleInstance? TryAcquire(
        string applicationId = "RDR2CoopStory.Launcher")
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(applicationId);
        var userIdentity = WindowsIdentity.GetCurrent().User?.Value;
        if (string.IsNullOrWhiteSpace(userIdentity))
        {
            userIdentity = $"{Environment.UserDomainName}\\{Environment.UserName}";
        }

        var scopeHash = Hashing.TextSha256(
            $"{applicationId}\n{userIdentity}")[..32];
        var mutex = new Mutex(
            initiallyOwned: true,
            name: $"Local\\RDR2CoopStory.Launcher.{scopeHash}",
            createdNew: out var createdNew);
        if (!createdNew)
        {
            mutex.Dispose();
            return null;
        }

        return new LauncherSingleInstance(mutex);
    }

    public void Dispose()
    {
        if (!_owned)
        {
            return;
        }

        _owned = false;
        try
        {
            _mutex.ReleaseMutex();
        }
        finally
        {
            _mutex.Dispose();
        }
    }
}
