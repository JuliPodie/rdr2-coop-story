using System.Text;
using System.Text.Json;
using CoopStory.Protocol;

namespace CoopStory.Sidecar.Diagnostics;

public sealed class JsonLineLogger : IAsyncDisposable
{
    private const long MaximumSegmentBytes = 8L * 1024L * 1024L;
    private const int ArchiveSegments = 3;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private readonly FileStream _stream;
    private readonly StreamWriter _writer;
    private bool _disposed;

    public JsonLineLogger(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        var fullPath = Path.GetFullPath(path);
        Directory.CreateDirectory(
            Path.GetDirectoryName(fullPath)
                ?? throw new ArgumentException("Log path has no parent directory.", nameof(path)));
        RotateAtSessionStart(fullPath);
        _stream = new FileStream(
            fullPath,
            FileMode.Append,
            FileAccess.Write,
            FileShare.Read,
            16 * 1024,
            FileOptions.Asynchronous | FileOptions.WriteThrough);
        _writer = new StreamWriter(_stream, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false))
        {
            AutoFlush = true
        };
    }

    internal static void RotateAtSessionStart(
        string path,
        long maximumSegmentBytes = MaximumSegmentBytes,
        int archiveSegments = ArchiveSegments)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        if (maximumSegmentBytes < 1)
        {
            throw new ArgumentOutOfRangeException(nameof(maximumSegmentBytes));
        }
        if (archiveSegments < 1)
        {
            throw new ArgumentOutOfRangeException(nameof(archiveSegments));
        }
        if (!File.Exists(path) ||
            new FileInfo(path).Length <= maximumSegmentBytes)
        {
            return;
        }

        var oldest = $"{path}.{archiveSegments}";
        if (File.Exists(oldest))
        {
            File.Delete(oldest);
        }
        for (var index = archiveSegments; index > 1; index--)
        {
            var previous = $"{path}.{index - 1}";
            if (File.Exists(previous))
            {
                File.Move(previous, $"{path}.{index}");
            }
        }
        File.Move(path, $"{path}.1");
    }

    public ValueTask InfoAsync(
        string eventName,
        string message,
        IReadOnlyDictionary<string, object?>? data = null,
        CancellationToken cancellationToken = default) =>
        WriteAsync("info", eventName, message, data, cancellationToken);

    public ValueTask WarningAsync(
        string eventName,
        string message,
        IReadOnlyDictionary<string, object?>? data = null,
        CancellationToken cancellationToken = default) =>
        WriteAsync("warning", eventName, message, data, cancellationToken);

    public ValueTask ErrorAsync(
        string eventName,
        string message,
        Exception? exception = null,
        CancellationToken cancellationToken = default)
    {
        Dictionary<string, object?>? data = null;
        if (exception is not null)
        {
            data = new Dictionary<string, object?>
            {
                ["exceptionType"] = exception.GetType().FullName,
                ["exceptionMessage"] = exception.Message
            };
        }

        return WriteAsync("error", eventName, message, data, cancellationToken);
    }

    public async ValueTask DisposeAsync()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        await _gate.WaitAsync().ConfigureAwait(false);
        try
        {
            await _writer.DisposeAsync().ConfigureAwait(false);
            await _stream.DisposeAsync().ConfigureAwait(false);
        }
        finally
        {
            _gate.Release();
            _gate.Dispose();
        }
    }

    private async ValueTask WriteAsync(
        string level,
        string eventName,
        string message,
        IReadOnlyDictionary<string, object?>? data,
        CancellationToken cancellationToken)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentException.ThrowIfNullOrWhiteSpace(eventName);
        ArgumentNullException.ThrowIfNull(message);

        var entry = new LogEntry(
            DateTimeOffset.UtcNow,
            level,
            eventName,
            message,
            data);
        var line = SecretRedactor.Redact(
            JsonSerializer.Serialize(entry, PayloadJson.Options));

        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await _writer.WriteLineAsync(line.AsMemory(), cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            _gate.Release();
        }
    }

    private sealed record LogEntry(
        DateTimeOffset Timestamp,
        string Level,
        string Event,
        string Message,
        IReadOnlyDictionary<string, object?>? Data);
}
