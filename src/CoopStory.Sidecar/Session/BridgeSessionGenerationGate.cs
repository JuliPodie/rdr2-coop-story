using CoopStory.Sidecar.Ipc;

namespace CoopStory.Sidecar.Session;

/// <summary>
/// Serializes a logical game-pipe authority boundary with bridge-originated
/// work that may mutate reusable session state. An inbound frame is admitted
/// only while its captured receive generation is still the ready generation.
/// </summary>
internal sealed class BridgeSessionGenerationGate : IDisposable
{
    private sealed class Lease : IAsyncDisposable
    {
        private BridgeSessionGenerationGate? _owner;

        public Lease(BridgeSessionGenerationGate owner)
        {
            _owner = owner;
        }

        public ValueTask DisposeAsync()
        {
            Interlocked.Exchange(ref _owner, null)?.Release();
            return ValueTask.CompletedTask;
        }
    }

    private readonly SemaphoreSlim _gate = new(1, 1);
    private int _disposed;

    public async ValueTask<IAsyncDisposable> EnterBoundaryAsync(
        CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(
            Volatile.Read(ref _disposed) != 0,
            this);
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        return new Lease(this);
    }

    public async ValueTask<IAsyncDisposable?> TryEnterInboundAsync(
        BridgePipeConnectionToken receiveConnection,
        Func<BridgePipeConnectionToken, bool> isReadyConnection,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(isReadyConnection);
        ObjectDisposedException.ThrowIf(
            Volatile.Read(ref _disposed) != 0,
            this);
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (!isReadyConnection(receiveConnection))
            {
                _gate.Release();
                return null;
            }

            return new Lease(this);
        }
        catch
        {
            _gate.Release();
            throw;
        }
    }

    private void Release() => _gate.Release();

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
        {
            return;
        }

        _gate.Dispose();
    }
}
