using System.Text.Json;
using CoopStory.Protocol;
using CoopStory.Sidecar.Session;

namespace CoopStory.Sidecar.Persistence;

public sealed class MissionProgressionJournalStore
{
    public async Task SaveAsync(
        string path,
        MissionProgressionJournal journal,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentNullException.ThrowIfNull(journal);
        var full = Path.GetFullPath(path);
        Directory.CreateDirectory(Path.GetDirectoryName(full) ??
            throw new IOException("Mission progression journal has no parent directory."));
        var temporary = full + ".tmp";
        await using (var stream = new FileStream(
            temporary, FileMode.Create, FileAccess.Write, FileShare.None,
            16 * 1024, FileOptions.WriteThrough))
        {
            await JsonSerializer.SerializeAsync(
                stream, journal.CaptureState(), PayloadJson.Options,
                cancellationToken).ConfigureAwait(false);
            await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
            stream.Flush(true);
        }
        if (File.Exists(full)) File.Replace(temporary, full, full + ".bak", true);
        else File.Move(temporary, full);
    }

    public async Task<MissionProgressionJournal> LoadAsync(
        string path,
        CancellationToken cancellationToken = default)
    {
        var full = Path.GetFullPath(path);
        if (!File.Exists(full) && !File.Exists(full + ".bak"))
        {
            return new MissionProgressionJournal();
        }
        IReadOnlyList<MissionProgressionCompletionRecord> records;
        try
        {
            records = await ReadAsync(full, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception primary) when (primary is IOException or JsonException or ArgumentException)
        {
            records = await ReadAsync(full + ".bak", cancellationToken).ConfigureAwait(false);
        }
        var journal = new MissionProgressionJournal();
        journal.Restore(records);
        return journal;
    }

    private static async Task<IReadOnlyList<MissionProgressionCompletionRecord>> ReadAsync(
        string path,
        CancellationToken cancellationToken)
    {
        await using var stream = File.OpenRead(path);
        return await JsonSerializer.DeserializeAsync<List<MissionProgressionCompletionRecord>>(
            stream, PayloadJson.Options, cancellationToken).ConfigureAwait(false) ??
            throw new InvalidDataException("Mission progression journal contains JSON null.");
    }
}
