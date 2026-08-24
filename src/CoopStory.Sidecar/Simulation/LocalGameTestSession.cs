using System.Numerics;
using System.Net;
using CoopStory.Protocol;
using CoopStory.Sidecar.Configuration;
using CoopStory.Sidecar.Diagnostics;
using CoopStory.Sidecar.Networking;
using CoopStory.Sidecar.Session;

namespace CoopStory.Sidecar.Simulation;

public sealed record LocalGameTestResult(
    int HostSnapshotsObserved,
    int GuestSnapshotsSent,
    int GuestSnapshotsDelivered,
    TimeSpan Duration);

public static class LocalGameTestSession
{
    private const long HostSnapshotFreshnessMs = 1_000;

    public static async Task<LocalGameTestResult> RunAsync(
        SidecarConfig config,
        SessionCredentials credentials,
        JsonLineLogger logger,
        CancellationToken cancellationToken = default,
        Action<string>? status = null,
        LocalGameTestMotionProfile motionProfile =
            LocalGameTestMotionProfile.PuppetCourse,
        bool waitForInGameActivation = false,
        string? ghostRecordingPath = null)
    {
        ArgumentNullException.ThrowIfNull(config);
        ArgumentNullException.ThrowIfNull(credentials);
        ArgumentNullException.ThrowIfNull(logger);
        config.Validate();
        if (config.Role != SessionRole.Host)
        {
            throw new ConfigurationException(
                "local-test requires a Host configuration.");
        }

        var hostConfig = config with
        {
            Role = SessionRole.Host,
            InGameMenuEnabled = false
        };
        var guestConfig = config with
        {
            Role = SessionRole.Guest,
            HostAddress = "127.0.0.1",
            InGameMenuEnabled = false
        };
        guestConfig.Validate();

        await using var runtime = new SidecarRuntime(
            hostConfig,
            credentials,
            logger,
            IPAddress.Loopback);
        await using var guest = new LanSessionGuest(
            guestConfig,
            credentials,
            logger,
            IPAddress.Loopback);

        HostAnchor? latestHost = null;
        var hostSnapshotsObserved = 0;
        var guestSnapshotsSent = 0;
        var guestSnapshotsDelivered = 0;
        var modeGate = new object();
        var mode = waitForInGameActivation
            ? LocalSoloTestMode.Idle
            : LocalSoloTestMode.Automatic;
        var modeGeneration = 0;
        var entityId = CreateSyntheticGuestId();
        GhostRecordingDocument? replayRecording = null;
        long replayStartedAtMilliseconds = 0;
        var replayFrameIndex = 0;
        ushort replayTraversalActionId = 0;
        ushort replayTraversalRevision = 0;
        var replayTraversalKind = PlayerTraversalKind.None;
        var recordingStore = new GhostRecordingStore(
            ResolveGhostRecordingPath(ghostRecordingPath),
            hostConfig.Replication.SnapshotRateHz);
        long motionCourseStartedAtMilliseconds = 0;
        long previousIdentityPublishMilliseconds = 0;
        long previousEquipmentPublishMilliseconds = 0;
        long previousGhostProgressMilliseconds = 0;
        string? previousMotionPhase = null;
        string? previousActionPhase = null;
        bool? previousReloading = null;
        ushort syntheticLocomotionEpoch = 1;
        uint syntheticAnimationSampleSequence = 0;
        ushort syntheticTraversalActionId = 0;
        var syntheticTraversalKind = PlayerTraversalKind.None;
        var syntheticTraversalAnchor = Vector3.Zero;
        var syntheticTraversalHeading = 0f;
        long syntheticTraversalExpiresMilliseconds = 0;
        var previousSyntheticJumping = false;
        Vector3? previousSyntheticPosition = null;
        var previousSyntheticHeading = 0f;
        runtime.SoloTestToggleRequested += () =>
        {
            bool enabled;
            lock (modeGate)
            {
                if (mode == LocalSoloTestMode.Recording)
                {
                    throw new InvalidOperationException(
                        "Stop Ghost Record first so the route can be saved safely.");
                }
                enabled = mode != LocalSoloTestMode.Automatic;
                mode = enabled
                    ? LocalSoloTestMode.Automatic
                    : LocalSoloTestMode.Idle;
                replayRecording = null;
                replayStartedAtMilliseconds = 0;
                replayFrameIndex = 0;
                replayTraversalActionId = 0;
                replayTraversalRevision = 0;
                replayTraversalKind = PlayerTraversalKind.None;
                entityId = CreateSyntheticGuestId();
                modeGeneration++;
            }
            status?.Invoke(
                enabled
                    ? "LOCAL_TEST_F9_ENABLED: synthetic player course started."
                    : "LOCAL_TEST_F9_DISABLED: synthetic player stream paused.");
            return enabled;
        };
        runtime.GhostRecordToggleRequested += () =>
        {
            lock (modeGate)
            {
                if (mode == LocalSoloTestMode.Recording)
                {
                    var saved = recordingStore.CompleteAndSave();
                    mode = LocalSoloTestMode.Idle;
                    modeGeneration++;
                    status?.Invoke(
                        $"GHOST_RECORD_SAVED: frames={saved.Frames.Count} " +
                        $"durationMs={saved.Frames[^1].OffsetMilliseconds} " +
                        $"path={recordingStore.Path}");
                    return false;
                }

                recordingStore.Begin();
                mode = LocalSoloTestMode.Recording;
                replayRecording = null;
                replayStartedAtMilliseconds = 0;
                replayFrameIndex = 0;
                replayTraversalActionId = 0;
                replayTraversalRevision = 0;
                replayTraversalKind = PlayerTraversalKind.None;
                entityId = CreateSyntheticGuestId();
                modeGeneration++;
                status?.Invoke(
                    "GHOST_RECORD_STARTED: move the local player; F9 stops and saves the route.");
                return true;
            }
        };
        runtime.GhostReplayToggleRequested += () =>
        {
            lock (modeGate)
            {
                if (mode == LocalSoloTestMode.Recording)
                {
                    throw new InvalidOperationException(
                        "Stop Ghost Record before starting Ghost Replay.");
                }
                if (mode == LocalSoloTestMode.Replaying)
                {
                    mode = LocalSoloTestMode.Idle;
                    replayRecording = null;
                    replayStartedAtMilliseconds = 0;
                    replayFrameIndex = 0;
                    replayTraversalActionId = 0;
                    replayTraversalRevision = 0;
                    replayTraversalKind = PlayerTraversalKind.None;
                    modeGeneration++;
                    status?.Invoke("GHOST_REPLAY_STOPPED: synthetic stream paused.");
                    return false;
                }

                replayRecording = recordingStore.Load();
                replayStartedAtMilliseconds = 0;
                replayFrameIndex = 0;
                replayTraversalActionId = 0;
                replayTraversalRevision = 0;
                replayTraversalKind = PlayerTraversalKind.None;
                entityId = CreateSyntheticGuestId();
                mode = LocalSoloTestMode.Replaying;
                modeGeneration++;
                status?.Invoke(
                    $"GHOST_REPLAY_STARTED: frames={replayRecording.Frames.Count} " +
                    $"durationMs={replayRecording.Frames[^1].OffsetMilliseconds}");
                return true;
            }
        };
        runtime.BridgeEnvelopeDelivered += envelope =>
        {
            if (envelope.Type != MessageType.PlayerState)
            {
                return;
            }

            var state = BinaryPayloadCodec.DecodePlayerState(
                envelope.Payload.Span);
            if (state.Slot != (byte)SessionRole.Guest)
            {
                return;
            }

            var delivered = Interlocked.Increment(
                ref guestSnapshotsDelivered);
            if (delivered == 1)
            {
                status?.Invoke(
                    "LOCAL_TEST_GUEST_STREAMING: guest snapshots reached the game bridge.");
            }
        };
        guest.EnvelopeReceived += async (
            envelope,
            _,
            eventCancellationToken) =>
        {
            if (envelope.Type == MessageType.MotionReplicationConfig)
            {
                var hostAnnouncement = AnimationReplicationPayloadCodec
                    .DecodeMotionReplicationConfig(envelope.Payload.Span);
                var expectedMode = ToWireMotionMode(
                    guestConfig.MotionReplicationMode);
                if (hostAnnouncement.Mode != expectedMode ||
                    hostAnnouncement.Flags !=
                        MotionReplicationConfigFlags.None)
                {
                    throw new InvalidDataException(
                        "Synthetic guest received an incompatible motion-mode announcement.");
                }

                var guestAnnouncement = new MotionReplicationConfigPayload(
                    AnimationReplicationPayloadCodec
                        .MotionReplicationConfigSchemaVersion,
                    expectedMode,
                    MotionReplicationConfigFlags.None,
                    Revision: 1);
                var announced = await guest.SendControlAsync(
                        MessageType.MotionReplicationConfig,
                        AnimationReplicationPayloadCodec
                            .EncodeMotionReplicationConfig(guestAnnouncement),
                        NetworkClock.Tick,
                        eventCancellationToken)
                    .ConfigureAwait(false);
                if (!announced)
                {
                    throw new IOException(
                        "Synthetic guest could not announce its motion mode.");
                }
                return;
            }

            if (envelope.Type == MessageType.PlayerState)
            {
                var state = BinaryPayloadCodec.DecodePlayerState(
                    envelope.Payload.Span);
                if (state.Slot == (byte)SessionRole.Host)
                {
                    var observedAtMilliseconds = Environment.TickCount64;
                    var observed = Interlocked.Increment(
                        ref hostSnapshotsObserved);
                    Volatile.Write(
                        ref latestHost,
                        new HostAnchor(
                            state.Position,
                            state.Velocity,
                            state.Heading,
                            state.Flags,
                            observedAtMilliseconds));
                    lock (modeGate)
                    {
                        if (mode == LocalSoloTestMode.Recording)
                        {
                            recordingStore.Capture(
                                state,
                                observedAtMilliseconds);
                        }
                    }
                    if (observed == 1)
                    {
                        status?.Invoke(
                            "LOCAL_TEST_BRIDGE_ACTIVE: received the live player position.");
                    }
                }
            }
            else if (envelope.Type == MessageType.EquipmentState)
            {
                var equipment = BinaryPayloadCodec.DecodeEquipmentState(
                    envelope.Payload.Span);
                recordingStore.ObserveEquipment(equipment);
            }

        };

        using var linked = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken);
        var started = DateTimeOffset.UtcNow;
        var runtimeTask = runtime.RunAsync(linked.Token);
        await ObserveStartupAsync(runtimeTask, linked.Token).ConfigureAwait(false);
        var guestTask = guest.RunAsync(linked.Token);

        try
        {
            await WaitForGuestAsync(
                guest,
                runtimeTask,
                guestTask,
                linked.Token).ConfigureAwait(false);
            await logger.InfoAsync(
                "local-test.peer.connected",
                "Synthetic guest authenticated over loopback; waiting for the game bridge.",
                cancellationToken: linked.Token).ConfigureAwait(false);
            status?.Invoke(
                "LOCAL_TEST_PEER_READY: synthetic guest authenticated; waiting for RDR2.");
            if (waitForInGameActivation)
            {
                status?.Invoke(
                    "LOCAL_TEST_WAITING_FOR_F9: open F9 and select Test solo: start / stop.");
            }

            var interval = TimeSpan.FromSeconds(
                1d / hostConfig.Replication.SnapshotRateHz);
            var observedModeGeneration = -1;
            using var timer = new PeriodicTimer(interval);
            while (await timer.WaitForNextTickAsync(linked.Token)
                       .ConfigureAwait(false))
            {
                ThrowIfServiceStopped(runtimeTask, guestTask, linked.Token);
                LocalSoloTestMode currentMode;
                int currentModeGeneration;
                NetEntityId currentEntityId;
                lock (modeGate)
                {
                    currentMode = mode;
                    currentModeGeneration = modeGeneration;
                    currentEntityId = entityId;
                }
                if (observedModeGeneration != currentModeGeneration)
                {
                    observedModeGeneration = currentModeGeneration;
                    motionCourseStartedAtMilliseconds = 0;
                    previousIdentityPublishMilliseconds = 0;
                    previousEquipmentPublishMilliseconds = 0;
                    previousGhostProgressMilliseconds = 0;
                    previousMotionPhase = null;
                    previousActionPhase = null;
                    previousReloading = null;
                    syntheticLocomotionEpoch = 1;
                    syntheticAnimationSampleSequence = 0;
                    syntheticTraversalActionId = 0;
                    syntheticTraversalKind = PlayerTraversalKind.None;
                    syntheticTraversalAnchor = Vector3.Zero;
                    syntheticTraversalHeading = 0f;
                    syntheticTraversalExpiresMilliseconds = 0;
                    previousSyntheticJumping = false;
                    previousSyntheticPosition = null;
                    previousSyntheticHeading = 0f;
                }

                var host = Volatile.Read(ref latestHost);
                if (host is null ||
                    Environment.TickCount64 - host.ObservedAtMilliseconds >
                    HostSnapshotFreshnessMs)
                {
                    if (host is not null)
                    {
                        _ = Interlocked.CompareExchange(
                            ref latestHost,
                            null,
                            host);
                    }

                    // This authenticated UDP packet teaches the host which
                    // ephemeral endpoint belongs to the synthetic guest.
                    // It is intentionally ignored by SidecarRuntime, so no
                    // invalid world-space position can reach the bridge.
                    _ = await guest.SendSnapshotAsync(
                        MessageType.Heartbeat,
                        ReadOnlyMemory<byte>.Empty,
                        NetworkClock.Tick,
                        linked.Token).ConfigureAwait(false);
                    continue;
                }

                if (currentMode is LocalSoloTestMode.Idle or
                    LocalSoloTestMode.Recording)
                {
                    _ = await guest.SendSnapshotAsync(
                        MessageType.Heartbeat,
                        ReadOnlyMemory<byte>.Empty,
                        NetworkClock.Tick,
                        linked.Token).ConfigureAwait(false);
                    continue;
                }

                var nowMilliseconds = Environment.TickCount64;
                if (currentMode == LocalSoloTestMode.Replaying)
                {
                    GhostRecordedFrame? replayFrame = null;
                    var replayCompleted = false;
                    var replayElapsedMilliseconds = 0L;
                    var activeReplayFrameIndex = 0;
                    lock (modeGate)
                    {
                        if (mode == LocalSoloTestMode.Replaying &&
                            replayRecording is not null)
                        {
                            if (replayStartedAtMilliseconds == 0)
                            {
                                replayStartedAtMilliseconds = nowMilliseconds;
                            }
                            replayElapsedMilliseconds = Math.Max(
                                0,
                                nowMilliseconds - replayStartedAtMilliseconds);
                            var frames = replayRecording.Frames;
                            while (replayFrameIndex + 1 < frames.Count &&
                                   frames[replayFrameIndex + 1]
                                           .OffsetMilliseconds <=
                                       replayElapsedMilliseconds)
                            {
                                replayFrameIndex++;
                            }
                            if (replayElapsedMilliseconds >
                                frames[^1].OffsetMilliseconds + 250L)
                            {
                                mode = LocalSoloTestMode.Idle;
                                replayRecording = null;
                                replayStartedAtMilliseconds = 0;
                                replayFrameIndex = 0;
                                modeGeneration++;
                                replayCompleted = true;
                            }
                            else
                            {
                                replayFrame = frames[replayFrameIndex];
                                activeReplayFrameIndex = replayFrameIndex;
                            }
                        }
                    }

                    if (replayCompleted)
                    {
                        const string completedMessage =
                            "GHOST_REPLAY_COMPLETED: route reached its final recorded frame.";
                        status?.Invoke(completedMessage);
                        await logger.InfoAsync(
                            "ghost-replay.completed",
                            completedMessage,
                            cancellationToken: linked.Token).ConfigureAwait(false);
                        _ = await guest.SendSnapshotAsync(
                            MessageType.Heartbeat,
                            ReadOnlyMemory<byte>.Empty,
                            NetworkClock.Tick,
                            linked.Token).ConfigureAwait(false);
                        continue;
                    }
                    if (replayFrame is null)
                    {
                        continue;
                    }

                    var replayState = replayFrame.ToPlayerState(currentEntityId);
                    if (hostConfig.MotionReplicationMode ==
                            MotionReplicationMode.AnimGraphReplica &&
                        replayState.LocomotionEpoch == 0)
                    {
                        replayState = replayState with
                        {
                            LocomotionEpoch = 1
                        };
                    }
                    var replayTick = NetworkClock.Tick;
                    var replaySent = await guest.SendSnapshotAsync(
                        MessageType.PlayerState,
                        BinaryPayloadCodec.EncodePlayerState(replayState),
                        replayTick,
                        linked.Token).ConfigureAwait(false);
                    if (replaySent)
                    {
                        _ = Interlocked.Increment(ref guestSnapshotsSent);
                    }
                    syntheticAnimationSampleSequence = AdvanceNonZero(
                        syntheticAnimationSampleSequence);
                    var replayAnimation = CreateSyntheticAnimationState(
                        hostConfig.MotionReplicationMode,
                        replayState,
                        syntheticAnimationSampleSequence);
                    if (replayAnimation is { } replayAnimationState)
                    {
                        _ = await guest.SendSnapshotAsync(
                            MessageType.PlayerAnimationState,
                            AnimationReplicationPayloadCodec
                                .EncodePlayerAnimationState(
                                    replayAnimationState),
                            replayTick,
                            linked.Token).ConfigureAwait(false);
                    }
                    if (replayState.TraversalActionId != 0 &&
                        replayState.TraversalKind != PlayerTraversalKind.None &&
                        (replayState.TraversalActionId !=
                             replayTraversalActionId ||
                         replayState.TraversalKind != replayTraversalKind))
                    {
                        if (replayState.TraversalActionId !=
                            replayTraversalActionId)
                        {
                            replayTraversalActionId =
                                replayState.TraversalActionId;
                            replayTraversalRevision = 1;
                        }
                        else
                        {
                            replayTraversalRevision =
                                replayTraversalRevision == ushort.MaxValue
                                    ? (ushort)1
                                    : (ushort)(replayTraversalRevision + 1);
                        }
                        replayTraversalKind = replayState.TraversalKind;
                        var traversal = new PlayerTraversalPayload(
                            replayState.EntityId,
                            replayState.Slot,
                            replayState.TraversalKind,
                            replayState.TraversalActionId,
                            replayTraversalRevision,
                            replayState.LocomotionEpoch == 0
                                ? (ushort)1
                                : replayState.LocomotionEpoch,
                            replayTraversalRevision == 1
                                ? PlayerTraversalFlags.InputEdgeDetected
                                : PlayerTraversalFlags.None,
                            replayState.TraversalHeading,
                            replayState.TraversalAnchor,
                            replayState.Velocity,
                            Vector3.Zero,
                            Vector3.Zero,
                            0f,
                            Vector3.Zero);
                        _ = await guest.SendControlAsync(
                            MessageType.PlayerTraversal,
                            BinaryPayloadCodec.EncodePlayerTraversal(traversal),
                            NetworkClock.Tick,
                            linked.Token).ConfigureAwait(false);
                    }

                    var replayEquipmentRefreshDue =
                        previousEquipmentPublishMilliseconds == 0 ||
                        nowMilliseconds < previousEquipmentPublishMilliseconds ||
                        nowMilliseconds - previousEquipmentPublishMilliseconds >= 1_000;
                    var replayReloading =
                        ((EquipmentStateFlags)replayFrame.EquipmentFlags &
                         EquipmentStateFlags.Reloading) != 0;
                    if (replayEquipmentRefreshDue ||
                        previousReloading != replayReloading)
                    {
                        var replayEquipment =
                            replayFrame.ToEquipmentState(currentEntityId);
                        _ = await guest.SendControlAsync(
                            MessageType.EquipmentState,
                            BinaryPayloadCodec.EncodeEquipmentState(replayEquipment),
                            NetworkClock.Tick,
                            linked.Token).ConfigureAwait(false);
                        previousEquipmentPublishMilliseconds = nowMilliseconds;
                        previousReloading = replayReloading;
                    }
                    if (previousIdentityPublishMilliseconds == 0 ||
                        nowMilliseconds < previousIdentityPublishMilliseconds ||
                        nowMilliseconds - previousIdentityPublishMilliseconds >= 5_000)
                    {
                        var replayIdentity = new PlayerIdentityPayload(
                            currentEntityId,
                            Slot: (byte)SessionRole.Guest,
                            Nickname: "GHOST REPLAY");
                        _ = await guest.SendControlAsync(
                            MessageType.PlayerIdentity,
                            BinaryPayloadCodec.EncodePlayerIdentity(replayIdentity),
                            NetworkClock.Tick,
                            linked.Token).ConfigureAwait(false);
                        previousIdentityPublishMilliseconds = nowMilliseconds;
                    }
                    if (previousGhostProgressMilliseconds == 0 ||
                        nowMilliseconds < previousGhostProgressMilliseconds ||
                        nowMilliseconds - previousGhostProgressMilliseconds >= 1_000)
                    {
                        previousGhostProgressMilliseconds = nowMilliseconds;
                        await logger.InfoAsync(
                            "ghost-replay.progress",
                            "Ghost Replay delivered an exact recorded player sample.",
                            new Dictionary<string, object?>
                            {
                                ["elapsedMs"] = replayElapsedMilliseconds,
                                ["frameIndex"] = activeReplayFrameIndex,
                                ["sourceOffsetMs"] = replayFrame.OffsetMilliseconds,
                                ["positionX"] = replayFrame.PositionX,
                                ["positionY"] = replayFrame.PositionY,
                                ["positionZ"] = replayFrame.PositionZ,
                                ["velocityMetersPerSecond"] = MathF.Sqrt(
                                    replayFrame.VelocityX * replayFrame.VelocityX +
                                    replayFrame.VelocityY * replayFrame.VelocityY +
                                    replayFrame.VelocityZ * replayFrame.VelocityZ),
                                ["playerFlags"] =
                                    $"0x{replayFrame.PlayerFlags:X8}",
                                ["fireSequence"] = replayFrame.FireSequence
                            },
                            linked.Token).ConfigureAwait(false);
                    }
                    continue;
                }

                if (motionCourseStartedAtMilliseconds == 0)
                {
                    motionCourseStartedAtMilliseconds = nowMilliseconds;
                }

                var elapsed = TimeSpan.FromMilliseconds(
                    Math.Max(
                        0,
                        nowMilliseconds - motionCourseStartedAtMilliseconds));
                var movementCoupling =
                    SyntheticPlayerActionCourse.CoupleMovement(elapsed);
                var sampledMotion = SyntheticPlayerMotionCourse.Sample(
                    motionProfile,
                    movementCoupling.MotionElapsed,
                    host.Heading);
                var motion = movementCoupling.MovementBlocked
                    ? sampledMotion with
                    {
                        RelativeVelocity = Vector3.Zero,
                        Phase = $"{movementCoupling.BlockingPhase}-hold"
                    }
                    : sampledMotion;
                if (!string.Equals(
                        previousMotionPhase,
                        motion.Phase,
                        StringComparison.Ordinal))
                {
                    previousMotionPhase = motion.Phase;
                    var message =
                        $"LOCAL_TEST_MOTION_PHASE: {motion.Phase}";
                    status?.Invoke(message);
                    await logger.InfoAsync(
                        "local-test.motion.phase",
                        message,
                        new Dictionary<string, object?>
                        {
                            ["profile"] = motionProfile.ToString(),
                            ["phase"] = motion.Phase,
                            ["elapsedMs"] = Math.Max(
                                0,
                                nowMilliseconds - motionCourseStartedAtMilliseconds)
                        },
                        linked.Token).ConfigureAwait(false);
                }

                var syntheticPosition = host.Position + motion.Offset;
                var syntheticHeading =
                    motion.RelativeVelocity.LengthSquared() > 0.01f
                        ? motion.RelativeHeadingDegrees
                        : host.Heading;
                var action = SyntheticPlayerActionCourse.Sample(
                    elapsed,
                    syntheticPosition,
                    syntheticHeading);
                if (!string.Equals(
                        previousActionPhase,
                        action.Phase,
                        StringComparison.Ordinal))
                {
                    previousActionPhase = action.Phase;
                    var message =
                        $"LOCAL_TEST_ACTION_PHASE: {action.Phase}";
                    status?.Invoke(message);
                    await logger.InfoAsync(
                        "local-test.action.phase",
                        message,
                        new Dictionary<string, object?>
                        {
                            ["phase"] = action.Phase,
                            ["aiming"] =
                                (action.Flags & PlayerStateFlags.Aiming) != 0,
                            ["firing"] =
                                (action.Flags & PlayerStateFlags.Firing) != 0,
                            ["reloading"] = action.Reloading,
                            ["fireSequence"] = action.FireSequence
                        },
                        linked.Token).ConfigureAwait(false);
                }

                var syntheticVelocity =
                    host.Velocity + motion.RelativeVelocity;
                var horizontalSpeed = MathF.Sqrt(
                    (syntheticVelocity.X * syntheticVelocity.X) +
                    (syntheticVelocity.Y * syntheticVelocity.Y));
                var movementHeading = horizontalSpeed >= 0.1f
                    ? NormalizeHeading(
                        MathF.Atan2(
                            syntheticVelocity.Y,
                            syntheticVelocity.X) *
                        (180f / MathF.PI))
                    : NormalizeHeading(syntheticHeading);
                var headingRadians =
                    NormalizeHeading(syntheticHeading) *
                    (MathF.PI / 180f);
                var forward = new Vector2(
                    MathF.Cos(headingRadians),
                    MathF.Sin(headingRadians));
                var localForwardSpeed =
                    (syntheticVelocity.X * forward.X) +
                    (syntheticVelocity.Y * forward.Y);
                var localRightSpeed =
                    (syntheticVelocity.X * -forward.Y) +
                    (syntheticVelocity.Y * forward.X);
                var desiredMoveBlend = horizontalSpeed switch
                {
                    < 0.12f => 0f,
                    < 2.2f => 1f,
                    < 5f => 2f,
                    _ => 3f
                };
                var jumping =
                    (action.Flags & PlayerStateFlags.Jumping) != 0;
                if (jumping && !previousSyntheticJumping)
                {
                    syntheticTraversalActionId = AdvanceNonZero(
                        syntheticTraversalActionId);
                    syntheticLocomotionEpoch = AdvanceNonZero(
                        syntheticLocomotionEpoch);
                    syntheticTraversalKind = PlayerTraversalKind.Jump;
                    syntheticTraversalAnchor =
                        previousSyntheticPosition ?? syntheticPosition;
                    syntheticTraversalHeading =
                        previousSyntheticPosition.HasValue
                            ? previousSyntheticHeading
                            : syntheticHeading;
                    syntheticTraversalExpiresMilliseconds =
                        nowMilliseconds + 1_500;
                }
                previousSyntheticJumping = jumping;
                if (syntheticTraversalExpiresMilliseconds != 0 &&
                    nowMilliseconds > syntheticTraversalExpiresMilliseconds)
                {
                    syntheticTraversalKind = PlayerTraversalKind.None;
                    syntheticTraversalExpiresMilliseconds = 0;
                }

                var state = new PlayerStatePayload(
                    currentEntityId,
                    Slot: (byte)SessionRole.Guest,
                    PlayerLifecycle.Alive,
                    syntheticPosition,
                    syntheticVelocity,
                    syntheticHeading,
                    HealthFraction: 1f,
                    Flags:
                        (host.Flags & PlayerStateFlags.InMission) |
                        PlayerStateFlags.SyntheticTest |
                        action.Flags,
                    AimTarget: action.AimTarget,
                    FireSequence: action.FireSequence,
                    MovementHeading: movementHeading,
                    LocalForwardSpeed: localForwardSpeed,
                    LocalRightSpeed: localRightSpeed,
                    DesiredMoveBlend: desiredMoveBlend,
                    LocomotionEpoch: syntheticLocomotionEpoch,
                    TraversalActionId: syntheticTraversalActionId,
                    TraversalKind: syntheticTraversalKind,
                    LocomotionMode: jumping
                        ? PlayerLocomotionMode.Traversal
                        : (action.Flags & PlayerStateFlags.Aiming) != 0
                            ? PlayerLocomotionMode.Aiming
                            : PlayerLocomotionMode.Grounded,
                    TraversalAnchor:
                        syntheticTraversalKind != PlayerTraversalKind.None
                            ? syntheticTraversalAnchor
                            : Vector3.Zero,
                    TraversalHeading:
                        syntheticTraversalKind != PlayerTraversalKind.None
                            ? syntheticTraversalHeading
                            : 0f);
                previousSyntheticPosition = syntheticPosition;
                previousSyntheticHeading = syntheticHeading;
                var snapshotTick = NetworkClock.Tick;
                var sent = await guest.SendSnapshotAsync(
                    MessageType.PlayerState,
                    BinaryPayloadCodec.EncodePlayerState(state),
                    snapshotTick,
                    linked.Token).ConfigureAwait(false);
                if (sent)
                {
                    _ = Interlocked.Increment(ref guestSnapshotsSent);
                }
                syntheticAnimationSampleSequence = AdvanceNonZero(
                    syntheticAnimationSampleSequence);
                var animation = CreateSyntheticAnimationState(
                    hostConfig.MotionReplicationMode,
                    state,
                    syntheticAnimationSampleSequence);
                if (animation is { } animationState)
                {
                    _ = await guest.SendSnapshotAsync(
                        MessageType.PlayerAnimationState,
                        AnimationReplicationPayloadCodec
                            .EncodePlayerAnimationState(animationState),
                        snapshotTick,
                        linked.Token).ConfigureAwait(false);
                }
                var equipmentRefreshDue =
                    previousEquipmentPublishMilliseconds == 0 ||
                    nowMilliseconds < previousEquipmentPublishMilliseconds ||
                    nowMilliseconds - previousEquipmentPublishMilliseconds >= 1_000;
                if (equipmentRefreshDue ||
                    previousReloading != action.Reloading)
                {
                    var equipmentFlags = EquipmentStateFlags.Equipped;
                    if (action.Reloading)
                    {
                        equipmentFlags |= EquipmentStateFlags.Reloading;
                    }
                    var equipment = new EquipmentStatePayload(
                        currentEntityId,
                        action.WeaponHash,
                        action.Ammo,
                        equipmentFlags);
                    _ = await guest.SendControlAsync(
                        MessageType.EquipmentState,
                        BinaryPayloadCodec.EncodeEquipmentState(equipment),
                        NetworkClock.Tick,
                        linked.Token).ConfigureAwait(false);
                    previousEquipmentPublishMilliseconds = nowMilliseconds;
                    previousReloading = action.Reloading;
                }
                if (previousIdentityPublishMilliseconds == 0 ||
                    nowMilliseconds < previousIdentityPublishMilliseconds ||
                    nowMilliseconds - previousIdentityPublishMilliseconds >= 5_000)
                {
                    var identity = new PlayerIdentityPayload(
                        currentEntityId,
                        Slot: (byte)SessionRole.Guest,
                        Nickname: "SOLO BOT");
                    _ = await guest.SendControlAsync(
                        MessageType.PlayerIdentity,
                        BinaryPayloadCodec.EncodePlayerIdentity(identity),
                        NetworkClock.Tick,
                        linked.Token).ConfigureAwait(false);
                    previousIdentityPublishMilliseconds = nowMilliseconds;
                }
            }
        }
        catch (OperationCanceledException) when (linked.IsCancellationRequested)
        {
        }
        finally
        {
            await linked.CancelAsync().ConfigureAwait(false);
            await ObserveShutdownAsync(runtimeTask, guestTask).ConfigureAwait(false);
        }

        var duration = DateTimeOffset.UtcNow - started;
        var result = new LocalGameTestResult(
            Volatile.Read(ref hostSnapshotsObserved),
            Volatile.Read(ref guestSnapshotsSent),
            Volatile.Read(ref guestSnapshotsDelivered),
            duration);
        await logger.InfoAsync(
            "local-test.stopped",
            $"Observed {result.HostSnapshotsObserved} host snapshots and sent " +
            $"{result.GuestSnapshotsSent} synthetic guest snapshots; " +
            $"{result.GuestSnapshotsDelivered} reached the game bridge.",
            new Dictionary<string, object?>
            {
                ["hostSnapshotsObserved"] = result.HostSnapshotsObserved,
                ["guestSnapshotsSent"] = result.GuestSnapshotsSent,
                ["guestSnapshotsDelivered"] = result.GuestSnapshotsDelivered,
                ["durationMs"] = result.Duration.TotalMilliseconds
            },
            CancellationToken.None).ConfigureAwait(false);
        return result;
    }

    private static ushort AdvanceNonZero(ushort value) =>
        value == ushort.MaxValue ? (ushort)1 : (ushort)(value + 1);

    private static uint AdvanceNonZero(uint value) =>
        value == uint.MaxValue ? 1U : value + 1U;

    internal static PlayerAnimationStatePayload? CreateSyntheticAnimationState(
        MotionReplicationMode mode,
        PlayerStatePayload state,
        uint sampleSequence)
    {
        if (mode != MotionReplicationMode.AnimGraphReplica)
        {
            return null;
        }

        var locomotionEpoch = state.LocomotionEpoch == 0
            ? (ushort)1
            : state.LocomotionEpoch;
        var motionState = state.DesiredMoveBlend switch
        {
            < 0.10f => RageJoaat("motionstate_idle"),
            < 1.50f => RageJoaat("motionstate_walk"),
            < 2.50f => RageJoaat("motionstate_run"),
            _ => RageJoaat("motionstate_sprint")
        };
        return new PlayerAnimationStatePayload(
            state.EntityId,
            state.Slot,
            AnimationReplicationPayloadCodec.PlayerAnimationStateSchemaVersion,
            PlayerAnimationSampleSource.LocomotionNative,
            locomotionEpoch,
            sampleSequence,
            PlayerAnimationCapabilities.StateIdentifier |
                PlayerAnimationCapabilities.RuntimeFlags,
            PlayerAnimationStateFlags.StateHashValid |
                PlayerAnimationStateFlags.Looping,
            GraphHash: 0,
            StateHash: motionState,
            PrimaryClipHash: 0,
            SecondaryClipHash: 0,
            PrimaryNormalizedPhase: 0,
            SecondaryNormalizedPhase: 0,
            PrimaryPlaybackRate: 0,
            SecondaryPlaybackRate: 0,
            PrimaryBlendWeight: 0,
            SecondaryBlendWeight: 0,
            TransitionProgress: 0);
    }

    private static uint RageJoaat(string value)
    {
        var hash = 0U;
        foreach (var character in value)
        {
            var normalized = character is >= 'A' and <= 'Z'
                ? character + ('a' - 'A')
                : character;
            unchecked
            {
                hash += (byte)normalized;
                hash += hash << 10;
                hash ^= hash >> 6;
            }
        }
        unchecked
        {
            hash += hash << 3;
            hash ^= hash >> 11;
            hash += hash << 15;
        }
        return hash;
    }

    private static MotionReplicationWireMode ToWireMotionMode(
        MotionReplicationMode mode) =>
        mode switch
        {
            MotionReplicationMode.TaskNavmesh =>
                MotionReplicationWireMode.TaskNavmesh,
            MotionReplicationMode.AnimGraphReplica =>
                MotionReplicationWireMode.AnimGraphReplica,
            _ => throw new ConfigurationException(
                $"Unsupported local-test motion mode '{mode}'.")
        };

    private static float NormalizeHeading(float heading)
    {
        heading %= 360f;
        return heading < 0f ? heading + 360f : heading;
    }

    private static string ResolveGhostRecordingPath(string? configuredPath)
    {
        if (!string.IsNullOrWhiteSpace(configuredPath))
        {
            return Path.GetFullPath(configuredPath);
        }

        var localAppData = Environment.GetFolderPath(
            Environment.SpecialFolder.LocalApplicationData);
        var root = string.IsNullOrWhiteSpace(localAppData)
            ? Path.GetTempPath()
            : localAppData;
        return Path.Combine(
            root,
            "RDR2CoopStory",
            "launcher",
            "recordings",
            "ghost-last.json");
    }

    private static async Task ObserveStartupAsync(
        Task runtimeTask,
        CancellationToken cancellationToken)
    {
        var delay = Task.Delay(50, cancellationToken);
        if (await Task.WhenAny(runtimeTask, delay).ConfigureAwait(false) ==
            runtimeTask)
        {
            await runtimeTask.ConfigureAwait(false);
            throw new IOException("Host runtime stopped during local-test startup.");
        }
    }

    private static async Task WaitForGuestAsync(
        LanSessionGuest guest,
        Task runtimeTask,
        Task guestTask,
        CancellationToken cancellationToken)
    {
        var deadline = Environment.TickCount64 + 5_000;
        while (!guest.IsConnected)
        {
            cancellationToken.ThrowIfCancellationRequested();
            ThrowIfServiceStopped(runtimeTask, guestTask, cancellationToken);
            if (Environment.TickCount64 >= deadline)
            {
                throw new TimeoutException(
                    "Synthetic guest did not connect to the local host within five seconds.");
            }

            await Task.Delay(20, cancellationToken).ConfigureAwait(false);
        }
    }

    private static void ThrowIfServiceStopped(
        Task runtimeTask,
        Task guestTask,
        CancellationToken cancellationToken)
    {
        if (cancellationToken.IsCancellationRequested)
        {
            cancellationToken.ThrowIfCancellationRequested();
        }

        if (runtimeTask.IsCompleted)
        {
            runtimeTask.GetAwaiter().GetResult();
            throw new IOException("Host runtime stopped unexpectedly.");
        }

        if (guestTask.IsCompleted)
        {
            guestTask.GetAwaiter().GetResult();
            throw new IOException("Synthetic guest stopped unexpectedly.");
        }
    }

    private static NetEntityId CreateSyntheticGuestId()
    {
        var epoch = unchecked(
            (uint)(DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() ^
                   Environment.TickCount64));
        epoch |= 1u;
        return NetEntityId.Create(epoch, counter: 2);
    }

    private static async Task ObserveShutdownAsync(params Task[] tasks)
    {
        try
        {
            await Task.WhenAll(tasks).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
        }
    }

    private sealed record HostAnchor(
        Vector3 Position,
        Vector3 Velocity,
        float Heading,
        PlayerStateFlags Flags,
        long ObservedAtMilliseconds);

    private enum LocalSoloTestMode
    {
        Idle,
        Automatic,
        Recording,
        Replaying
    }
}
