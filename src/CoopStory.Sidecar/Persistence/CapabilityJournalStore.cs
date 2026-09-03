using System.Text.Json;
using CoopStory.Protocol;
using CoopStory.Sidecar.Session;

namespace CoopStory.Sidecar.Persistence;

// Saves host-approved capability events with a backup copy.
// This lets a guest remember it applied an unlock even if the session disconnects right away.
public sealed class CapabilityJournalStore
{
    public async Task SaveAsync(string path, CapabilityJournal journal, CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentNullException.ThrowIfNull(journal);
        var full = Path.GetFullPath(path);
        Directory.CreateDirectory(Path.GetDirectoryName(full) ?? throw new IOException("Capability journal has no parent directory."));
        var temporary = full + ".tmp";
        await using (var stream = new FileStream(temporary, FileMode.Create, FileAccess.Write, FileShare.None, 16 * 1024, FileOptions.WriteThrough))
        {
            await JsonSerializer.SerializeAsync(stream, journal.CaptureState(), PayloadJson.Options, cancellationToken).ConfigureAwait(false);
            await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
            stream.Flush(true);
        }
        if (File.Exists(full)) File.Replace(temporary, full, full + ".bak", true);
        else File.Move(temporary, full);
    }

    public async Task<CapabilityJournal> LoadAsync(string path, CancellationToken cancellationToken = default)
    {
        var full = Path.GetFullPath(path);
        if (!File.Exists(full) && !File.Exists(full + ".bak"))
        {
            return new CapabilityJournal();
        }
        IReadOnlyList<CapabilityGrant> grants;
        try
        {
            grants = await ReadAsync(full, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception primary) when (primary is IOException or JsonException or ArgumentException)
        {
            grants = await ReadAsync(full + ".bak", cancellationToken).ConfigureAwait(false);
        }
        var journal = new CapabilityJournal();
        journal.Restore(grants);
        return journal;
    }

    private static async Task<IReadOnlyList<CapabilityGrant>> ReadAsync(string path, CancellationToken cancellationToken)
    {
        await using var stream = File.OpenRead(path);
        return await JsonSerializer.DeserializeAsync<List<CapabilityGrant>>(stream, PayloadJson.Options, cancellationToken).ConfigureAwait(false)
            ?? throw new InvalidDataException("Capability journal contains JSON null.");
    }
}
