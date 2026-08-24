using System.Text.Json;

namespace CoopStory.Launcher;

public sealed class LauncherLogger : IDisposable
{
    private static readonly JsonSerializerOptions CompactJsonOptions = new(
        JsonSupport.Options)
    {
        WriteIndented = false
    };

    private readonly object _gate = new();
    private readonly FileStream _stream;
    private readonly StreamWriter _writer;
    private bool _disposed;

    public LauncherLogger(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        var fullPath = Path.GetFullPath(path);
        Directory.CreateDirectory(
            Path.GetDirectoryName(fullPath)
                ?? throw new LauncherException("The log path has no directory."));
        _stream = new FileStream(
            fullPath,
            FileMode.Append,
            FileAccess.Write,
            FileShare.ReadWrite | FileShare.Delete,
            16 * 1024,
            FileOptions.WriteThrough);
        _writer = new StreamWriter(_stream, new System.Text.UTF8Encoding(false))
        {
            AutoFlush = true
        };
    }

    public event EventHandler<string>? LineWritten;

    public void Info(string eventName, string message) =>
        Write("info", eventName, message, null);

    public void Warning(string eventName, string message) =>
        Write("warning", eventName, message, null);

    public void Error(string eventName, Exception exception) =>
        Write("error", eventName, exception.Message, exception.ToString());

    public void WriteSidecarLine(string stream, string line)
    {
        if (string.IsNullOrWhiteSpace(line))
        {
            return;
        }

        Write("info", $"sidecar.{stream}", line, null);
    }

    public void Dispose()
    {
        lock (_gate)
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;
            _writer.Dispose();
            _stream.Dispose();
        }
    }

    private void Write(
        string level,
        string eventName,
        string message,
        string? details)
    {
        var record = new
        {
            timestampUtc = DateTimeOffset.UtcNow,
            level,
            eventName,
            message,
            details,
            processId = Environment.ProcessId,
            machine = Environment.MachineName
        };
        var line = JsonSerializer.Serialize(record, CompactJsonOptions);
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            _writer.WriteLine(line);
            _stream.Flush(flushToDisk: true);
        }

        var notification = $"[{level.ToUpperInvariant()}] {message}";
        foreach (var subscriber in LineWritten?
                     .GetInvocationList()
                     .Cast<EventHandler<string>>() ?? [])
        {
            try
            {
                subscriber(this, notification);
            }
            catch
            {
                // A UI subscriber must never be able to break durable logging.
            }
        }
    }
}
