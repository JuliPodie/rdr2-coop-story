using System.Numerics;
using System.Net;
using CoopStory.Protocol;
using CoopStory.Sidecar.Configuration;
using CoopStory.Sidecar.Diagnostics;
using CoopStory.Sidecar.Networking;

namespace CoopStory.Sidecar.Simulation;

// Runs an in-process host and guest with artificial network loss/delay.
// This tests replication behavior without launching two RDR2 games.
public sealed record SimulationResult(
    int AttemptedSnapshots,
    int DeliveredByImpairment,
    int ReceivedByHost,
    TimeSpan Duration);

public static class SyntheticPeerSimulator
{
    public static async Task<SimulationResult> RunAsync(
        SidecarConfig baseConfig,
        SessionCredentials credentials,
        JsonLineLogger logger,
        TimeSpan duration,
        NetworkImpairmentProfile impairmentProfile,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(baseConfig);
        ArgumentNullException.ThrowIfNull(credentials);
        ArgumentNullException.ThrowIfNull(logger);
        if (duration <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(duration));
        }

        var hostConfig = baseConfig with
        {
            Role = SessionRole.Host,
            HostAddress = "127.0.0.1"
        };
        var guestConfig = baseConfig with
        {
            Role = SessionRole.Guest,
            HostAddress = "127.0.0.1"
        };
        hostConfig.Validate();
        guestConfig.Validate();

        await using var host = new LanSessionHost(
            hostConfig,
            credentials,
            logger,
            IPAddress.Loopback);
        await using var guest = new LanSessionGuest(
            guestConfig,
            credentials,
            logger,
            IPAddress.Loopback);
        var received = 0;
        host.EnvelopeReceived += (envelope, peerToken, cancellation) =>
        {
            _ = peerToken;
            _ = cancellation;
            if (envelope.Type == MessageType.PlayerState)
            {
                _ = BinaryPayloadCodec.DecodePlayerState(envelope.Payload.Span);
                Interlocked.Increment(ref received);
            }

            return ValueTask.CompletedTask;
        };

        using var timed = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timed.CancelAfter(duration);
        var hostTask = host.RunAsync(timed.Token);
        await Task.Delay(50, timed.Token).ConfigureAwait(false);
        var guestTask = guest.RunAsync(timed.Token);

        var waitUntil = Environment.TickCount64 + 3000;
        while (!guest.IsConnected && Environment.TickCount64 < waitUntil)
        {
            await Task.Delay(20, timed.Token).ConfigureAwait(false);
        }

        if (!guest.IsConnected)
        {
            await timed.CancelAsync().ConfigureAwait(false);
            await ObserveCancellationAsync(hostTask, guestTask).ConfigureAwait(false);
            throw new TimeoutException("Synthetic guest did not connect within three seconds.");
        }

        var impairment = new NetworkImpairmentModel(impairmentProfile);
        var id = NetEntityId.Create(
            unchecked((uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds()) | 1u,
            1);
        var attempted = 0;
        var delivered = 0;
        var pendingTransmissions = new List<Task>();
        var started = DateTimeOffset.UtcNow;
        var interval = TimeSpan.FromSeconds(1d / baseConfig.Replication.SnapshotRateHz);

        try
        {
            using var timer = new PeriodicTimer(interval);
            while (await timer.WaitForNextTickAsync(timed.Token).ConfigureAwait(false))
            {
                var elapsed = DateTimeOffset.UtcNow - started;
                var angle = (float)(elapsed.TotalSeconds * 0.75);
                var state = new PlayerStatePayload(
                    id,
                    Slot: 1,
                    PlayerLifecycle.Alive,
                    new Vector3(
                        MathF.Cos(angle) * 5f,
                        MathF.Sin(angle) * 5f,
                        0f),
                    new Vector3(
                        -MathF.Sin(angle) * 3.75f,
                        MathF.Cos(angle) * 3.75f,
                        0f),
                    angle * (180f / MathF.PI),
                    HealthFraction: 1f,
                    PlayerStateFlags.None);
                var payload = BinaryPayloadCodec.EncodePlayerState(state);
                attempted++;
                var transmission = impairment.TransmitAsync(
                    payload,
                    async (bytes, token) =>
                    {
                        var sent = await guest.SendSnapshotAsync(
                            MessageType.PlayerState,
                            bytes,
                            NetworkClock.Tick,
                            token).ConfigureAwait(false);
                        if (sent)
                        {
                            Interlocked.Increment(ref delivered);
                        }
                    },
                    timed.Token).AsTask();
                pendingTransmissions.Add(transmission);
                if (pendingTransmissions.Count >= 128)
                {
                    pendingTransmissions.RemoveAll(static task => task.IsCompleted);
                }
            }
        }
        catch (OperationCanceledException) when (timed.IsCancellationRequested)
        {
        }
        finally
        {
            await timed.CancelAsync().ConfigureAwait(false);
            await ObserveCancellationAsync(pendingTransmissions.ToArray()).ConfigureAwait(false);
            await ObserveCancellationAsync(hostTask, guestTask).ConfigureAwait(false);
        }

        var actualDuration = DateTimeOffset.UtcNow - started;
        var result = new SimulationResult(attempted, delivered, received, actualDuration);
        await logger.InfoAsync(
            "simulation.completed",
            $"Attempted {attempted}, impairment-delivered {delivered}, host-received {received}.",
            new Dictionary<string, object?>
            {
                ["attemptedSnapshots"] = attempted,
                ["deliveredByImpairment"] = delivered,
                ["receivedByHost"] = received,
                ["durationMs"] = actualDuration.TotalMilliseconds
            },
            CancellationToken.None).ConfigureAwait(false);
        return result;
    }

    private static async Task ObserveCancellationAsync(params Task[] tasks)
    {
        try
        {
            await Task.WhenAll(tasks).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
        }
    }
}
