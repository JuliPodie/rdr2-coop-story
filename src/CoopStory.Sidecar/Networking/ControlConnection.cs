using System.Net;
using System.Net.Sockets;
using CoopStory.Protocol;

namespace CoopStory.Sidecar.Networking;

// Owns one reliable TCP connection.
// TCP carries session control and important entity lifecycle messages, while fast-changing snapshots use UDP elsewhere.
internal sealed class ControlConnection : IAsyncDisposable
{
    // NetworkStream cannot safely interleave two frame writes, so all callers must take this gate before ProtocolCodec writes a complete frame.
    private readonly SemaphoreSlim _sendGate = new(1, 1);
    private readonly TcpClient _client;
    private readonly NetworkStream _stream;
    private bool _disposed;

    public ControlConnection(TcpClient client)
    {
        _client = client ?? throw new ArgumentNullException(nameof(client));
        // Disable Nagle buffering: control frames should leave immediately, rather than waiting for another small TCP write to accompany them.
        _client.NoDelay = true;
        RemoteEndPoint = _client.Client.RemoteEndPoint as IPEndPoint
            ?? throw new SocketException((int)SocketError.NotConnected);
        _stream = client.GetStream();
        LastReceivedTimestamp = Environment.TickCount64;
    }

    public IPEndPoint RemoteEndPoint { get; }

    public long LastReceivedTimestamp { get; private set; }

    public async ValueTask<ProtocolEnvelope?> ReceiveAsync(
        CancellationToken cancellationToken)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        // ReadAsync waits for one whole framed protocol message, handling TCP's normal partial reads before the rest of the sidecar sees the payload.
        var envelope = await ProtocolCodec.ReadAsync(_stream, cancellationToken)
            .ConfigureAwait(false);
        if (envelope is not null)
        {
            // The session watchdog uses this timestamp to distinguish a live quiet connection from a peer that has stopped sending entirely.
            LastReceivedTimestamp = Environment.TickCount64;
        }

        return envelope;
    }

    public async ValueTask SendAsync(
        ProtocolEnvelope envelope,
        CancellationToken cancellationToken)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        // Serialize the header and payload together so another async sender cannot put its own bytes in the middle of this message.
        await _sendGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await ProtocolCodec.WriteAsync(_stream, envelope, cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            _sendGate.Release();
        }
    }

    public ValueTask DisposeAsync()
    {
        if (_disposed)
        {
            return ValueTask.CompletedTask;
        }

        _disposed = true;
        // Closing the stream also releases a blocked read on the session loop.
        _stream.Dispose();
        _client.Dispose();
        _sendGate.Dispose();
        return ValueTask.CompletedTask;
    }
}
