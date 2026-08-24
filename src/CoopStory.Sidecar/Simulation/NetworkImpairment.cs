namespace CoopStory.Sidecar.Simulation;

public sealed record NetworkImpairmentProfile
{
    public int LatencyMs { get; init; }

    public int JitterMs { get; init; }

    public double LossRate { get; init; }

    public double ReorderRate { get; init; }

    public int Seed { get; init; } = 1;

    public NetworkImpairmentProfile Validate()
    {
        if (LatencyMs < 0 ||
            JitterMs < 0 ||
            LossRate is < 0 or > 1 ||
            ReorderRate is < 0 or > 1)
        {
            throw new ArgumentOutOfRangeException(
                nameof(NetworkImpairmentProfile),
                "Network impairment values are outside their valid ranges.");
        }

        return this;
    }
}

public readonly record struct ImpairmentDecision(
    bool Drop,
    bool Reorder,
    TimeSpan Delay);

public sealed class NetworkImpairmentModel
{
    private readonly object _sync = new();
    private readonly NetworkImpairmentProfile _profile;
    private readonly Random _random;

    public NetworkImpairmentModel(NetworkImpairmentProfile profile)
    {
        _profile = (profile ?? throw new ArgumentNullException(nameof(profile))).Validate();
        _random = new Random(profile.Seed);
    }

    public ImpairmentDecision Next()
    {
        lock (_sync)
        {
            var drop = _random.NextDouble() < _profile.LossRate;
            var reorder = !drop && _random.NextDouble() < _profile.ReorderRate;
            var jitter = _profile.JitterMs == 0
                ? 0
                : _random.Next(-_profile.JitterMs, _profile.JitterMs + 1);
            var delay = Math.Max(0, _profile.LatencyMs + jitter);
            if (reorder)
            {
                delay += Math.Max(1, _profile.JitterMs);
            }

            return new ImpairmentDecision(drop, reorder, TimeSpan.FromMilliseconds(delay));
        }
    }

    public async ValueTask<bool> TransmitAsync<T>(
        T item,
        Func<T, CancellationToken, ValueTask> destination,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(destination);
        var decision = Next();
        if (decision.Drop)
        {
            return false;
        }

        if (decision.Delay > TimeSpan.Zero)
        {
            await Task.Delay(decision.Delay, cancellationToken).ConfigureAwait(false);
        }

        await destination(item, cancellationToken).ConfigureAwait(false);
        return true;
    }
}
