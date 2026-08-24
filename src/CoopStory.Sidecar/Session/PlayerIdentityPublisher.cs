using CoopStory.Protocol;
using CoopStory.Sidecar.Networking;

namespace CoopStory.Sidecar.Session;

internal enum IdentityPublishResult
{
    NotReady,
    NotDue,
    Delivered,
    Failed
}

internal sealed class PlayerIdentityPublisher
{
    internal const long RefreshIntervalMilliseconds = 5_000;

    private readonly object _sync = new();
    private readonly SemaphoreSlim _sendGate = new(1, 1);
    private readonly string _nickname;
    private readonly Func<long> _clock;
    private PlayerIdentityPayload? _identity;
    private long _nextRefreshAtMilliseconds;

    public PlayerIdentityPublisher(
        string nickname,
        Func<long>? clock = null)
    {
        _nickname = PlayerIdentityRules.ValidateNickname(nickname);
        _clock = clock ?? (static () => Environment.TickCount64);
    }

    public void ObservePlayerState(PlayerStatePayload state)
    {
        if (!state.EntityId.IsValid ||
            state.Slot > (byte)SessionRole.Guest)
        {
            throw new ProtocolException(
                "Cannot publish identity for an invalid local player state.");
        }

        var observed = new PlayerIdentityPayload(
            state.EntityId,
            state.Slot,
            _nickname);
        lock (_sync)
        {
            if (_identity == observed)
            {
                return;
            }

            _identity = observed;
            _nextRefreshAtMilliseconds = 0;
        }
    }

    public async ValueTask<IdentityPublishResult> PublishIfDueAsync(
        ILanSession network,
        ulong tick,
        bool force,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(network);
        return await PublishIfDueAsync(
                () => network.IsConnected,
                (payload, sendTick, token) => network.SendControlAsync(
                    MessageType.PlayerIdentity,
                    payload,
                    sendTick,
                    token),
                tick,
                force,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask<IdentityPublishResult> PublishIfDueAsync(
        Func<bool> isPeerConnected,
        Func<ReadOnlyMemory<byte>, ulong, CancellationToken, ValueTask<bool>>
            send,
        ulong tick,
        bool force,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(isPeerConnected);
        ArgumentNullException.ThrowIfNull(send);
        await _sendGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            PlayerIdentityPayload identity;
            var now = _clock();
            lock (_sync)
            {
                if (!_identity.HasValue || !isPeerConnected())
                {
                    return IdentityPublishResult.NotReady;
                }

                if (!force &&
                    now >= 0 &&
                    now < _nextRefreshAtMilliseconds)
                {
                    return IdentityPublishResult.NotDue;
                }

                identity = _identity.Value;
            }

            var delivered = await send(
                BinaryPayloadCodec.EncodePlayerIdentity(identity),
                tick,
                cancellationToken).ConfigureAwait(false);
            if (!delivered)
            {
                return IdentityPublishResult.Failed;
            }

            lock (_sync)
            {
                if (_identity == identity)
                {
                    _nextRefreshAtMilliseconds =
                        now + RefreshIntervalMilliseconds;
                }
            }
            return IdentityPublishResult.Delivered;
        }
        finally
        {
            _sendGate.Release();
        }
    }
}
