namespace CoopStory.Sidecar.Session;

/// <summary>
/// Serializes reliable peer control operations. Callers may keep cache
/// mutation, snapshot capture and a multi-frame send in one atomic lane.
/// </summary>
internal sealed class PeerControlSendGate
{
    private readonly SemaphoreSlim _gate = new(1, 1);

    public async ValueTask<T> RunAsync<T>(
        Func<CancellationToken, ValueTask<T>> operation,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(operation);
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
