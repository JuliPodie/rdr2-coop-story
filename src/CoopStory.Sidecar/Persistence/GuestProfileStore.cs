using System.Text.Json;
using CoopStory.Protocol;

namespace CoopStory.Sidecar.Persistence;

// Reads/writes the guest's local profile with a backup fallback.
// This protects local save-like data from a failed write; it is not a LAN synchronization path.
public sealed class GuestProfileStore : IDisposable
{
    private readonly SemaphoreSlim _gate = new(1, 1);
    private bool _disposed;

    public async Task<GuestProfileLoadResult> LoadAsync(
        string path,
        CancellationToken cancellationToken = default)
    {
        ThrowIfDisposed();
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        var fullPath = Path.GetFullPath(path);

        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var primaryError = default(Exception);
            try
            {
                var primary = await ReadAsync(fullPath, cancellationToken).ConfigureAwait(false);
                return new GuestProfileLoadResult(primary, RecoveredFromBackup: false);
            }
            catch (Exception exception) when (
                exception is IOException or JsonException or GuestProfileException)
            {
                primaryError = exception;
            }

            var backupPath = GetBackupPath(fullPath);
            try
            {
                var backup = await ReadAsync(backupPath, cancellationToken).ConfigureAwait(false);
                return new GuestProfileLoadResult(backup, RecoveredFromBackup: true);
            }
            catch (Exception backupError) when (
                backupError is IOException or JsonException or GuestProfileException)
            {
                throw new GuestProfileException(
                    $"Neither guest profile '{fullPath}' nor its backup could be loaded.",
                    new AggregateException(primaryError, backupError));
            }
        }
        finally
        {
            _gate.Release();
        }
    }

    public async Task SaveAsync(
        string path,
        GuestProfile profile,
        CancellationToken cancellationToken = default)
    {
        ThrowIfDisposed();
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentNullException.ThrowIfNull(profile);
        profile.Validate();
        var updated = profile with
        {
            UpdatedAtUnixMilliseconds = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()
        };

        var fullPath = Path.GetFullPath(path);
        var directory = Path.GetDirectoryName(fullPath)
            ?? throw new GuestProfileException("Guest profile path has no parent directory.");
        var temporaryPath = Path.Combine(
            directory,
            $".{Path.GetFileName(fullPath)}.{Guid.NewGuid():N}.tmp");

        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            Directory.CreateDirectory(directory);
            try
            {
                await WriteTemporaryAsync(
                    temporaryPath,
                    updated,
                    cancellationToken).ConfigureAwait(false);

                if (File.Exists(fullPath))
                {
                    File.Replace(
                        temporaryPath,
                        fullPath,
                        GetBackupPath(fullPath),
                        ignoreMetadataErrors: true);
                }
                else
                {
                    File.Move(temporaryPath, fullPath);
                }
            }
            finally
            {
                if (File.Exists(temporaryPath))
                {
                    File.Delete(temporaryPath);
                }
            }
        }
        finally
        {
            _gate.Release();
        }
    }

    public static string GetBackupPath(string path) =>
        $"{Path.GetFullPath(path)}.bak";

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _gate.Dispose();
    }

    private static async Task<GuestProfile> ReadAsync(
        string path,
        CancellationToken cancellationToken)
    {
        await using var stream = new FileStream(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            16 * 1024,
            FileOptions.Asynchronous | FileOptions.SequentialScan);
        var profile = await JsonSerializer.DeserializeAsync<GuestProfile>(
            stream,
            PayloadJson.Options,
            cancellationToken).ConfigureAwait(false);
        return (profile ?? throw new GuestProfileException(
            $"Guest profile '{path}' contains JSON null.")).Validate();
    }

    private static async Task WriteTemporaryAsync(
        string path,
        GuestProfile profile,
        CancellationToken cancellationToken)
    {
        await using var stream = new FileStream(
            path,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.None,
            16 * 1024,
            FileOptions.Asynchronous | FileOptions.WriteThrough);
        await JsonSerializer.SerializeAsync(
            stream,
            profile,
            PayloadJson.Options,
            cancellationToken).ConfigureAwait(false);
        await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
        stream.Flush(flushToDisk: true);
    }

    private void ThrowIfDisposed() =>
        ObjectDisposedException.ThrowIf(_disposed, this);
}
