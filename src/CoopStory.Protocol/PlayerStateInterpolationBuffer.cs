using System.Numerics;

namespace CoopStory.Protocol;

// Legacy/protocol-side bounded snapshot history.
// It exposes the same essential idea as the bridge's remote-motion buffer without depending on RDR2 natives.
public sealed class PlayerStateInterpolationBuffer
{
    private readonly List<TimedState> _states;
    private readonly SequenceTracker _sequence = new();
    private readonly int _capacity;
    private NetEntityId _entityId;

    public PlayerStateInterpolationBuffer(int capacity = 64)
    {
        if (capacity < 2)
        {
            throw new ArgumentOutOfRangeException(nameof(capacity));
        }

        _capacity = capacity;
        _states = new List<TimedState>(capacity);
    }

    public int Count => _states.Count;

    public bool TryAdd(uint sequence, ulong tick, PlayerStatePayload state)
    {
        // Never place duplicate or late data into this strictly ordered history.
        if (!_sequence.TryAccept(sequence))
        {
            return false;
        }

        if (_entityId.IsNone)
        {
            _entityId = state.EntityId;
        }
        else if (_entityId != state.EntityId)
        {
            throw new ArgumentException(
                "An interpolation buffer can contain only one entity.",
                nameof(state));
        }


        // A locomotion epoch marks a discontinuity such as respawn/teleport; discard the old route rather than visually blending across it.
        if (_states.Count > 0 &&
            _states[^1].State.LocomotionEpoch != 0 &&
            state.LocomotionEpoch != 0 &&
            _states[^1].State.LocomotionEpoch != state.LocomotionEpoch)
        {
            _states.Clear();
        }

        if (_states.Count > 0 && tick < _states[^1].Tick)
        {
            return false;
        }

        _states.Add(new TimedState(tick, state));
        if (_states.Count > _capacity)
        {
            _states.RemoveRange(0, _states.Count - _capacity);
        }

        return true;
    }

    public bool TrySample(
        ulong remoteNowTick,
        int interpolationDelayMs,
        out PlayerStatePayload state)
    {
        if (interpolationDelayMs < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(interpolationDelayMs));
        }

        if (_states.Count == 0)
        {
            state = default;
            return false;
        }

        // Render behind the sender's now-tick so two confirmed snapshots are usually available.
        // Outside history, hold the closest state safely.
        var delay = (ulong)interpolationDelayMs;
        var targetTick = remoteNowTick >= delay ? remoteNowTick - delay : 0;
        if (targetTick <= _states[0].Tick)
        {
            state = _states[0].State;
            return true;
        }

        if (targetTick >= _states[^1].Tick)
        {
            state = _states[^1].State;
            return true;
        }

        // Locate the surrounding pair and use velocity-aware interpolation.
        for (var index = 1; index < _states.Count; index++)
        {
            var newer = _states[index];
            if (newer.Tick < targetTick)
            {
                continue;
            }

            var older = _states[index - 1];
            var span = newer.Tick - older.Tick;
            var amount = span == 0
                ? 1f
                : (float)(targetTick - older.Tick) / span;
            state = Interpolate(
                older.State,
                newer.State,
                amount,
                span / 1000f);
            return true;
        }

        state = _states[^1].State;
        return true;
    }

    private static PlayerStatePayload Interpolate(
        PlayerStatePayload older,
        PlayerStatePayload newer,
        float amount,
        float intervalSeconds)
    {
        // Use Hermite for position and shortest-angle interpolation for heading so walking around the 0/360-degree boundary does not spin the puppet.
        var headingDelta = NormalizeAngle(newer.Heading - older.Heading);
        return older with
        {
            Position = HermiteClamped(
                older.Position,
                older.Velocity,
                newer.Position,
                newer.Velocity,
                amount,
                intervalSeconds),
            Velocity = Vector3.Lerp(older.Velocity, newer.Velocity, amount),
            Heading = NormalizeAngle(older.Heading + (headingDelta * amount)),
            MovementHeading = NormalizeAngle(
                older.MovementHeading +
                (NormalizeAngle(newer.MovementHeading - older.MovementHeading) * amount)),
            LocalForwardSpeed = float.Lerp(
                older.LocalForwardSpeed,
                newer.LocalForwardSpeed,
                amount),
            LocalRightSpeed = float.Lerp(
                older.LocalRightSpeed,
                newer.LocalRightSpeed,
                amount),
            DesiredMoveBlend = float.Lerp(
                older.DesiredMoveBlend,
                newer.DesiredMoveBlend,
                amount),
            HealthFraction = float.Lerp(older.HealthFraction, newer.HealthFraction, amount),
            Lifecycle = amount < 1f ? older.Lifecycle : newer.Lifecycle,
            Flags = amount < 1f ? older.Flags : newer.Flags
        };
    }

    private static Vector3 HermiteClamped(
        Vector3 from,
        Vector3 fromVelocity,
        Vector3 to,
        Vector3 toVelocity,
        float amount,
        float intervalSeconds)
    {
        var t = Math.Clamp(amount, 0f, 1f);
        var t2 = t * t;
        var t3 = t2 * t;
        var value =
            (((2f * t3) - (3f * t2) + 1f) * from) +
            ((t3 - (2f * t2) + t) * intervalSeconds * fromVelocity) +
            (((-2f * t3) + (3f * t2)) * to) +
            ((t3 - t2) * intervalSeconds * toVelocity);
        return new Vector3(
            ClampHermiteAxis(value.X, from.X, to.X),
            ClampHermiteAxis(value.Y, from.Y, to.Y),
            ClampHermiteAxis(value.Z, from.Z, to.Z));
    }

    private static float ClampHermiteAxis(float value, float from, float to)
    {
        var margin = MathF.Max(0.2f, MathF.Abs(to - from) * 0.25f);
        return Math.Clamp(value, MathF.Min(from, to) - margin, MathF.Max(from, to) + margin);
    }

    private static float NormalizeAngle(float angle)
    {
        angle %= 360f;
        if (angle > 180f)
        {
            angle -= 360f;
        }
        else if (angle < -180f)
        {
            angle += 360f;
        }

        return angle;
    }

    private readonly record struct TimedState(ulong Tick, PlayerStatePayload State);
}
