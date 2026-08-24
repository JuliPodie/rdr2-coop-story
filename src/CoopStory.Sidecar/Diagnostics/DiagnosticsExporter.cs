using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using CoopStory.Protocol;
using CoopStory.Sidecar.Configuration;

namespace CoopStory.Sidecar.Diagnostics;

public sealed record DiagnosticsExportResult(
    string OutputPath,
    bool IncludedLog,
    long SourceLogBytes);

public static class DiagnosticsExporter
{
    private const long MaximumLogBytes = 16 * 1024 * 1024;

    public static async Task<DiagnosticsExportResult> ExportAsync(
        string configPath,
        string outputPath,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(configPath);
        ArgumentException.ThrowIfNullOrWhiteSpace(outputPath);
        var config = await SidecarConfigStore.LoadAsync(
            configPath,
            cancellationToken).ConfigureAwait(false);

        var output = Path.GetFullPath(outputPath);
        if (File.Exists(output) || Directory.Exists(output))
        {
            throw new ConfigurationException(
                "Diagnostics output path already exists; choose a new ZIP file.");
        }

        var parent = Path.GetDirectoryName(output)
            ?? throw new ConfigurationException(
                "Diagnostics output path has no parent directory.");
        Directory.CreateDirectory(parent);
        var staging = Path.Combine(
            parent,
            $".coopstory-diagnostics-{Guid.NewGuid():N}.tmp");

        var logPath = config.ExpandedLogPath;
        var includedLog = File.Exists(logPath);
        var sourceLogBytes = includedLog
            ? new FileInfo(logPath).Length
            : 0;

        try
        {
            await using (var stream = new FileStream(
                             staging,
                             FileMode.CreateNew,
                             FileAccess.ReadWrite,
                             FileShare.None,
                             64 * 1024,
                             FileOptions.Asynchronous | FileOptions.WriteThrough))
            {
                using var archive = new ZipArchive(
                    stream,
                    ZipArchiveMode.Create,
                    leaveOpen: true,
                    Encoding.UTF8);

                var summary = new
                {
                    generatedUtc = DateTimeOffset.UtcNow,
                    protocolVersion = ProtocolConstants.Version,
                    sidecarVersion =
                        typeof(DiagnosticsExporter).Assembly.GetName().Version?.ToString()
                        ?? "unknown",
                    role = config.Role.ToString(),
                    operatingSystem = RuntimeInformation.OSDescription,
                    processArchitecture = RuntimeInformation.ProcessArchitecture.ToString(),
                    framework = RuntimeInformation.FrameworkDescription,
                    logPresent = includedLog,
                    sourceLogBytes,
                    includedLogBytes = Math.Min(sourceLogBytes, MaximumLogBytes),
                    note =
                        "sessionToken is deliberately redacted from this archive."
                };
                await WriteTextEntryAsync(
                    archive,
                    "summary.json",
                    JsonSerializer.Serialize(summary, PayloadJson.Options),
                    cancellationToken).ConfigureAwait(false);

                var redactedConfig = config with
                {
                    SessionToken = SecretRedactor.Replacement
                };
                await WriteTextEntryAsync(
                    archive,
                    "config.redacted.json",
                    SecretRedactor.Redact(
                        JsonSerializer.Serialize(redactedConfig, PayloadJson.Options),
                        config.SessionToken),
                    cancellationToken).ConfigureAwait(false);

                if (includedLog)
                {
                    var log = await ReadLogTailAsync(
                        logPath,
                        cancellationToken).ConfigureAwait(false);
                    await WriteTextEntryAsync(
                        archive,
                        "sidecar.jsonl",
                        SecretRedactor.Redact(log, config.SessionToken),
                        cancellationToken).ConfigureAwait(false);
                }
                else
                {
                    await WriteTextEntryAsync(
                        archive,
                        "sidecar-log-missing.txt",
                        $"No sidecar log existed at export time ({DateTimeOffset.UtcNow:o}).",
                        cancellationToken).ConfigureAwait(false);
                }
            }

            File.Move(staging, output);
        }
        catch
        {
            DeleteOwnedStagingFile(staging, parent);
            throw;
        }

        return new DiagnosticsExportResult(
            output,
            includedLog,
            sourceLogBytes);
    }

    private static async Task<string> ReadLogTailAsync(
        string path,
        CancellationToken cancellationToken)
    {
        await using var stream = new FileStream(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.ReadWrite | FileShare.Delete,
            64 * 1024,
            FileOptions.Asynchronous | FileOptions.SequentialScan);
        var truncated = stream.Length > MaximumLogBytes;
        if (truncated)
        {
            stream.Seek(-MaximumLogBytes, SeekOrigin.End);
        }

        using var reader = new StreamReader(
            stream,
            new UTF8Encoding(
                encoderShouldEmitUTF8Identifier: false,
                throwOnInvalidBytes: false),
            detectEncodingFromByteOrderMarks: true,
            bufferSize: 64 * 1024,
            leaveOpen: true);
        var content = await reader.ReadToEndAsync(cancellationToken)
            .ConfigureAwait(false);
        if (!truncated)
        {
            return content;
        }

        var firstLineBreak = content.IndexOf('\n');
        if (firstLineBreak >= 0)
        {
            content = content[(firstLineBreak + 1)..];
        }

        return
            "{\"event\":\"diagnostics.log-truncated\",\"message\":" +
            "\"Only the newest 16 MiB are included.\"}\n" +
            content;
    }

    private static async Task WriteTextEntryAsync(
        ZipArchive archive,
        string name,
        string content,
        CancellationToken cancellationToken)
    {
        var entry = archive.CreateEntry(name, CompressionLevel.Optimal);
        await using var output = entry.Open();
        await using var writer = new StreamWriter(
            output,
            new UTF8Encoding(encoderShouldEmitUTF8Identifier: false),
            bufferSize: 16 * 1024,
            leaveOpen: false);
        await writer.WriteAsync(content.AsMemory(), cancellationToken)
            .ConfigureAwait(false);
    }

    private static void DeleteOwnedStagingFile(
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
                ".coopstory-diagnostics-",
                StringComparison.Ordinal) ||
            !Path.GetFileName(resolvedStaging).EndsWith(
                ".tmp",
                StringComparison.Ordinal))
        {
            return;
        }

        if (File.Exists(resolvedStaging))
        {
            File.Delete(resolvedStaging);
        }
    }
}
