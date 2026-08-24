using System.Net;
using System.Net.Sockets;
using CoopStory.Protocol;

namespace CoopStory.Sidecar.Networking;

internal sealed class ControlConnection : IAsyncDisposable
{
    private readonly SemaphoreSlim _sendGate = new(1, 1);
    private readonly TcpClient _client;
    private readonly NetworkStream _stream;
    private bool _disposed;

    public ControlConnection(TcpClient client)
    {
        _client = client ?? throw new ArgumentNullException(nameof(client));
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
        var envelope = await ProtocolCodec.ReadAsync(_stream, cancellationToken)
            .ConfigureAwait(false);
        if (envelope is not null)
        {
            LastReceivedTimestamp = Environment.TickCount64;
        }

        return envelope;
    }

    public async ValueTask SendAsync(
        ProtocolEnvelope envelope,
        CancellationToken cancellationToken)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
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
        _stream.Dispose();
        _client.Dispose();
        _sendGate.Dispose();
        return ValueTask.CompletedTask;
    }
}
