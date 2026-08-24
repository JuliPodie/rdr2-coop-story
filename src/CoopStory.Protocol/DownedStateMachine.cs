namespace CoopStory.Protocol;

public sealed class DownedStateMachine
{
    public const int ReviveDurationMs = 4000;
    public const float ReviveDistanceMeters = 2f;
    public const float RevivedHealthFraction = 0.35f;

    private NetEntityId _reviverId;
    private ulong _reviveStartedTick;

    public DownedStateMachine(NetEntityId entityId)
    {
        if (entityId.IsNone)
        {
            throw new ArgumentException("Entity identifier cannot be zero.", nameof(entityId));
        }

        EntityId = entityId;
    }

    public NetEntityId EntityId { get; }

    public PlayerLifecycle State { get; private set; } = PlayerLifecycle.Alive;

    public float HealthFraction { get; private set; } = 1f;

    public void ApplyHealth(float healthFraction, ulong tick)
    {
        if (!float.IsFinite(healthFraction))
        {
            throw new ArgumentOutOfRangeException(nameof(healthFraction));
        }

        if (healthFraction <= 0f)
        {
            EnterDowned(tick);
            return;
        }

        if (State == PlayerLifecycle.Alive)
        {
            HealthFraction = Math.Clamp(healthFraction, 0f, 1f);
        }
    }

    public void EnterDowned(ulong tick)
    {
        _ = tick;
        State = PlayerLifecycle.Downed;
        HealthFraction = 0f;
        _reviverId = NetEntityId.None;
        _reviveStartedTick = 0;
    }

    public bool TryBeginRevive(
        NetEntityId reviverId,
        float distanceMeters,
        ulong tick)
    {
        if (State != PlayerLifecycle.Downed ||
            reviverId.IsNone ||
            reviverId == EntityId ||
            !float.IsFinite(distanceMeters) ||
            distanceMeters < 0f ||
            distanceMeters > ReviveDistanceMeters)
        {
            return false;
        }

        State = PlayerLifecycle.Reviving;
        _reviverId = reviverId;
        _reviveStartedTick = tick;
        return true;
    }

    public bool UpdateRevive(
        NetEntityId reviverId,
        float distanceMeters,
        bool holdingInteraction,
        ulong tick)
    {
        if (State != PlayerLifecycle.Reviving)
        {
            return false;
        }

        if (!holdingInteraction ||
            reviverId != _reviverId ||
            !float.IsFinite(distanceMeters) ||
            distanceMeters < 0f ||
            distanceMeters > ReviveDistanceMeters ||
            tick < _reviveStartedTick)
        {
            CancelRevive();
            return false;
        }

        if (tick - _reviveStartedTick < ReviveDurationMs)
        {
            return false;
        }

        State = PlayerLifecycle.Alive;
        HealthFraction = RevivedHealthFraction;
        _reviverId = NetEntityId.None;
        _reviveStartedTick = 0;
        return true;
    }

    public void CancelRevive()
    {
        if (State == PlayerLifecycle.Reviving)
        {
            State = PlayerLifecycle.Downed;
            _reviverId = NetEntityId.None;
            _reviveStartedTick = 0;
        }
    }

    public static bool ShouldRetryCheckpoint(
        DownedStateMachine host,
        DownedStateMachine guest)
    {
        ArgumentNullException.ThrowIfNull(host);
        ArgumentNullException.ThrowIfNull(guest);
        return IsIncapacitated(host.State) && IsIncapacitated(guest.State);
    }

    private static bool IsIncapacitated(PlayerLifecycle state) =>
        state is PlayerLifecycle.Downed or PlayerLifecycle.Reviving;
}
