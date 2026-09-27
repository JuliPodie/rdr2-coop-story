using System.Numerics;
using CoopStory.Protocol;

namespace CoopStory.Sidecar.Simulation;

// Repeatable fake combat/action timeline for local multiplayer tests.
// It lets developers test aim, shooting, reloads, jumps, and movement without input.
internal readonly record struct SyntheticActionSample(
    PlayerStateFlags Flags,
    Vector3 AimTarget,
    uint FireSequence,
    uint WeaponHash,
    uint Ammo,
    bool Reloading,
    string Phase);

internal readonly record struct SyntheticMovementCoupling(
    TimeSpan MotionElapsed,
    bool MovementBlocked,
    string? BlockingPhase);

internal static class SyntheticPlayerActionCourse
{
    public const uint CattlemanRevolverHash = 0x169F59F7u;
    private const double CycleSeconds = 60d;
    private const double DirectionSequenceStartSeconds = 10d;
    private const double DirectionSegmentSeconds = 3d;
    private const int DirectionCount = 6;
    private const double ReloadStartSeconds = 30d;
    private const double ReloadEndSeconds = 34d;
    private const double JumpStartSeconds = 36d;
    private const double JumpEndSeconds = 36.75d;
    private const double ReloadDurationSeconds =
        ReloadEndSeconds - ReloadStartSeconds;

    private static readonly string[] DirectionNames =
    [
        "forward",
        "up",
        "down",
        "left",
        "right",
        "behind-180"
    ];

    public static SyntheticMovementCoupling CoupleMovement(
        TimeSpan elapsed)
    {
        var totalSeconds = Math.Max(0d, elapsed.TotalSeconds);
        var completedCycles = Math.Floor(totalSeconds / CycleSeconds);
        var cycleSeconds = totalSeconds - (completedCycles * CycleSeconds);
        var currentCycleBlockedSeconds = cycleSeconds switch
        {
            < ReloadStartSeconds => 0d,
            < ReloadEndSeconds => cycleSeconds - ReloadStartSeconds,
            _ => ReloadDurationSeconds
        };
        var totalBlockedSeconds =
            (completedCycles * ReloadDurationSeconds) +
            currentCycleBlockedSeconds;
        var movementBlocked =
            cycleSeconds is >= ReloadStartSeconds and < ReloadEndSeconds;

        // The action and movement courses share one logical clock.
        // While a full-body reload owns the puppet task graph, the network marker is frozen at the same point instead of continuing along the route and creating an impossible 10-15 metre catch-up.
        // Subtracting every completed reload keeps the trajectory continuous across cycles.
        return new SyntheticMovementCoupling(
            TimeSpan.FromSeconds(
                Math.Max(0d, totalSeconds - totalBlockedSeconds)),
            movementBlocked,
            movementBlocked ? "reload" : null);
    }

    public static SyntheticActionSample Sample(
        TimeSpan elapsed,
        Vector3 playerPosition,
        float playerHeadingDegrees)
    {
        var totalSeconds = Math.Max(0d, elapsed.TotalSeconds);
        var completedCycles = Math.Floor(totalSeconds / CycleSeconds);
        var cycleSeconds = totalSeconds - (completedCycles * CycleSeconds);
        var directionSequenceEnd =
            DirectionSequenceStartSeconds +
            (DirectionCount * DirectionSegmentSeconds);

        if (cycleSeconds >= DirectionSequenceStartSeconds &&
            cycleSeconds < directionSequenceEnd)
        {
            var sequenceElapsed =
                cycleSeconds - DirectionSequenceStartSeconds;
            var directionIndex = Math.Clamp(
                (int)(sequenceElapsed / DirectionSegmentSeconds),
                0,
                DirectionCount - 1);
            var segmentSeconds =
                sequenceElapsed -
                (directionIndex * DirectionSegmentSeconds);
            var firing = segmentSeconds is >= 1.50d and < 1.80d;
            var fireSequenceValue = checked(
                ((long)completedCycles * DirectionCount) +
                directionIndex +
                1L);
            var fireSequence = unchecked((uint)fireSequenceValue);
            if (fireSequence == 0u)
            {
                fireSequence = 1u;
            }

            var flags =
                PlayerStateFlags.Aiming |
                PlayerStateFlags.AimTargetValid;
            if (firing)
            {
                flags |= PlayerStateFlags.Firing;
            }

            var directionName = DirectionNames[directionIndex];
            return new SyntheticActionSample(
                flags,
                AimTarget(
                    directionIndex,
                    playerPosition,
                    playerHeadingDegrees),
                firing ? fireSequence : 0u,
                CattlemanRevolverHash,
                Ammo: 120u,
                Reloading: false,
                Phase: firing
                    ? $"fire-{directionName}"
                    : $"aim-{directionName}");
        }

        if (cycleSeconds is >= ReloadStartSeconds and < ReloadEndSeconds)
        {
            return new SyntheticActionSample(
                PlayerStateFlags.None,
                Vector3.Zero,
                FireSequence: 0u,
                CattlemanRevolverHash,
                Ammo: 120u,
                Reloading: true,
                Phase: "reload");
        }

        if (cycleSeconds is >= JumpStartSeconds and < JumpEndSeconds)
        {
            return new SyntheticActionSample(
                PlayerStateFlags.Jumping,
                Vector3.Zero,
                FireSequence: 0u,
                CattlemanRevolverHash,
                Ammo: 120u,
                Reloading: false,
                Phase: "jump");
        }

        return new SyntheticActionSample(
            PlayerStateFlags.None,
            Vector3.Zero,
            FireSequence: 0u,
            CattlemanRevolverHash,
            Ammo: 120u,
            Reloading: false,
            Phase: "movement-only");
    }

    private static Vector3 AimTarget(
        int directionIndex,
        Vector3 playerPosition,
        float headingDegrees)
    {
        var radians = headingDegrees * (MathF.PI / 180f);
        var forward = new Vector3(
            MathF.Cos(radians),
            MathF.Sin(radians),
            0f);
        var left = new Vector3(-forward.Y, forward.X, 0f);
        var origin = playerPosition + new Vector3(0f, 0f, 1.15f);

        return directionIndex switch
        {
            0 => origin + (forward * 25f),
            1 => origin + (forward * 20f) + new Vector3(0f, 0f, 15f),
            2 => origin + (forward * 20f) - new Vector3(0f, 0f, 8f),
            3 => origin + (left * 25f),
            4 => origin - (left * 25f),
            5 => origin - (forward * 25f),
            _ => origin + (forward * 25f)
        };
    }
}
