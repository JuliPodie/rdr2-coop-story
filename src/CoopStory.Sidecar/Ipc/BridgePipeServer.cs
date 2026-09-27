using System.IO.Pipes;
using CoopStory.Protocol;

namespace CoopStory.Sidecar.Ipc;

// A monotonically changing capability for one native-bridge pipe connection.
// It prevents delayed work from an old game session being sent to a new one.
public readonly record struct BridgePipeConnectionToken(long Generation)
{
    public bool IsValid => Generation != 0;
}

public delegate ValueTask BridgePipeEnvelopeReceivedHandler(
    ProtocolEnvelope envelope,
    BridgePipeConnectionToken receiveConnection,
    CancellationToken cancellationToken);

// Hosts the local named pipe between the external C# sidecar and the in-game native bridge.
// It is local IPC, not a multiplayer network listener.
public sealed class BridgePipeServer : IAsyncDisposable
{
    private readonly object _connectionSync = new();
    private readonly SemaphoreSlim _sendGate = new(1, 1);
    private readonly CancellationTokenSource _stop = new();
    private NamedPipeServerStream? _connection;
    private long _nextConnectionGeneration;
    private long _connectionGeneration;
    private long _activeSendGeneration;
    private long _activeSendStartedTimestamp;
    private bool _disposed;

    public BridgePipeServer(string baseName)
    {
        // Add the Windows user identity so another local user cannot connect to a predictable global pipe name.
        PipeName = PipeNameResolver.ResolveForCurrentUser(baseName);
    }

    public string PipeName { get; }

    public event BridgePipeEnvelopeReceivedHandler? MessageReceived;

    public event Func<Exception, ValueTask>? ConnectionFaulted;

    public event Func<ValueTask>? ConnectionOpened;

    public event Func<ValueTask>? ConnectionClosed;

    public async Task RunAsync(CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            _stop.Token);

        // A game reload recreates the client pipe; keep accepting replacements until the sidecar itself is stopped.
        while (!linked.IsCancellationRequested)
        {
            await using var pipe = CreatePipe();
            var opened = false;
            try
            {
                await pipe.WaitForConnectionAsync(linked.Token).ConfigureAwait(false);
                lock (_connectionSync)
                {
                    _connection = pipe;
                    // A new pipe is a new authority boundary for queued sends and in-flight callbacks from the previous bridge.
                    _connectionGeneration = NextConnectionGeneration();
                }
                opened = true;
                if (ConnectionOpened is { } openedHandler)
                {
                    await openedHandler().ConfigureAwait(false);
                }

                await ReceiveLoopAsync(pipe, linked.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (linked.IsCancellationRequested)
            {
                break;
            }
            catch (Exception exception) when (
                linked.IsCancellationRequested &&
                exception is IOException or ObjectDisposedException)
            {
                break;
            }
            catch (Exception exception) when (
                exception is IOException or
                    ProtocolException or
                    UnauthorizedAccessException or
                    ObjectDisposedException)
            {
                // Ordinary game unloads and pipe breaks land here.
                // Surface them to the session coordinator, then return to accepting a pipe.
                if (ConnectionFaulted is { } faulted)
                {
                    await faulted(exception).ConfigureAwait(false);
                }
            }
            finally
            {
                lock (_connectionSync)
                {
                    if (ReferenceEquals(_connection, pipe))
                    {
                        _connection = null;
                        _connectionGeneration = 0;
                    }
                }

                if (opened && ConnectionClosed is { } closedHandler)
                {
                    await closedHandler().ConfigureAwait(false);
                }
            }
        }
    }

    public bool TryCaptureConnection(
        out BridgePipeConnectionToken connectionToken)
    {
        lock (_connectionSync)
        {
            if (_connection is null ||
                !_connection.IsConnected ||
                _connectionGeneration == 0)
            {
                connectionToken = default;
                return false;
            }

            // Capture the generation together with the connection; callers use it later to ensure their message still targets this game session.
            connectionToken = new BridgePipeConnectionToken(
                _connectionGeneration);
            return true;
        }
    }

    public bool IsConnectionCurrent(
        BridgePipeConnectionToken connectionToken)
    {
        if (!connectionToken.IsValid)
        {
            return false;
        }

        lock (_connectionSync)
        {
            return _connection is not null &&
                _connection.IsConnected &&
                _connectionGeneration == connectionToken.Generation;
        }
    }

    /// <summary>
    /// Invalidates every token captured for the current pipe without closing the pipe.
    /// The rotation is serialized with complete frame writes, so a frame that already started finishes before the authority boundary and a queued sender holding the previous token fails after it.
    /// </summary>
    public async ValueTask<BridgePipeConnectionToken?>
        RotateConnectionTokenAsync(
            BridgePipeConnectionToken expectedConnection,
            CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (!expectedConnection.IsValid)
        {
            return null;
        }
        // Rotation waits for any complete write so an old and new logical session cannot share a partially written protocol frame.
        await _sendGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            lock (_connectionSync)
            {
                if (_connection is null ||
                    !_connection.IsConnected ||
                    _connectionGeneration != expectedConnection.Generation)
                {
                    return null;
                }

                _connectionGeneration = NextConnectionGeneration();
                return new BridgePipeConnectionToken(
                    _connectionGeneration);
            }
        }
        finally
        {
            _sendGate.Release();
        }
    }

    public ValueTask<bool> SendAsync(
        ProtocolEnvelope envelope,
        CancellationToken cancellationToken = default) =>
        SendAsyncCore(
            expectedConnection: null,
            envelope,
            cancellationToken);

    public ValueTask<bool> SendAsync(
        BridgePipeConnectionToken expectedConnection,
        ProtocolEnvelope envelope,
        CancellationToken cancellationToken = default) =>
        SendAsyncCore(
            expectedConnection,
            envelope,
            cancellationToken);

    private async ValueTask<bool> SendAsyncCore(
        BridgePipeConnectionToken? expectedConnection,
        ProtocolEnvelope envelope,
        CancellationToken cancellationToken)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        // The pipe is a byte stream, so serialize full protocol-frame writes.
        await _sendGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            NamedPipeServerStream? connection;
            long generation;
            lock (_connectionSync)
            {
                connection = _connection;
                generation = _connectionGeneration;
                if (connection is not null &&
                    connection.IsConnected &&
                    generation != 0 &&
                    (!expectedConnection.HasValue ||
                        expectedConnection.Value.IsValid &&
                        expectedConnection.Value.Generation == generation))
                {
                    _activeSendGeneration = generation;
                    _activeSendStartedTimestamp =
                        Environment.TickCount64;
                }
                else
                {
                    connection = null;
                    generation = 0;
                }
            }

            // A disconnected/replaced bridge is not an error for the network layer; the caller can cache or drop this non-current delivery.
            if (connection is null || generation == 0)
            {
                return false;
            }

            try
            {
                // Once a frame starts, cancellation must not leave a partial frame on a connection that remains usable.
                // The caller token only cancels the wait for the serialized send gate.
                // A stuck write is released by aborting its owning pipe generation.
                await ProtocolCodec.WriteAsync(
                        connection,
                        envelope,
                        CancellationToken.None)
                    .ConfigureAwait(false);
                return true;
            }
            catch (OperationCanceledException)
            {
                return false;
            }
            catch (Exception exception) when (
                exception is IOException or ObjectDisposedException)
            {
                return false;
            }
            finally
            {
                lock (_connectionSync)
                {
                    if (_activeSendGeneration == generation)
                    {
                        _activeSendGeneration = 0;
                        _activeSendStartedTimestamp = 0;
                    }
                }
            }
        }
        finally
        {
            _sendGate.Release();
        }
    }

    public async ValueTask StopAsync()
    {
        // Tell the accept loop to stop before disposing the active stream.
        await _stop.CancelAsync().ConfigureAwait(false);
        _ = AbortCurrentConnection();
    }

    public bool AbortCurrentConnection()
    {
        NamedPipeServerStream? connection;
        lock (_connectionSync)
        {
            connection = _connection;
            _connection = null;
            _connectionGeneration = 0;
        }

        // Closing the connection releases an un-cancelled full-frame write.
        // Any partial bytes belong to this discarded stream and cannot poison the next bridge connection.
        connection?.Dispose();
        return connection is not null;
    }

    private long NextConnectionGeneration()
    {
        // Keep zero reserved as the invalid/default token, even after overflow.
        var generation = unchecked(++_nextConnectionGeneration);
        if (generation == 0)
        {
            generation = unchecked(++_nextConnectionGeneration);
        }
        return generation;
    }

    public bool AbortStalledSend(
        long minimumActiveMilliseconds,
        out long generation,
        out long activeMilliseconds)
    {
        // This watchdog is deliberately explicit: a stuck game process must lose this pipe rather than block all later sidecar-to-game traffic.
        if (minimumActiveMilliseconds <= 0)
        {
            throw new ArgumentOutOfRangeException(
                nameof(minimumActiveMilliseconds));
        }

        NamedPipeServerStream? connection;
        lock (_connectionSync)
        {
            generation = _activeSendGeneration;
            activeMilliseconds =
                generation == 0 ||
                _activeSendStartedTimestamp == 0
                    ? 0
                    : Math.Max(
                        0,
                        Environment.TickCount64 -
                        _activeSendStartedTimestamp);
            if (generation == 0 ||
                activeMilliseconds < minimumActiveMilliseconds ||
                generation != _connectionGeneration ||
                _connection is null)
            {
                return false;
            }

            connection = _connection;
            _connection = null;
            _connectionGeneration = 0;
        }

        // Close only the pipe generation that owns the stalled write.
        // A new connection cannot be aborted while an older SendAsync unwinds.
        connection.Dispose();
        return true;
    }

    public async ValueTask DisposeAsync()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        await StopAsync().ConfigureAwait(false);

        _stop.Dispose();
    }

    private NamedPipeServerStream CreatePipe() =>
        new(
            pipeName: PipeName,
            direction: PipeDirection.InOut,
            maxNumberOfServerInstances: 1,
            transmissionMode: PipeTransmissionMode.Byte,
            options: PipeOptions.Asynchronous |
                     PipeOptions.WriteThrough |
                     PipeOptions.CurrentUserOnly,
            inBufferSize: 64 * 1024,
            // Network snapshots are already coalesced before this pipe.
            // A small output quota limits the amount of stale state Windows can buffer while the game script is paused or loading.
            outBufferSize: 4 * 1024);

    private async Task ReceiveLoopAsync(
        NamedPipeServerStream pipe,
        CancellationToken cancellationToken)
    {
        while (pipe.IsConnected && !cancellationToken.IsCancellationRequested)
        {
            // Capture before ReadAsync so a logical rotation that happens while this read is pending cannot relabel an already in-flight frame as belonging to the replacement session.
            // Later reads capture the new generation.
            // Hello/session-menu frames remain explicit boundaries; the native bridge clears its role before it emits Hello and does not publish gameplay state again until the matching HelloAck.
            BridgePipeConnectionToken receiveConnection;
            lock (_connectionSync)
            {
                if (!ReferenceEquals(_connection, pipe) ||
                    _connectionGeneration == 0)
                {
                    return;
                }
                receiveConnection = new BridgePipeConnectionToken(
                    _connectionGeneration);
            }
            var envelope = await ProtocolCodec.ReadAsync(pipe, cancellationToken)
                .ConfigureAwait(false);
            if (envelope is null)
            {
                return;
            }

            if (MessageReceived is { } received)
            {
                await received(
                        envelope,
                        receiveConnection,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
        }
    }
}
