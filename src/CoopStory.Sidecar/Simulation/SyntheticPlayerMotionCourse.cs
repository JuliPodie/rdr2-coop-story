using System.Numerics;

namespace CoopStory.Sidecar.Simulation;

public enum LocalGameTestMotionProfile
{
    LiveMirror,
    FollowHost,
    PuppetCourse
}

internal readonly record struct SyntheticMotionSample(
    Vector3 Offset,
    Vector3 RelativeVelocity,
    float RelativeHeadingDegrees,
    string Phase);

internal static class SyntheticPlayerMotionCourse
{
    private const double CycleSeconds = 36d;
    private const float RadiusMeters = 8f;
    private const double CycleAngleRadians = 5.625d;

    public static SyntheticMotionSample Sample(
        LocalGameTestMotionProfile profile,
        TimeSpan elapsed,
        float hostHeadingDegrees)
    {
        if (profile == LocalGameTestMotionProfile.LiveMirror)
        {
            throw new ArgumentException(
                "LiveMirror remaps the real host stream and does not use the synthetic motion course.",
                nameof(profile));
        }

        if (profile == LocalGameTestMotionProfile.FollowHost)
        {
            var headingRadians = hostHeadingDegrees * (MathF.PI / 180f);
            return new SyntheticMotionSample(
                new Vector3(
                    MathF.Cos(headingRadians) * 2f,
                    MathF.Sin(headingRadians) * 2f,
                    0f),
                Vector3.Zero,
                hostHeadingDegrees,
                "follow");
        }

        var totalSeconds = Math.Max(0d, elapsed.TotalSeconds);
        var completedCycles = Math.Floor(totalSeconds / CycleSeconds);
        var cycleSeconds = totalSeconds - (completedCycles * CycleSeconds);
        var (phaseAngle, angularVelocity, phase) = SampleCycle(cycleSeconds);
        var angle = (float)(
            (completedCycles * CycleAngleRadians) + phaseAngle);
        var cosine = MathF.Cos(angle);
        var sine = MathF.Sin(angle);
        var relativeVelocity = new Vector3(
            -sine * RadiusMeters * angularVelocity,
            cosine * RadiusMeters * angularVelocity,
            0f);
        var heading = relativeVelocity.LengthSquared() > 0.01f
            ? NormalizeHeading(
                MathF.Atan2(relativeVelocity.Y, relativeVelocity.X) *
                (180f / MathF.PI))
            : NormalizeHeading(
                (angle * (180f / MathF.PI)) + 90f);

        return new SyntheticMotionSample(
            new Vector3(cosine * RadiusMeters, sine * RadiusMeters, 0f),
            relativeVelocity,
            heading,
            phase);
    }

    private static (double Angle, float AngularVelocity, string Phase)
        SampleCycle(double seconds)
    {
        if (seconds < 3d)
        {
            return (0d, 0f, "idle");
        }

        if (seconds < 9d)
        {
            var elapsed = seconds - 3d;
            return (elapsed * 0.125d, 0.125f, "walk");
        }

        if (seconds < 15d)
        {
            var elapsed = seconds - 9d;
            return (0.75d + (elapsed * 0.375d), 0.375f, "run");
        }

        if (seconds < 21d)
        {
            var elapsed = seconds - 15d;
            return (3d + (elapsed * 0.6875d), 0.6875f, "sprint");
        }

        if (seconds < 24d)
        {
            return (7.125d, 0f, "stop-and-turn");
        }

        if (seconds < 30d)
        {
            var elapsed = seconds - 24d;
            return (7.125d - (elapsed * 0.375d), -0.375f, "reverse-run");
        }

        var finalElapsed = seconds - 30d;
        return (4.875d + (finalElapsed * 0.125d), 0.125f, "walk-return");
    }

    private static float NormalizeHeading(float heading)
    {
        heading %= 360f;
        return heading < 0f ? heading + 360f : heading;
    }
}
