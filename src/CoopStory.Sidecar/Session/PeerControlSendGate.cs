namespace CoopStory.Sidecar.Session;

/// <summary>
/// Serializes reliable peer control operations.
/// Callers may keep cache mutation, snapshot capture and a multi-frame send in one atomic lane.
/// </summary>
internal sealed class PeerControlSendGate
{
    private readonly SemaphoreSlim _gate = new(1, 1);

    public async ValueTask<T> RunAsync<T>(
        Func<CancellationToken, ValueTask<T>> operation,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(operation);
        // Keep cache changes and related multi-frame TCP sends in their chosen order.
        // This prevents a resync snapshot interleaving a normal spawn.
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            return await operation(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            _gate.Release();
        }
    }
}
