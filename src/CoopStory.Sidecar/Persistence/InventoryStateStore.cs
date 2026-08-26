using System.Text.Json;
using CoopStory.Protocol;
using CoopStory.Sidecar.Session;

namespace CoopStory.Sidecar.Persistence;

/// <summary>Atomic persistence for host-authoritative inventory reconnect state.</summary>
public sealed class InventoryStateStore
{
    public async Task SaveAsync(string path, InventorySessionState state,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentNullException.ThrowIfNull(state);
        state.Validate();
        var fullPath = Path.GetFullPath(path);
        Directory.CreateDirectory(Path.GetDirectoryName(fullPath) ?? throw new IOException("Inventory path has no directory."));
        var temporary = fullPath + ".tmp";
        await using (var stream = new FileStream(temporary, FileMode.Create, FileAccess.Write, FileShare.None))
        {
            await JsonSerializer.SerializeAsync(stream, state, PayloadJson.Options, cancellationToken).ConfigureAwait(false);
            await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
            stream.Flush(true);
        }
        File.Move(temporary, fullPath, overwrite: true);
    }

    public async Task<InventorySessionState> LoadAsync(string path,
        CancellationToken cancellationToken = default)
    {
        await using var stream = File.OpenRead(Path.GetFullPath(path));
        var state = await JsonSerializer.DeserializeAsync<InventorySessionState>(stream, PayloadJson.Options, cancellationToken).ConfigureAwait(false)
            ?? throw new InvalidDataException("Inventory state contains JSON null.");
        state.Validate();
        return state;
    }
}
