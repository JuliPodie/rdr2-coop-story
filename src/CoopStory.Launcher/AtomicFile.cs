using System.Text;
using System.Text.Json;

namespace CoopStory.Launcher;

public static class AtomicFile
{
    private static readonly UTF8Encoding Utf8WithoutBom = new(false);

    public static void WriteJson<T>(string path, T value, JsonSerializerOptions options)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentNullException.ThrowIfNull(value);
        ArgumentNullException.ThrowIfNull(options);
        WriteBytes(path, JsonSerializer.SerializeToUtf8Bytes(value, options));
    }

    public static void WriteText(string path, string value)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentNullException.ThrowIfNull(value);
        WriteBytes(path, Utf8WithoutBom.GetBytes(value));
    }

    public static void WriteBytes(string path, ReadOnlySpan<byte> bytes)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        var target = Path.GetFullPath(path);
        var parent = Path.GetDirectoryName(target)
            ?? throw new LauncherException("Ścieżka pliku nie ma katalogu nadrzędnego.");
        Directory.CreateDirectory(parent);

        var temporary = Path.Combine(
            parent,
            $".{Path.GetFileName(target)}.{Guid.NewGuid():N}.tmp");
        try
        {
            using (var stream = new FileStream(
                       temporary,
                       FileMode.CreateNew,
                       FileAccess.Write,
                       FileShare.None,
                       16 * 1024,
                       FileOptions.WriteThrough))
            {
                stream.Write(bytes);
                stream.Flush(flushToDisk: true);
            }

            File.Move(temporary, target, overwrite: true);
        }
        finally
        {
            if (File.Exists(temporary))
            {
                File.Delete(temporary);
            }
        }
    }
}
