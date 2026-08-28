using System.Buffers.Binary;
using System.Diagnostics;
using System.IO.Compression;
using System.IO.Pipes;
using System.Net;
using System.Net.Sockets;
using System.Numerics;
using CoopStory.Protocol;
using CoopStory.Sidecar.Configuration;
using CoopStory.Sidecar.Diagnostics;
using CoopStory.Sidecar.Ipc;
using CoopStory.Sidecar.Networking;
using CoopStory.Sidecar.Persistence;
using CoopStory.Sidecar.Session;
using CoopStory.Sidecar.Simulation;

namespace CoopStory.SelfTest;

internal static class Program
{
    private static readonly (string Name, Func<Task> Run)[] Tests =
    [
        ("codec roundtrip and fixed wire header", CodecRoundtripAsync),
        ("codec rejects malformed, oversized and wrong-version frames", CodecRejectionAsync),
        ("lifecycle payload validation matches the bridge", LifecyclePayloadsAsync),
        ("authenticated datagram rejects tampering", DatagramAuthenticationAsync),
        ("stable entity IDs and sequence wrap", EntityAndSequenceAsync),
        ("player interpolation and binary payloads", InterpolationAndPayloadsAsync),
        ("player action transactions enforce wire and authority rules",
            PlayerActionProtocolAsync),
        ("host interaction authority validates revive, restraint and recovery",
            InteractionAuthorityProtocolAsync),
        ("host interaction mutations are negotiated and generation-bound",
            HostInteractionGenerationTransactionsAsync),
        ("AnimGraph state and motion-mode payloads reject malformed data",
            AnimationReplicationPayloadsAsync),
        ("sidecar mission state validates, coalesces and replays monotonically",
            MissionStateSidecarControlAsync),
        ("mission camera snapshots are validated and host-authoritative",
            MissionCameraPresentationProtocolAsync),
        ("mission cinematic FSM payloads reject malformed generations and flags",
            MissionCinematicProtocolAsync),
        ("MetaPed appearance and AnimScene replica payloads are versioned and authoritative",
            AppearanceAndAnimSceneProtocolAsync),
        ("AnimScene definitions remain reliable, authoritative and replayable",
            AnimSceneDefinitionSidecarAsync),
        ("host peer resync batch is atomic and generation-bound",
            HostPeerResyncBatchAtomicAsync),
        ("world and equipment payload codecs and authority", WorldAndEquipmentAsync),
        ("world mirror and damage-intent payload codecs", WorldMirrorPayloadsAsync),
        ("host-authoritative world graph orders dependencies and tombstones",
            AuthoritativeWorldGraphAsync),
        ("player identity validation and reliable refresh", PlayerIdentityAsync),
        ("player inventories and map loot remain independent", PlayerInventoryAsync),
        ("pickup collection payload is fixed, bounded, and itemless",
            PickupCollectionProtocolAsync),
        ("campaign capability journal is idempotent and recovers atomically",
            CapabilityJournalRecoveryAsync),
        ("atomic guest profile write and backup recovery", ProfileRecoveryAsync),
        ("downed and revive state machine", DownedStateAsync),
        ("deterministic network impairment primitives", NetworkImpairmentAsync),
        ("IPC bridge role negotiation", IpcRoleNegotiationAsync),
        ("bounded network-to-bridge pump coalesces without cancelling frames",
            NetworkBridgeDeliveryPumpAsync),
        ("game pipe sends are generation-bound across reconnect",
            BridgePipeGenerationBoundAsync),
        ("bridge inbound handlers cannot cross a logical session reset",
            BridgeInboundGenerationBoundaryAsync),
        ("stalled game pipe is reset without poisoning reconnect",
            BridgePipeStallRecoveryAsync),
        ("sidecar publishes and relays player identity", PlayerIdentityRelayAsync),
        ("sidecars fail fast when motion replication modes differ",
            MotionModeMismatchStopsRuntimeAsync),
        ("guest reconnect replays one local reset before requesting host state",
            GuestReconnectResyncGateAsync),
        ("in-game invite code and session-menu payloads", SessionMenuCodecAsync),
        ("in-game JOIN activates the sidecar after Hello", InGameJoinNegotiationAsync),
        ("bridge Goodbye stops the sidecar fail-closed", BridgeGoodbyeStopsRuntimeAsync),
        ("synthetic puppet course is continuous and exercises gait phases",
            SyntheticPuppetCourseAsync),
        ("synthetic action course couples blocking reloads to motion",
            SyntheticActionCourseAsync),
        ("ghost recording saves atomically and replays validated player samples",
            GhostRecordingStoreAsync),
        ("local game test forwards a synthetic guest to the bridge", LocalGameTestAsync),
        ("TCP authentication and reconnect loopback", TcpReconnectLoopbackAsync),
        ("UDP binding rejects spoofing and replay", UdpBindingAndReplayAsync),
        ("normal host listens on LAN and test host stays loopback-only",
            ListenerIsolationAsync),
        ("paired session configs and host-address safety", SessionConfigPairAsync),
        ("bounded message-flow diagnostics preserve gaps without payloads",
            MessageFlowDiagnosticsAsync),
        ("diagnostic log rotation preserves bounded session history",
            DiagnosticLogRotationAsync),
        ("diagnostics ZIP redacts session credentials", DiagnosticsRedactionAsync),
        ("configuration defaults and validation", ConfigurationAsync)
    ];

    public static async Task<int> Main()
    {
        var failures = new List<string>();
        var started = DateTimeOffset.UtcNow;
        foreach (var test in Tests)
        {
            try
            {
                await test.Run().ConfigureAwait(false);
                Console.WriteLine($"PASS {test.Name}");
            }
            catch (Exception exception)
            {
                failures.Add(test.Name);
                Console.Error.WriteLine(
                    $"FAIL {test.Name}: {exception.GetType().Name}: {exception.Message}");
            }
        }

        var elapsed = DateTimeOffset.UtcNow - started;
        Console.WriteLine(
            $"SELFTEST total={Tests.Length} passed={Tests.Length - failures.Count} " +
            $"failed={failures.Count} elapsedMs={elapsed.TotalMilliseconds:F0}");
        return failures.Count == 0 ? 0 : 1;
    }

    private static async Task CodecRoundtripAsync()
    {
        var original = new ProtocolEnvelope(
            MessageType.MissionState,
            0xAABBCCDD,
            0x0102030405060708,
            new byte[] { 1, 2, 3, 4 });
        var encoded = ProtocolCodec.Encode(original);
        Check.Equal(ProtocolConstants.HeaderSize + 4, encoded.Length);
        Check.Equal(ProtocolConstants.Magic, BinaryPrimitives.ReadUInt32LittleEndian(encoded));
        Check.Equal(
            ProtocolConstants.Version,
            BinaryPrimitives.ReadUInt16LittleEndian(encoded.AsSpan(4)));
        Check.Equal(
            (ushort)MessageType.MissionState,
            BinaryPrimitives.ReadUInt16LittleEndian(encoded.AsSpan(6)));

        var decoded = ProtocolCodec.Decode(encoded);
        Check.Equal(original.Type, decoded.Type);
        Check.Equal(original.Sequence, decoded.Sequence);
        Check.Equal(original.Tick, decoded.Tick);
        Check.SequenceEqual(original.Payload.Span, decoded.Payload.Span);

        await using var stream = new MemoryStream();
        await ProtocolCodec.WriteAsync(stream, original).ConfigureAwait(false);
        stream.Position = 0;
        var streamed = await ProtocolCodec.ReadAsync(stream).ConfigureAwait(false);
        Check.NotNull(streamed);
        Check.Equal(original.Sequence, streamed!.Sequence);
        Check.Null(await ProtocolCodec.ReadAsync(stream).ConfigureAwait(false));
    }

    private static async Task CodecRejectionAsync()
    {
        Check.Throws<ProtocolException>(() => ProtocolCodec.Decode(new byte[23]));

        var valid = ProtocolCodec.Encode(
            new ProtocolEnvelope(MessageType.Heartbeat, 1, 2, ReadOnlyMemory<byte>.Empty));
        var badMagic = valid.ToArray();
        badMagic[0] ^= 0xFF;
        Check.Throws<ProtocolException>(() => ProtocolCodec.Decode(badMagic));

        var badVersion = valid.ToArray();
        BinaryPrimitives.WriteUInt16LittleEndian(
            badVersion.AsSpan(4),
            ProtocolConstants.Version + 1);
        Check.Throws<ProtocolException>(() => ProtocolCodec.Decode(badVersion));

        var oversized = valid.ToArray();
        BinaryPrimitives.WriteUInt32LittleEndian(
            oversized.AsSpan(20),
            ProtocolConstants.MaxPayloadSize + 1u);
        Check.Throws<ProtocolException>(() => ProtocolCodec.Decode(oversized));

        var lengthMismatch = valid.ToArray();
        BinaryPrimitives.WriteUInt32LittleEndian(lengthMismatch.AsSpan(20), 1);
        Check.Throws<ProtocolException>(() => ProtocolCodec.Decode(lengthMismatch));

        var partial = ProtocolCodec.Encode(
            new ProtocolEnvelope(
                MessageType.Error,
                7,
                9,
                new byte[] { 1, 2, 3, 4, 5 }));
        await using var truncated = new MemoryStream(partial[..^1]);
        await Check.ThrowsAsync<EndOfStreamException>(
            async () => _ = await ProtocolCodec.ReadAsync(truncated).ConfigureAwait(false))
            .ConfigureAwait(false);
    }

    private static Task MissionStateSidecarControlAsync()
    {
        var state = new MissionStatePayload(
            NetEntityId.Create(90, 1),
            MissionEpoch: 3,
            Revision: 7,
            CheckpointGeneration: 2,
            MissionPhase.Active,
            MissionStateFlags.MissionActive | MissionStateFlags.AnchorValid,
            new Vector3(10f, 20f, 30f),
            HostHeading: 135f);
        var payload = BinaryPayloadCodec.EncodeMissionState(state);
        var first = new ProtocolEnvelope(
            MessageType.MissionState,
            uint.MaxValue,
            100,
            payload);

        SidecarRuntime.ValidateBinaryControlPayload(first);
        Check.Equal(
            "MissionState/Active/epoch=3/revision=7/checkpoint=2/flags=0x03",
            SidecarRuntime.DescribeControlEnvelope(first));
        Check.False(SidecarRuntime.IsReplaceableBridgeDeliveryType(
            MessageType.MissionState));
        Check.False(SidecarRuntime.IsReplaceableBridgeDeliveryType(
            MessageType.PlayerAction));

        var cache = new AuthoritativeMissionStateCache();
        Check.Equal(
            MissionStateCacheDisposition.Accepted,
            cache.Apply(first).Disposition);
        payload[24] ^= 0x7F;
        Check.Equal(
            state,
            BinaryPayloadCodec.DecodeMissionState(
                cache.Capture()!.Payload.Span));

        var heartbeat = new ProtocolEnvelope(
            MessageType.MissionState,
            0,
            101,
            BinaryPayloadCodec.EncodeMissionState(state));
        Check.Equal(
            MissionStateCacheDisposition.Refreshed,
            cache.Apply(heartbeat).Disposition);
        Check.Equal((uint)0, cache.Capture()!.Sequence);

        var staleState = state with
        {
            MissionEpoch = 2,
            Revision = 99
        };
        Check.Equal(
            MissionStateCacheDisposition.Stale,
            cache.Apply(new ProtocolEnvelope(
                MessageType.MissionState,
                1,
                102,
                BinaryPayloadCodec.EncodeMissionState(staleState)))
                .Disposition);
        Check.Equal((uint)0, cache.Capture()!.Sequence);

        var next = state with { Revision = 8, HostHeading = 140f };
        Check.Equal(
            MissionStateCacheDisposition.Accepted,
            cache.Apply(new ProtocolEnvelope(
                MessageType.MissionState,
                2,
                103,
                BinaryPayloadCodec.EncodeMissionState(next)))
                .Disposition);
        Check.Equal(next, BinaryPayloadCodec.DecodeMissionState(
            cache.Capture()!.Payload.Span));

        var conflictingSameRevision = next with { HostHeading = 141f };
        Check.Throws<ProtocolException>(() => cache.Apply(
            new ProtocolEnvelope(
                MessageType.MissionState,
                3,
                104,
                BinaryPayloadCodec.EncodeMissionState(
                    conflictingSameRevision))));

        var malformed = BinaryPayloadCodec.EncodeMissionState(next);
        malformed[22] = 1;
        Check.Throws<ProtocolException>(() =>
            SidecarRuntime.ValidateBinaryControlPayload(
                new ProtocolEnvelope(
                    MessageType.MissionState,
                    4,
                    105,
                    malformed)));
        Check.Equal(
            "MissionState/Invalid",
            SidecarRuntime.DescribeControlEnvelope(
                new ProtocolEnvelope(
                    MessageType.MissionState,
                    4,
                    105,
                    malformed)));

        cache.Clear();
        Check.Null(cache.Capture());
        return Task.CompletedTask;
    }

    private static Task MissionCameraPresentationProtocolAsync()
    {
        Check.Equal((ushort)31, (ushort)MessageType.MissionCameraState);
        var camera = new MissionCameraStatePayload(
            NetEntityId.Create(90, 1),
            MissionEpoch: 3,
            CinematicGeneration: 8,
            Revision: 19,
            MissionCameraStateFlags.Active |
                MissionCameraStateFlags.SourceRenderingScriptCamera,
            Position: new Vector3(10.25f, -20.5f, 30.75f),
            Rotation: new Vector3(-12f, 0.5f, 181f),
            FieldOfView: 52.5f);
        var encoded = BinaryPayloadCodec.EncodeMissionCameraState(camera);
        Check.Equal(BinaryPayloadCodec.MissionCameraStateSize, encoded.Length);
        Check.Equal(
            camera,
            BinaryPayloadCodec.DecodeMissionCameraState(encoded));

        var inactive = camera with
        {
            Revision = 20,
            Flags = MissionCameraStateFlags.None,
            Position = Vector3.Zero,
            Rotation = Vector3.Zero,
            FieldOfView = 0f
        };
        Check.Equal(
            inactive,
            BinaryPayloadCodec.DecodeMissionCameraState(
                BinaryPayloadCodec.EncodeMissionCameraState(inactive)));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeMissionCameraState(
                camera with { FieldOfView = 0f }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeMissionCameraState(
                camera with { Flags = (MissionCameraStateFlags)0x8000 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeMissionCameraState(
                camera with
                {
                    Flags = MissionCameraStateFlags.Active |
                        MissionCameraStateFlags.ScreenFadedOut |
                        MissionCameraStateFlags.ScreenFadingIn
                }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeMissionCameraState(
                camera with { CinematicGeneration = 0 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeMissionCameraState(
                inactive with { Position = Vector3.One }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodeMissionCameraState(encoded[..^1]));

        var envelope = new ProtocolEnvelope(
            MessageType.MissionCameraState,
            7,
            99,
            encoded);
        SidecarRuntime.ValidateBinaryControlPayload(envelope);
        Check.True(SidecarRuntime.IsReplaceableBridgeDeliveryType(
            MessageType.MissionCameraState));
        Check.True(SidecarRuntime.IsLocalBridgeMessageAuthorized(
            SessionRole.Host,
            MessageType.MissionCameraState));
        Check.False(SidecarRuntime.IsLocalBridgeMessageAuthorized(
            SessionRole.Guest,
            MessageType.MissionCameraState));
        Check.True(SidecarRuntime.IsPeerMessageAuthorized(
            SessionRole.Guest,
            MessageType.MissionCameraState));
        Check.False(SidecarRuntime.IsPeerMessageAuthorized(
            SessionRole.Host,
            MessageType.MissionCameraState));

        var binding = new UdpPeerBinding(IPAddress.Loopback, 43121);
        Check.True(binding.TryAccept(
            new IPEndPoint(IPAddress.Loopback, 43121),
            envelope,
            out var rejectionReason));
        Check.Equal(string.Empty, rejectionReason);

        var reconnectBinding = new UdpPeerBinding(
            IPAddress.Loopback,
            controlSequenceFloor: 100);
        Check.False(reconnectBinding.TryAccept(
            new IPEndPoint(IPAddress.Loopback, 43121),
            envelope with { Sequence = 99 },
            out var delayedReason));
        Check.Equal("sequence-floor", delayedReason);
        Check.Null(reconnectBinding.PinnedEndpoint);
        var freshEndpoint = new IPEndPoint(IPAddress.Loopback, 43122);
        Check.True(reconnectBinding.TryAccept(
            freshEndpoint,
            envelope with { Sequence = 101 },
            out var freshReason));
        Check.Equal(string.Empty, freshReason);
        Check.Equal(
            freshEndpoint,
            reconnectBinding.PinnedEndpoint ??
                throw new SelfTestException(
                    "Fresh reconnect UDP endpoint was not pinned."));
        Check.False(reconnectBinding.TryAccept(
            new IPEndPoint(IPAddress.Loopback, 43121),
            envelope with { Sequence = 102 },
            out var oldPortReason));
        Check.Equal("source-endpoint", oldPortReason);
        return Task.CompletedTask;
    }

    private static Task MissionCinematicProtocolAsync()
    {
        Check.Equal((ushort)27, ProtocolConstants.Version);
        Check.Equal((ushort)35, (ushort)MessageType.MissionCinematicState);
        Check.Equal((ushort)36, (ushort)MessageType.MissionCinematicAction);

        var state = new MissionCinematicStatePayload(
            NetEntityId.Create(91, 1),
            MissionEpoch: 4,
            CinematicGeneration: 2,
            Revision: 7,
            CheckpointGeneration: 3,
            MissionCinematicPhase.PrepareResume,
            MissionCinematicStateFlags.CameraExpected |
                MissionCinematicStateFlags.AnchorValid,
            ResumeAnchor: new Vector3(12f, 34f, 56f),
            ResumeHeading: 87f);
        var stateBytes = BinaryPayloadCodec.EncodeMissionCinematicState(state);
        Check.Equal(BinaryPayloadCodec.MissionCinematicStateSize, stateBytes.Length);
        Check.Equal(state, BinaryPayloadCodec.DecodeMissionCinematicState(stateBytes));
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.EncodeMissionCinematicState(
                state with
                {
                    Phase = MissionCinematicPhase.Playing,
                    Flags = MissionCinematicStateFlags.ResumeTimedOut |
                        MissionCinematicStateFlags.AnchorValid
                }));
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.EncodeMissionCinematicState(
                state with
                {
                    Flags = MissionCinematicStateFlags.None,
                    ResumeAnchor = Vector3.Zero,
                    ResumeHeading = 0f
                }));
        var stateReserved = stateBytes.ToArray();
        stateReserved[44] = 1;
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.DecodeMissionCinematicState(stateReserved));

        var action = new MissionCinematicActionPayload(
            state.HostEntityId,
            state.MissionEpoch,
            state.CinematicGeneration,
            ActionId: 11,
            MissionCinematicActionKind.ResumeReady,
            SenderSlot: 1,
            MissionCinematicActionFlags.FallbackUsed);
        var actionBytes = BinaryPayloadCodec.EncodeMissionCinematicAction(action);
        Check.Equal(BinaryPayloadCodec.MissionCinematicActionSize, actionBytes.Length);
        Check.Equal(action, BinaryPayloadCodec.DecodeMissionCinematicAction(actionBytes));
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.EncodeMissionCinematicAction(
                action with
                {
                    Kind = MissionCinematicActionKind.SkipRequest,
                    Flags = MissionCinematicActionFlags.FallbackUsed
                }));
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.EncodeMissionCinematicAction(
                action with { SenderSlot = 2 }));
        var actionReserved = actionBytes.ToArray();
        actionReserved[31] = 1;
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.DecodeMissionCinematicAction(actionReserved));

        var stateEnvelope = new ProtocolEnvelope(
            MessageType.MissionCinematicState,
            100,
            200,
            stateBytes);
        var actionEnvelope = new ProtocolEnvelope(
            MessageType.MissionCinematicAction,
            101,
            201,
            actionBytes);
        SidecarRuntime.ValidateBinaryControlPayload(stateEnvelope);
        SidecarRuntime.ValidateBinaryControlPayload(actionEnvelope);
        Check.False(SidecarRuntime.IsReplaceableBridgeDeliveryType(
            MessageType.MissionCinematicState));
        Check.False(SidecarRuntime.IsReplaceableBridgeDeliveryType(
            MessageType.MissionCinematicAction));
        Check.True(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Host,
            stateEnvelope));
        Check.True(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Guest,
            stateEnvelope));
        Check.False(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Guest,
            stateEnvelope));
        Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Host,
            stateEnvelope));
        Check.True(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Guest,
            actionEnvelope));
        Check.True(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Host,
            actionEnvelope));
        Check.False(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Host,
            actionEnvelope));
        Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Guest,
            actionEnvelope));
        Check.True(SidecarRuntime.DescribeControlEnvelope(stateEnvelope)
            .Contains("PrepareResume", StringComparison.Ordinal));
        Check.True(SidecarRuntime.DescribeControlEnvelope(actionEnvelope)
            .Contains("ResumeReady", StringComparison.Ordinal));

        var cache = new AuthoritativeMissionCinematicStateCache();
        Check.Equal(
            MissionCinematicStateCacheDisposition.Accepted,
            cache.Apply(stateEnvelope).Disposition);
        Check.Equal(
            MissionCinematicStateCacheDisposition.Refreshed,
            cache.Apply(stateEnvelope with { Sequence = 102 }).Disposition);
        var nextGeneration = state with
        {
            CinematicGeneration = state.CinematicGeneration + 1,
            Revision = 1,
            Phase = MissionCinematicPhase.Playing,
            Flags = MissionCinematicStateFlags.CameraExpected,
            ResumeAnchor = Vector3.Zero,
            ResumeHeading = 0f
        };
        var nextEnvelope = stateEnvelope with
        {
            Sequence = 103,
            Payload = BinaryPayloadCodec.EncodeMissionCinematicState(
                nextGeneration)
        };
        Check.Equal(
            MissionCinematicStateCacheDisposition.Accepted,
            cache.Apply(nextEnvelope).Disposition);
        Check.Equal(
            MissionCinematicStateCacheDisposition.Stale,
            cache.Apply(stateEnvelope with { Sequence = 104 }).Disposition);
        var mutatedSameRevision = nextGeneration with
        {
            Flags = MissionCinematicStateFlags.CameraExpected |
                MissionCinematicStateFlags.SkipPending
        };
        Check.Throws<ProtocolException>(() => cache.Apply(nextEnvelope with
        {
            Sequence = 105,
            Payload = BinaryPayloadCodec.EncodeMissionCinematicState(
                mutatedSameRevision)
        }));
        Check.SequenceEqual(
            nextEnvelope.Payload.ToArray(),
            cache.Capture()?.Payload.ToArray() ?? []);
        cache.Clear();
        Check.Null(cache.Capture());
        return Task.CompletedTask;
    }

    private static Task AppearanceAndAnimSceneProtocolAsync()
    {
        Check.Equal((ushort)27, ProtocolConstants.Version);
        Check.Equal((ushort)37, (ushort)MessageType.PlayerAppearanceState);
        Check.Equal((ushort)38, (ushort)MessageType.AnimSceneReplicaState);
        Check.Equal((ushort)39, (ushort)MessageType.AnimSceneDefinition);
        Check.Equal((ushort)40, (ushort)MessageType.AnimSceneControl);

        var hostId = NetEntityId.Create(77, 1);
        var appearance = new PlayerAppearanceStatePayload(
            hostId,
            Slot: (byte)SessionRole.Host,
            SchemaVersion: 1,
            PlayerAppearanceStateFlags.CompleteComponentSet |
                PlayerAppearanceStateFlags.StoryMetaPed,
            Revision: 4,
            ModelHash: 0xAABBCCDD,
            Fingerprint: 0x0102030405060708,
            ComponentHashes: [0x11111111, 0x22222222, 0x33333333]);
        var appearanceBytes =
            BinaryPayloadCodec.EncodePlayerAppearanceState(appearance);
        var decodedAppearance =
            BinaryPayloadCodec.DecodePlayerAppearanceState(appearanceBytes);
        Check.Equal(appearance.EntityId, decodedAppearance.EntityId);
        Check.Equal(appearance.Fingerprint, decodedAppearance.Fingerprint);
        Check.True(appearance.ComponentHashes.SequenceEqual(
            decodedAppearance.ComponentHashes));
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.EncodePlayerAppearanceState(
                appearance with
                {
                    ComponentHashes = [0x11111111, 0x11111111]
                }));
        var appearanceReserved = appearanceBytes.ToArray();
        appearanceReserved[22] = 1;
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.DecodePlayerAppearanceState(
                appearanceReserved));
        var appearanceEnvelope = new ProtocolEnvelope(
            MessageType.PlayerAppearanceState,
            10,
            100,
            appearanceBytes);
        SidecarRuntime.ValidateBinaryControlPayload(appearanceEnvelope);
        Check.True(SidecarRuntime.IsReplaceableBridgeDeliveryType(
            MessageType.PlayerAppearanceState));
        Check.True(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Host,
            appearanceEnvelope));
        Check.True(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Guest,
            appearanceEnvelope));
        Check.False(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Guest,
            appearanceEnvelope));
        Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Host,
            appearanceEnvelope));

        var scene = new AnimSceneReplicaStatePayload(
            hostId,
            MissionEpoch: 3,
            CinematicGeneration: 2,
            DefinitionRevision: 7,
            Revision: 15,
            DictionaryHash: 0x1234ABCD,
            AnimSceneReplicaStateFlags.Active |
                AnimSceneReplicaStateFlags.Running |
                AnimSceneReplicaStateFlags.Loaded |
                AnimSceneReplicaStateFlags.CameraActive |
                AnimSceneReplicaStateFlags.OriginValid,
            Phase: 0.42f,
            DurationSeconds: 97.5f,
            Rate: 1f,
            OriginPosition: new Vector3(10f, 20f, 30f),
            OriginRotation: new Vector3(0f, 0f, 90f),
            ActiveCameraCount: 1);
        var sceneBytes = BinaryPayloadCodec.EncodeAnimSceneReplicaState(scene);
        Check.Equal(
            scene,
            BinaryPayloadCodec.DecodeAnimSceneReplicaState(sceneBytes));
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.EncodeAnimSceneReplicaState(
                scene with { Phase = 1.5f }));
        var fallbackScene = scene with { DefinitionRevision = 0 };
        Check.Equal(
            fallbackScene,
            BinaryPayloadCodec.DecodeAnimSceneReplicaState(
                BinaryPayloadCodec.EncodeAnimSceneReplicaState(fallbackScene)));
        var sceneReserved = sceneBytes.ToArray();
        sceneReserved[70] = 1;
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.DecodeAnimSceneReplicaState(sceneReserved));

        var definition = new AnimSceneDefinitionPayload(
            hostId,
            MissionEpoch: 3,
            CinematicGeneration: 2,
            DefinitionRevision: 7,
            DictionaryHash: 0x1234ABCD,
            FingerprintLow: 0,
            FingerprintHigh: 0,
            DurationSeconds: 97.5f,
            SceneFlags: 0x10203040,
            CreateOptionFlags: 0x03,
            ResourceName: "script@story@intro",
            PlaybackList: "pl_main",
            Roles:
            [
                new AnimSceneRoleBindingPayload(
                    "Arthur",
                    NetEntityId.Create(77, 2),
                    0xAABBCCDD,
                    AnimSceneRoleKind.Ped,
                    AnimSceneRoleFlags.Required |
                        AnimSceneRoleFlags.Player,
                    0x01),
                new AnimSceneRoleBindingPayload(
                    "Dutch",
                    NetEntityId.Create(77, 3),
                    0x10203040,
                    AnimSceneRoleKind.Horse,
                    AnimSceneRoleFlags.Required,
                    0x02),
                new AnimSceneRoleBindingPayload(
                    "Pickup",
                    NetEntityId.Create(77, 4),
                    0x55667788,
                    AnimSceneRoleKind.Pickup,
                    AnimSceneRoleFlags.None,
                    0x04)
            ]);
        var fingerprint =
            BinaryPayloadCodec.ComputeAnimSceneDefinitionFingerprint(definition);
        Check.True((fingerprint.Low | fingerprint.High) != 0);
        definition = definition with
        {
            FingerprintLow = fingerprint.Low,
            FingerprintHigh = fingerprint.High
        };
        var definitionBytes =
            BinaryPayloadCodec.EncodeAnimSceneDefinition(definition);
        Check.True(
            definitionBytes.Length <=
            BinaryPayloadCodec.AnimSceneDefinitionMaximumSize);
        var decodedDefinition =
            BinaryPayloadCodec.DecodeAnimSceneDefinition(definitionBytes);
        Check.Equal(definition.HostEntityId, decodedDefinition.HostEntityId);
        Check.Equal(definition.FingerprintLow, decodedDefinition.FingerprintLow);
        Check.True(definition.Roles.SequenceEqual(decodedDefinition.Roles));
        Check.Equal(
            AnimSceneRoleKind.Pickup,
            decodedDefinition.Roles[2].Kind);

        var definitionReserved = definitionBytes.ToArray();
        definitionReserved[50] = 1;
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.DecodeAnimSceneDefinition(definitionReserved));
        var definitionCreateReserved = definitionBytes.ToArray();
        definitionCreateReserved[57] = 1;
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.DecodeAnimSceneDefinition(
                definitionCreateReserved));
        var definitionWrongFingerprint = definitionBytes.ToArray();
        definitionWrongFingerprint[24] ^= 0x01;
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.DecodeAnimSceneDefinition(
                definitionWrongFingerprint));
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.EncodeAnimSceneDefinition(definition with
            {
                FingerprintLow = 0,
                FingerprintHigh = 0
            }));
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.ComputeAnimSceneDefinitionFingerprint(
                definition with
                {
                    Roles =
                    [
                        definition.Roles[1],
                        definition.Roles[0],
                        definition.Roles[2]
                    ]
                }));
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.ComputeAnimSceneDefinitionFingerprint(
                definition with
                {
                    Roles =
                    [
                        definition.Roles[0],
                        definition.Roles[1] with
                        {
                            EntityId = definition.Roles[0].EntityId
                        },
                        definition.Roles[2]
                    ]
                }));
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.ComputeAnimSceneDefinitionFingerprint(
                definition with { ResourceName = "script\nintro" }));
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.ComputeAnimSceneDefinitionFingerprint(
                definition with { ResourceName = new string('A', 257) }));
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.ComputeAnimSceneDefinitionFingerprint(
                definition with { PlaybackList = new string('P', 129) }));
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.ComputeAnimSceneDefinitionFingerprint(
                definition with { CreateOptionFlags = 0x04 }));
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.ComputeAnimSceneDefinitionFingerprint(
                definition with
                {
                    Roles = Enumerable.Repeat(
                        definition.Roles[0],
                        BinaryPayloadCodec.AnimSceneDefinitionMaximumRoles + 1)
                        .ToArray()
                }));
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.ComputeAnimSceneDefinitionFingerprint(
                definition with
                {
                    Roles =
                    [
                        definition.Roles[0] with
                        {
                            RoleName = new string('A', 65)
                        }
                    ]
                }));
        var oversizedDefinitionPayload = definitionBytes
            .Concat(new byte[
                BinaryPayloadCodec.AnimSceneDefinitionMaximumSize + 1 -
                definitionBytes.Length])
            .ToArray();
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.DecodeAnimSceneDefinition(
                oversizedDefinitionPayload));

        var playCommit = new AnimSceneControlPayload(
            definition.HostEntityId,
            definition.MissionEpoch,
            definition.CinematicGeneration,
            definition.DefinitionRevision,
            9,
            definition.FingerprintLow,
            definition.FingerprintHigh,
            12_345,
            0.25f,
            1f,
            AnimSceneControlKind.HostPlayCommit,
            (byte)SessionRole.Host,
            AnimSceneControlReason.None,
            AnimSceneControlFlags.LateJoin);
        var playCommitBytes =
            BinaryPayloadCodec.EncodeAnimSceneControl(playCommit);
        Check.Equal(
            playCommit,
            BinaryPayloadCodec.DecodeAnimSceneControl(playCommitBytes));
        var controlReserved = playCommitBytes.ToArray();
        controlReserved[59] = 1;
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.DecodeAnimSceneControl(controlReserved));
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.EncodeAnimSceneControl(playCommit with
            {
                SenderSlot = (byte)SessionRole.Guest
            }));
        var sceneEnvelope = new ProtocolEnvelope(
            MessageType.AnimSceneReplicaState,
            11,
            101,
            sceneBytes);
        SidecarRuntime.ValidateBinaryControlPayload(sceneEnvelope);
        Check.True(SidecarRuntime.IsReplaceableBridgeDeliveryType(
            MessageType.AnimSceneReplicaState));
        Check.True(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Host,
            sceneEnvelope));
        Check.True(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Guest,
            sceneEnvelope));
        Check.False(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Guest,
            sceneEnvelope));
        Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Host,
            sceneEnvelope));

        var animSceneUdpBinding = new UdpPeerBinding(
            IPAddress.Loopback,
            expectedPort: 43121);
        Check.True(
            animSceneUdpBinding.TryAccept(
                new IPEndPoint(IPAddress.Loopback, 43121),
                sceneEnvelope,
                out var animSceneUdpRejection),
            $"UDP binding rejected AnimScene snapshot: {animSceneUdpRejection}");
        return Task.CompletedTask;
    }

    private static async Task AnimSceneDefinitionSidecarAsync()
    {
        var definition = CreateCanonicalAnimSceneDefinition(
            definitionRevision: 7,
            sceneFlags: 0x10203040);
        var definitionBytes =
            BinaryPayloadCodec.EncodeAnimSceneDefinition(definition);
        var originalBytes = definitionBytes.ToArray();
        var definitionEnvelope = new ProtocolEnvelope(
            MessageType.AnimSceneDefinition,
            Sequence: 100,
            Tick: 1_000,
            definitionBytes);

        var cache = new AuthoritativeAnimSceneDefinitionCache();
        var accepted = cache.Apply(definitionEnvelope);
        Check.Equal(
            AnimSceneDefinitionCacheDisposition.Accepted,
            accepted.Disposition);
        Check.Equal(definition.DefinitionRevision, accepted.Key.DefinitionRevision);
        Check.SequenceEqual(
            originalBytes,
            cache.Capture()?.Payload.ToArray() ?? []);

        // Apply freezes the payload used for reconnect replay.
        definitionBytes[0] ^= 0xFF;
        Check.SequenceEqual(
            originalBytes,
            cache.Capture()?.Payload.ToArray() ?? []);

        var staleDefinition = CreateCanonicalAnimSceneDefinition(
            definitionRevision: 6,
            sceneFlags: definition.SceneFlags);
        var staleEnvelope = new ProtocolEnvelope(
            MessageType.AnimSceneDefinition,
            Sequence: 101,
            Tick: 1_001,
            BinaryPayloadCodec.EncodeAnimSceneDefinition(staleDefinition));
        Check.Equal(
            AnimSceneDefinitionCacheDisposition.Stale,
            cache.Apply(staleEnvelope).Disposition);

        var mutatedSameRevision = CreateCanonicalAnimSceneDefinition(
            definitionRevision: definition.DefinitionRevision,
            sceneFlags: definition.SceneFlags ^ 0x01);
        var mutatedEnvelope = new ProtocolEnvelope(
            MessageType.AnimSceneDefinition,
            Sequence: 102,
            Tick: 1_002,
            BinaryPayloadCodec.EncodeAnimSceneDefinition(
                mutatedSameRevision));
        Check.Throws<ProtocolException>(() => cache.Apply(mutatedEnvelope));
        Check.SequenceEqual(
            originalBytes,
            cache.Capture()?.Payload.ToArray() ?? []);

        var refreshedEnvelope = new ProtocolEnvelope(
            MessageType.AnimSceneDefinition,
            Sequence: 103,
            Tick: 1_003,
            originalBytes);
        Check.Equal(
            AnimSceneDefinitionCacheDisposition.Refreshed,
            cache.Apply(refreshedEnvelope).Disposition);
        Check.Equal((uint)103, cache.Capture()?.Sequence ?? 0);
        Check.SequenceEqual(
            originalBytes,
            cache.Capture()?.Payload.ToArray() ?? []);

        var guestReady = new AnimSceneControlPayload(
            definition.HostEntityId,
            definition.MissionEpoch,
            definition.CinematicGeneration,
            definition.DefinitionRevision,
            ActionId: 1,
            definition.FingerprintLow,
            definition.FingerprintHigh,
            PlayAtHostTick: 0,
            StartPhase: 0,
            Rate: 0,
            AnimSceneControlKind.GuestReady,
            SenderSlot: (byte)SessionRole.Guest,
            AnimSceneControlReason.None,
            AnimSceneControlFlags.ResourceLoaded |
                AnimSceneControlFlags.RequiredRolesBound);
        var guestRejected = guestReady with
        {
            ActionId = 2,
            Kind = AnimSceneControlKind.GuestRejected,
            Reason = AnimSceneControlReason.MissingBinding,
            Flags = AnimSceneControlFlags.None
        };
        var hostCommit = guestReady with
        {
            ActionId = 3,
            PlayAtHostTick = 12_345,
            StartPhase = 0.25f,
            Rate = 1f,
            Kind = AnimSceneControlKind.HostPlayCommit,
            SenderSlot = (byte)SessionRole.Host,
            Flags = AnimSceneControlFlags.None
        };
        var hostAbort = guestReady with
        {
            ActionId = 4,
            Kind = AnimSceneControlKind.HostAbort,
            SenderSlot = (byte)SessionRole.Host,
            Reason = AnimSceneControlReason.Superseded,
            Flags = AnimSceneControlFlags.FallbackUsed
        };

        var controls = new[]
        {
            guestReady,
            guestRejected,
            hostCommit,
            hostAbort
        };
        var controlEnvelopes = controls
            .Select((control, index) => new ProtocolEnvelope(
                MessageType.AnimSceneControl,
                Sequence: (uint)(110 + index),
                Tick: (ulong)(1_100 + index),
                BinaryPayloadCodec.EncodeAnimSceneControl(control)))
            .ToArray();
        SidecarRuntime.ValidateBinaryControlPayload(definitionEnvelope with
        {
            Payload = originalBytes
        });
        foreach (var envelope in controlEnvelopes)
        {
            SidecarRuntime.ValidateBinaryControlPayload(envelope);
        }

        Check.True(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Host,
            definitionEnvelope with { Payload = originalBytes }));
        Check.False(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Guest,
            definitionEnvelope with { Payload = originalBytes }));
        Check.True(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Guest,
            definitionEnvelope with { Payload = originalBytes }));
        Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Host,
            definitionEnvelope with { Payload = originalBytes }));

        foreach (var envelope in controlEnvelopes.Take(2))
        {
            Check.True(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
                SessionRole.Guest,
                envelope));
            Check.False(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
                SessionRole.Host,
                envelope));
            Check.True(SidecarRuntime.IsPeerEnvelopeAuthorized(
                SessionRole.Host,
                envelope));
            Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
                SessionRole.Guest,
                envelope));
        }
        foreach (var envelope in controlEnvelopes.Skip(2))
        {
            Check.True(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
                SessionRole.Host,
                envelope));
            Check.False(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
                SessionRole.Guest,
                envelope));
            Check.True(SidecarRuntime.IsPeerEnvelopeAuthorized(
                SessionRole.Guest,
                envelope));
            Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
                SessionRole.Host,
                envelope));
        }

        Check.False(SidecarRuntime.IsReplaceableBridgeDeliveryType(
            MessageType.AnimSceneDefinition));
        Check.False(SidecarRuntime.IsReplaceableBridgeDeliveryType(
            MessageType.AnimSceneControl));
        var udp = new UdpPeerBinding(
            IPAddress.Loopback,
            expectedPort: 43122);
        var udpSource = new IPEndPoint(IPAddress.Loopback, 43122);
        Check.False(udp.TryAccept(
            udpSource,
            definitionEnvelope with { Payload = originalBytes },
            out var definitionUdpRejection));
        Check.Equal("message-type", definitionUdpRejection);
        Check.False(udp.TryAccept(
            udpSource,
            controlEnvelopes[0],
            out var controlUdpRejection));
        Check.Equal("message-type", controlUdpRejection);
        Check.Null(udp.PinnedEndpoint);

        var mismatchedAbort = hostAbort with
        {
            DefinitionRevision = hostAbort.DefinitionRevision + 1
        };
        Check.False(cache.ClearMatching(mismatchedAbort));
        Check.True(cache.Capture() is not null);
        Check.True(cache.ClearMatching(hostAbort));
        Check.Null(cache.Capture());

        var delivered = new List<ProtocolEnvelope>();
        var deliveredSync = new object();
        ValueTask<bool> DeliverAsync(ProtocolEnvelope envelope)
        {
            lock (deliveredSync)
            {
                delivered.Add(envelope);
            }
            return ValueTask.FromResult(true);
        }

        var pump = new NetworkBridgeDeliveryPump(
            DeliverAsync,
            criticalCapacity: 8);
        var orderedFrames = new[]
        {
            PumpEnvelope(MessageType.MissionCinematicState, 200),
            definitionEnvelope with
            {
                Sequence = 201,
                Payload = originalBytes
            },
            definitionEnvelope with
            {
                Sequence = 202,
                Payload = originalBytes
            },
            controlEnvelopes[0] with { Sequence = 203 },
            controlEnvelopes[2] with { Sequence = 204 }
        };
        foreach (var envelope in orderedFrames)
        {
            Check.Equal(
                NetworkBridgeEnqueueDisposition.Queued,
                pump.TryEnqueue(envelope).Disposition);
        }
        var queuedSnapshot = pump.ReadSnapshot();
        Check.Equal(5, queuedSnapshot.Backlog);
        Check.Equal(0L, queuedSnapshot.Coalesced);

        using var stop = new CancellationTokenSource();
        var run = pump.RunAsync(stop.Token);
        await WaitUntilAsync(
                () =>
                {
                    lock (deliveredSync)
                    {
                        return delivered.Count == orderedFrames.Length;
                    }
                },
                TimeSpan.FromSeconds(2),
                "AnimScene definition/control FIFO did not drain.")
            .ConfigureAwait(false);
        await stop.CancelAsync().ConfigureAwait(false);
        await run.WaitAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(false);

        ProtocolEnvelope[] observed;
        lock (deliveredSync)
        {
            observed = delivered.ToArray();
        }
        Check.Equal(
            "200,201,202,203,204",
            string.Join(',', observed.Select(static item => item.Sequence)));
        Check.Equal(
            "MissionCinematicState,AnimSceneDefinition,AnimSceneDefinition,AnimSceneControl,AnimSceneControl",
            string.Join(',', observed.Select(static item => item.Type)));
        Check.Equal(0L, pump.ReadSnapshot().Coalesced);
        Check.Equal(5L, pump.ReadSnapshot().Delivered);

        var replayMission = new MissionStatePayload(
            definition.HostEntityId,
            definition.MissionEpoch,
            Revision: 8,
            CheckpointGeneration: 4,
            MissionPhase.Cutscene,
            MissionStateFlags.MissionActive |
                MissionStateFlags.AnchorValid,
            new Vector3(10f, 20f, 30f),
            HostHeading: 90f);
        var replayMissionEnvelope = new ProtocolEnvelope(
            MessageType.MissionState,
            Sequence: 300,
            Tick: 3_000,
            BinaryPayloadCodec.EncodeMissionState(replayMission));
        var replayCinematic = new MissionCinematicStatePayload(
            definition.HostEntityId,
            definition.MissionEpoch,
            definition.CinematicGeneration,
            Revision: 5,
            CheckpointGeneration: replayMission.CheckpointGeneration,
            MissionCinematicPhase.Playing,
            MissionCinematicStateFlags.CameraExpected,
            ResumeAnchor: Vector3.Zero,
            ResumeHeading: 0f);
        var replayCinematicEnvelope = new ProtocolEnvelope(
            MessageType.MissionCinematicState,
            Sequence: 301,
            Tick: 3_010,
            BinaryPayloadCodec.EncodeMissionCinematicState(
                replayCinematic));

        var mountId = NetEntityId.Create(77, 20);
        var riderId = NetEntityId.Create(77, 21);
        var mount = new WorldEntityStatePayload(
            mountId,
            ModelHash: 0x01020304,
            WorldEntityKind.Ped,
            WorldEntityStateFlags.Horse |
                WorldEntityStateFlags.ScriptOwned,
            WorldCombatTargetSlot.None,
            new Vector3(1f, 2f, 3f),
            Vector3.Zero,
            Heading: 45f,
            HealthFraction: 1f,
            WeaponHash: 0,
            WorldTaskKind.Locomotion);
        var rider = new WorldEntityStatePayload(
            riderId,
            ModelHash: 0x05060708,
            WorldEntityKind.Ped,
            WorldEntityStateFlags.Human |
                WorldEntityStateFlags.Mounted |
                WorldEntityStateFlags.ScriptOwned,
            WorldCombatTargetSlot.None,
            new Vector3(1f, 2f, 4f),
            Vector3.Zero,
            Heading: 45f,
            HealthFraction: 1f,
            WeaponHash: 0,
            WorldTaskKind.Mounted,
            ParentEntityId: mountId,
            TaskTarget: new Vector3(1f, 2f, 4f));
        var replayGraph = new AuthoritativeWorldGraphRegistry();
        Check.Equal(
            WorldGraphApplyDisposition.Applied,
            replayGraph.Apply(new ProtocolEnvelope(
                MessageType.EntityUpdate,
                Sequence: 302,
                Tick: 3_020,
                BinaryPayloadCodec.EncodeWorldEntityState(rider))));
        Check.Equal(
            WorldGraphApplyDisposition.Applied,
            replayGraph.Apply(new ProtocolEnvelope(
                MessageType.EntityUpdate,
                Sequence: 303,
                Tick: 3_030,
                BinaryPayloadCodec.EncodeWorldEntityState(mount))));
        var replayDefinitionEnvelope = definitionEnvelope with
        {
            Sequence = 304,
            Tick = 3_040,
            Payload = originalBytes
        };

        var replayPlan = AuthoritativePeerResyncReplay.Create(
            replayMissionEnvelope,
            replayCinematicEnvelope,
            replayGraph.CaptureSpawnSnapshot(),
            replayDefinitionEnvelope);
        Check.True(replayPlan.IncludesDefinition);
        Check.Equal(
            AuthoritativePeerResyncDefinitionDisposition.Included,
            replayPlan.DefinitionDisposition);
        Check.Equal(
            "MissionState,MissionCinematicState,EntitySpawn,EntitySpawn,AnimSceneDefinition",
            string.Join(',', replayPlan.Frames.Select(
                static frame => frame.Type)));
        Check.Equal(
            mountId,
            BinaryPayloadCodec.DecodeWorldEntityState(
                replayPlan.Frames[2].Payload.Span).EntityId);
        Check.Equal(
            riderId,
            BinaryPayloadCodec.DecodeWorldEntityState(
                replayPlan.Frames[3].Payload.Span).EntityId);

        var replayedFrames = new List<ProtocolEnvelope>();
        var replayResult = await AuthoritativePeerResyncReplay.SendAsync(
            replayPlan,
            (frame, _) =>
            {
                replayedFrames.Add(frame);
                return ValueTask.FromResult(true);
            });
        Check.True(replayResult.Completed);
        Check.True(replayResult.DefinitionDelivered);
        Check.Equal(replayPlan.Frames.Count, replayResult.DeliveredFrames);
        Check.Equal(
            "300,301,303,302,304",
            string.Join(',', replayedFrames.Select(
                static frame => frame.Sequence)));

        var mismatchedCinematicEnvelope = replayCinematicEnvelope with
        {
            Payload = BinaryPayloadCodec.EncodeMissionCinematicState(
                replayCinematic with
                {
                    CinematicGeneration =
                        replayCinematic.CinematicGeneration + 1
                })
        };
        var definitionMismatchPlan =
            AuthoritativePeerResyncReplay.Create(
                replayMissionEnvelope,
                mismatchedCinematicEnvelope,
                replayGraph.CaptureSpawnSnapshot(),
                replayDefinitionEnvelope);
        Check.False(definitionMismatchPlan.IncludesDefinition);
        Check.Equal(
            AuthoritativePeerResyncDefinitionDisposition
                .DefinitionKeyMismatch,
            definitionMismatchPlan.DefinitionDisposition);
        Check.False(definitionMismatchPlan.Frames.Any(
            static frame =>
                frame.Type == MessageType.AnimSceneDefinition));

        var missionMismatchPlan = AuthoritativePeerResyncReplay.Create(
            replayMissionEnvelope with
            {
                Payload = BinaryPayloadCodec.EncodeMissionState(
                    replayMission with
                    {
                        MissionEpoch = replayMission.MissionEpoch + 1
                    })
            },
            replayCinematicEnvelope,
            replayGraph.CaptureSpawnSnapshot(),
            replayDefinitionEnvelope);
        Check.False(missionMismatchPlan.IncludesDefinition);
        Check.Equal(
            AuthoritativePeerResyncDefinitionDisposition
                .MissionCinematicMismatch,
            missionMismatchPlan.DefinitionDisposition);
        Check.False(missionMismatchPlan.Frames.Any(
            static frame =>
                frame.Type == MessageType.AnimSceneDefinition));

        var attemptedBeforeFailure = new List<ProtocolEnvelope>();
        var failedReplay = await AuthoritativePeerResyncReplay.SendAsync(
            replayPlan,
            (frame, _) =>
            {
                attemptedBeforeFailure.Add(frame);
                var isRider = frame.Type == MessageType.EntitySpawn &&
                    BinaryPayloadCodec.DecodeWorldEntityState(
                        frame.Payload.Span).EntityId == riderId;
                return ValueTask.FromResult(!isRider);
            });
        Check.False(failedReplay.Completed);
        Check.True(failedReplay.FailedType == MessageType.EntitySpawn);
        Check.False(failedReplay.DefinitionDelivered);
        Check.False(attemptedBeforeFailure.Any(
            static frame =>
                frame.Type == MessageType.AnimSceneDefinition));
    }

    private static AnimSceneDefinitionPayload
        CreateCanonicalAnimSceneDefinition(
            uint definitionRevision,
            uint sceneFlags)
    {
        var definition = new AnimSceneDefinitionPayload(
            NetEntityId.Create(77, 1),
            MissionEpoch: 3,
            CinematicGeneration: 2,
            definitionRevision,
            DictionaryHash: 0x1234ABCD,
            FingerprintLow: 0,
            FingerprintHigh: 0,
            DurationSeconds: 97.5f,
            sceneFlags,
            CreateOptionFlags: 0x03,
            ResourceName: "script@story@intro",
            PlaybackList: "pl_main",
            Roles:
            [
                new AnimSceneRoleBindingPayload(
                    "Arthur",
                    NetEntityId.Create(77, 2),
                    0xAABBCCDD,
                    AnimSceneRoleKind.Ped,
                    AnimSceneRoleFlags.Required |
                        AnimSceneRoleFlags.Player,
                    0x01),
                new AnimSceneRoleBindingPayload(
                    "Dutch",
                    NetEntityId.Create(77, 3),
                    0x10203040,
                    AnimSceneRoleKind.Horse,
                    AnimSceneRoleFlags.Required,
                    0x02)
            ]);
        var fingerprint =
            BinaryPayloadCodec.ComputeAnimSceneDefinitionFingerprint(
                definition);
        return definition with
        {
            FingerprintLow = fingerprint.Low,
            FingerprintHigh = fingerprint.High
        };
    }

    private static async Task HostPeerResyncBatchAtomicAsync()
    {
        static AuthoritativePeerResyncReplayPlan CreatePlan() => new(
            [
                new ProtocolEnvelope(
                    MessageType.MissionState,
                    Sequence: 1,
                    Tick: 10,
                    ReadOnlyMemory<byte>.Empty),
                new ProtocolEnvelope(
                    MessageType.MissionCinematicState,
                    Sequence: 2,
                    Tick: 20,
                    ReadOnlyMemory<byte>.Empty),
                new ProtocolEnvelope(
                    MessageType.EntitySpawn,
                    Sequence: 3,
                    Tick: 30,
                    ReadOnlyMemory<byte>.Empty),
                new ProtocolEnvelope(
                    MessageType.AnimSceneDefinition,
                    Sequence: 4,
                    Tick: 40,
                    ReadOnlyMemory<byte>.Empty)
            ],
            AuthoritativePeerResyncDefinitionDisposition.Included);

        var atomicPeer = new GenerationBoundLanSession();
        Check.True(atomicPeer.TryCaptureControlPeer(out var atomicToken));
        atomicPeer.BlockNextBoundSend();
        var atomicGate = new PeerControlSendGate();
        var snapshotCaptured = 0;
        var replayTask = atomicGate.RunAsync(
                async cancellationToken =>
                {
                    Interlocked.Increment(ref snapshotCaptured);
                    var plan = CreatePlan();
                    return await AuthoritativePeerResyncReplay.SendAsync(
                            plan,
                            (frame, token) => atomicPeer.SendControlAsync(
                                atomicToken,
                                frame.Type,
                                frame.Payload,
                                frame.Tick,
                                token),
                            cancellationToken)
                        .ConfigureAwait(false);
                })
            .AsTask();
        await atomicPeer.WaitForBlockedSendAsync().ConfigureAwait(false);

        var liveMutationEntered = 0;
        var liveTask = atomicGate.RunAsync(
                async cancellationToken =>
                {
                    Interlocked.Exchange(ref liveMutationEntered, 1);
                    Check.True(atomicPeer.TryCaptureControlPeer(
                        out var liveToken));
                    var despawnDelivered = await atomicPeer.SendControlAsync(
                            liveToken,
                            MessageType.EntityDespawn,
                            ReadOnlyMemory<byte>.Empty,
                            tick: 50,
                            cancellationToken)
                        .ConfigureAwait(false);
                    var terminalDelivered = await atomicPeer.SendControlAsync(
                            liveToken,
                            MessageType.MissionCinematicState,
                            ReadOnlyMemory<byte>.Empty,
                            tick: 60,
                            cancellationToken)
                        .ConfigureAwait(false);
                    return despawnDelivered && terminalDelivered;
                })
            .AsTask();
        await Task.Yield();
        Check.Equal(0, Volatile.Read(ref liveMutationEntered));
        Check.Equal(1, Volatile.Read(ref snapshotCaptured));

        atomicPeer.ReleaseBlockedSend();
        var atomicReplay = await replayTask.ConfigureAwait(false);
        Check.True(atomicReplay.Completed);
        Check.True(await liveTask.ConfigureAwait(false));
        Check.Equal(
            "MissionState:10,MissionCinematicState:20,EntitySpawn:30," +
            "AnimSceneDefinition:40,EntityDespawn:50," +
            "MissionCinematicState:60",
            atomicPeer.DescribeControls(atomicToken.Generation));

        var replacementPeer = new GenerationBoundLanSession();
        Check.True(replacementPeer.TryCaptureControlPeer(
            out var replacedToken));
        replacementPeer.BlockNextBoundSend();
        var replacementGate = new PeerControlSendGate();
        var replacementReplayTask = replacementGate.RunAsync(
                cancellationToken => AuthoritativePeerResyncReplay.SendAsync(
                    CreatePlan(),
                    (frame, token) => replacementPeer.SendControlAsync(
                        replacedToken,
                        frame.Type,
                        frame.Payload,
                        frame.Tick,
                        token),
                    cancellationToken))
            .AsTask();
        await replacementPeer.WaitForBlockedSendAsync().ConfigureAwait(false);
        var replacementToken = replacementPeer.ReplacePeer();
        replacementPeer.ReleaseBlockedSend();
        var replacementReplay = await replacementReplayTask
            .ConfigureAwait(false);
        Check.False(replacementReplay.Completed);
        Check.Equal(1, replacementReplay.DeliveredFrames);
        Check.Equal(
            "MissionState:10",
            replacementPeer.DescribeControls(replacedToken.Generation));
        Check.Equal(
            string.Empty,
            replacementPeer.DescribeControls(replacementToken.Generation));

        var delayedPeer = new GenerationBoundLanSession();
        Check.True(delayedPeer.TryCaptureControlPeer(out var delayedToken));
        var releaseOldHandler = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var delayedCacheMutations = 0;
        var delayedSnapshotCaptures = 0;
        var delayedHandler = Task.Run(async () =>
        {
            await releaseOldHandler.Task.ConfigureAwait(false);
            return await new PeerControlSendGate().RunAsync(
                    async cancellationToken =>
                    {
                        if (!delayedPeer.IsControlPeerCurrent(delayedToken))
                        {
                            return false;
                        }

                        Interlocked.Increment(ref delayedCacheMutations);
                        Interlocked.Increment(ref delayedSnapshotCaptures);
                        return await delayedPeer.SendControlAsync(
                                delayedToken,
                                MessageType.MissionState,
                                ReadOnlyMemory<byte>.Empty,
                                tick: 70,
                                cancellationToken)
                            .ConfigureAwait(false);
                    })
                .ConfigureAwait(false);
        });
        var delayedReplacement = delayedPeer.ReplacePeer();
        releaseOldHandler.TrySetResult(true);
        Check.False(await delayedHandler.ConfigureAwait(false));
        Check.Equal(0, Volatile.Read(ref delayedCacheMutations));
        Check.Equal(0, Volatile.Read(ref delayedSnapshotCaptures));
        Check.Equal(
            string.Empty,
            delayedPeer.DescribeControls(delayedToken.Generation));
        Check.Equal(
            string.Empty,
            delayedPeer.DescribeControls(delayedReplacement.Generation));

        var staleMotionNegotiations = 0;
        if (delayedPeer.IsControlPeerCurrent(delayedToken))
        {
            Interlocked.Increment(ref staleMotionNegotiations);
        }
        Check.Equal(0, Volatile.Read(ref staleMotionNegotiations));
    }

    private static Task LifecyclePayloadsAsync()
    {
        var host = new NetEntityId(0x0102030405060708);
        var guest = new NetEntityId(0x1112131415161718);

        var downed = new DownedStatePayload(
            guest,
            PlayerLifecycle.Downed,
            0.25f);
        Check.Equal(
            downed,
            BinaryPayloadCodec.DecodeDownedState(
                BinaryPayloadCodec.EncodeDownedState(downed)));
        var badReserved = BinaryPayloadCodec.EncodeDownedState(downed);
        badReserved[9] = 1;
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodeDownedState(badReserved));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeDownedState(
                downed with { EntityId = new NetEntityId(1) }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeDownedState(
                downed with { Lifecycle = (PlayerLifecycle)255 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeDownedState(
                downed with { HealthFraction = float.NaN }));

        var request = new ReviveRequestPayload(host, guest);
        Check.Equal(
            request,
            BinaryPayloadCodec.DecodeReviveRequest(
                BinaryPayloadCodec.EncodeReviveRequest(request)));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeReviveRequest(
                request with { TargetId = host }));

        var complete = new ReviveCompletePayload(host, guest, 0.35f);
        Check.Equal(
            complete,
            BinaryPayloadCodec.DecodeReviveComplete(
                BinaryPayloadCodec.EncodeReviveComplete(complete)));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeReviveComplete(
                complete with { HealthFraction = 1.1f }));

        Check.False(
            SidecarRuntime.IsPeerMessageAuthorized(
                SessionRole.Host,
                MessageType.ReviveComplete));
        Check.False(
            SidecarRuntime.IsPeerMessageAuthorized(
                SessionRole.Host,
                MessageType.SpectatorState));
        Check.False(
            SidecarRuntime.IsLocalBridgeMessageAuthorized(
                SessionRole.Guest,
                MessageType.ReviveComplete));
        Check.False(
            SidecarRuntime.IsLocalBridgeMessageAuthorized(
                SessionRole.Guest,
                MessageType.SpectatorState));
        Check.True(
            SidecarRuntime.IsPeerMessageAuthorized(
                SessionRole.Host,
                MessageType.DownedState));
        Check.True(
            SidecarRuntime.IsLocalBridgeMessageAuthorized(
                SessionRole.Guest,
                MessageType.ReviveRequest));
        return Task.CompletedTask;
    }

    private static Task DatagramAuthenticationAsync()
    {
        var credentials = SessionCredentials.Generate();
        var token = credentials.ExportToken();
        var parsed = SessionCredentials.ParseToken(token);
        Check.Equal(credentials.SessionId, parsed.SessionId);

        var envelope = new ProtocolEnvelope(
            MessageType.PlayerState,
            42,
            100,
            new byte[] { 5, 4, 3 });
        var datagram = AuthenticatedDatagramCodec.Encode(envelope, credentials);
        var decoded = AuthenticatedDatagramCodec.Decode(datagram, parsed);
        Check.Equal(envelope.Sequence, decoded.Sequence);

        datagram[^1] ^= 1;
        Check.Throws<ProtocolException>(
            () => AuthenticatedDatagramCodec.Decode(datagram, parsed));

        var nonce = SessionCredentials.CreateNonce();
        var instance = Guid.NewGuid();
        var proof = credentials.CreateClientProof(instance, SessionRole.Guest, nonce);
        Check.True(credentials.VerifyClientProof(
            instance,
            SessionRole.Guest,
            nonce,
            proof));
        Check.False(credentials.VerifyClientProof(
            instance,
            SessionRole.Host,
            nonce,
            proof));
        return Task.CompletedTask;
    }

    private static Task EntityAndSequenceAsync()
    {
        var entity = NetEntityId.Create(0x12345678, 0x9ABCDEF0);
        Check.Equal(0x12345678u, entity.Epoch);
        Check.Equal(0x9ABCDEF0u, entity.Counter);
        Check.True(NetEntityId.TryParse(entity.ToString(), out var parsed));
        Check.Equal(entity, parsed);

        var allocator = new NetEntityIdAllocator(17);
        var firstEntity = allocator.Next();
        Check.Equal(NetEntityId.Create(17, 1), firstEntity);
        Check.True(firstEntity.IsValid);
        Check.False(new NetEntityId(1).IsValid);
        Check.Equal(NetEntityId.Create(17, 2), allocator.Next());

        var tracker = new SequenceTracker();
        Check.True(tracker.TryAccept(uint.MaxValue - 1));
        Check.True(tracker.TryAccept(uint.MaxValue));
        Check.True(tracker.TryAccept(0));
        Check.True(tracker.TryAccept(1));
        Check.False(tracker.TryAccept(1));
        Check.False(tracker.TryAccept(uint.MaxValue));
        Check.True(SequenceNumber.IsNewer(0, uint.MaxValue));

        var replay = new SequenceReplayWindow();
        Check.True(replay.TryAccept(uint.MaxValue - 1));
        Check.True(replay.TryAccept(0));
        Check.True(replay.TryAccept(uint.MaxValue));
        Check.False(replay.TryAccept(uint.MaxValue));
        Check.True(replay.TryAccept(1));
        Check.False(replay.TryAccept(uint.MaxValue - 64));
        Check.Equal(1u, replay.Latest);
        return Task.CompletedTask;
    }

    private static Task InterpolationAndPayloadsAsync()
    {
        var entity = NetEntityId.Create(5, 1);
        var older = new PlayerStatePayload(
            entity,
            1,
            PlayerLifecycle.Alive,
            Vector3.Zero,
            Vector3.UnitX,
            350f,
            1f,
            PlayerStateFlags.InMission);
        var newer = older with
        {
            Position = new Vector3(10, 20, 30),
            Velocity = Vector3.UnitY,
            Heading = 10f,
            HealthFraction = 0.5f
        };

        var buffer = new PlayerStateInterpolationBuffer();
        Check.True(buffer.TryAdd(100, 900, older));
        Check.False(buffer.TryAdd(100, 900, older));
        Check.True(buffer.TryAdd(101, 1100, newer));
        Check.True(buffer.TrySample(1100, 100, out var sampled));
        Check.Near(5.025f, sampled.Position.X);
        Check.Near(9.975f, sampled.Position.Y);
        Check.Near(15f, sampled.Position.Z);
        Check.Near(0f, sampled.Heading);
        Check.Near(0.75f, sampled.HealthFraction);

        var encodedPlayer = BinaryPayloadCodec.EncodePlayerState(newer);
        Check.Equal(BinaryPayloadCodec.PlayerStateSize, encodedPlayer.Length);
        Check.Equal(newer, BinaryPayloadCodec.DecodePlayerState(encodedPlayer));
        var aiming = newer with
        {
            Flags =
                newer.Flags |
                PlayerStateFlags.Aiming |
                PlayerStateFlags.Firing |
                PlayerStateFlags.AimTargetValid |
                PlayerStateFlags.MeleeCombat |
                PlayerStateFlags.PeerCombatTarget |
                PlayerStateFlags.PeerLassoActive |
                PlayerStateFlags.MeleeBlocking |
                PlayerStateFlags.MeleeGrappling |
                PlayerStateFlags.PeerKnockdown |
                PlayerStateFlags.StealthMovement |
                PlayerStateFlags.InCover |
                PlayerStateFlags.GoingIntoCover |
                PlayerStateFlags.CoverFacingLeft |
                PlayerStateFlags.AimingFromCover |
                PlayerStateFlags.InWater |
                PlayerStateFlags.Swimming |
                PlayerStateFlags.SwimmingUnderwater |
                PlayerStateFlags.SyntheticTest,
            AimTarget = new Vector3(11f, -22f, 33.5f),
            FireSequence = 0xA1B2C3D4,
            MovementHeading = 25f,
            LocalForwardSpeed = 3.25f,
            LocalRightSpeed = -0.75f,
            DesiredMoveBlend = 2f,
            LocomotionEpoch = 5,
            TraversalActionId = 7,
            TraversalKind = PlayerTraversalKind.Jump,
            LocomotionMode = PlayerLocomotionMode.Traversal,
            TraversalAnchor = new Vector3(9f, 8f, 7f),
            TraversalHeading = 30f
        };
        var encodedAiming = BinaryPayloadCodec.EncodePlayerState(aiming);
        Check.Equal(104, encodedAiming.Length);
        Check.Equal(
            aiming.AimTarget.X,
            BinaryPrimitives.ReadSingleLittleEndian(encodedAiming.AsSpan(48)));
        Check.Equal(
            aiming.AimTarget.Y,
            BinaryPrimitives.ReadSingleLittleEndian(encodedAiming.AsSpan(52)));
        Check.Equal(
            aiming.AimTarget.Z,
            BinaryPrimitives.ReadSingleLittleEndian(encodedAiming.AsSpan(56)));
        Check.Equal(
            aiming.FireSequence,
            BinaryPrimitives.ReadUInt32LittleEndian(encodedAiming.AsSpan(60)));
        Check.Equal(
            aiming.LocomotionEpoch,
            BinaryPrimitives.ReadUInt16LittleEndian(encodedAiming.AsSpan(80)));
        Check.Equal((byte)PlayerTraversalKind.Jump, encodedAiming[84]);
        Check.Equal((byte)PlayerLocomotionMode.Traversal, encodedAiming[85]);
        Check.Equal(
            aiming,
            BinaryPayloadCodec.DecodePlayerState(encodedAiming));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerState(
                newer with { EntityId = new NetEntityId(1) }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerState(
                newer with { Slot = 2 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerState(
                newer with { AimTarget = Vector3.One }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerState(
                aiming with { AimTarget = new Vector3(float.NaN, 0, 0) }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerState(
                aiming with { TraversalActionId = 0 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerState(
                aiming with { DesiredMoveBlend = 3.1f }));

        var traversal = new PlayerTraversalPayload(
            entity,
            (byte)SessionRole.Guest,
            PlayerTraversalKind.Climb,
            ActionId: 7,
            Revision: 2,
            LocomotionEpoch: 5,
            PlayerTraversalFlags.InputEdgeDetected |
                PlayerTraversalFlags.ObstacleValid |
                PlayerTraversalFlags.ExpectedLandingValid,
            TakeoffHeading: 30f,
            TakeoffPosition: new Vector3(9f, 8f, 7f),
            ApproachVelocity: new Vector3(3f, 2f, 0f),
            ObstaclePoint: new Vector3(10f, 8f, 7.5f),
            ObstacleNormal: new Vector3(-1f, 0f, 0f),
            ObstacleTopZ: 7.5f,
            ExpectedLanding: new Vector3(11f, 8f, 7f));
        var encodedTraversal =
            BinaryPayloadCodec.EncodePlayerTraversal(traversal);
        Check.Equal(
            BinaryPayloadCodec.PlayerTraversalSize,
            encodedTraversal.Length);
        Check.Equal(
            traversal,
            BinaryPayloadCodec.DecodePlayerTraversal(encodedTraversal));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerTraversal(
                traversal with
                {
                    Flags = PlayerTraversalFlags.None,
                    ObstaclePoint = Vector3.One,
                    ObstacleNormal = Vector3.Zero,
                    ObstacleTopZ = 0,
                    ExpectedLanding = Vector3.Zero
                }));

        foreach (var opcode in new[]
                 {
                     CommandOpcode.ToggleDiagnostics,
                     CommandOpcode.ResyncEquipment,
                     CommandOpcode.DiagnosticMarker
                 })
        {
            var command = new CommandPayload(
                opcode,
                CommandFlags.Acknowledge,
                entity,
                new Vector3(1, 2, 3),
                90f,
                7f);
            var encodedCommand = BinaryPayloadCodec.EncodeCommand(command);
            Check.Equal(32, encodedCommand.Length);
            Check.Equal(command, BinaryPayloadCodec.DecodeCommand(encodedCommand));
        }

        return Task.CompletedTask;
    }

    private static Task PlayerActionProtocolAsync()
    {
        Check.Equal((ushort)27, ProtocolConstants.Version);
        Check.Equal((ushort)30, (ushort)MessageType.PlayerAction);

        var guestId = NetEntityId.Create(0x11223344, 2);
        var hostId = NetEntityId.Create(0x11223344, 1);
        const PlayerActionFlags semanticFlags =
            PlayerActionFlags.Intent |
            PlayerActionFlags.TargetEntityValid |
            PlayerActionFlags.TargetPointValid |
            PlayerActionFlags.ActorAnchorValid |
            PlayerActionFlags.Persistent |
            PlayerActionFlags.PhysicalTargetEffect |
            PlayerActionFlags.VariantValid |
            PlayerActionFlags.AnimationSampleValid |
            PlayerActionFlags.NormalizedPhaseValid;
        var guestIntent = new PlayerActionPayload(
            guestId,
            hostId,
            Sequence: 9,
            ActionId: 11,
            Revision: 2,
            ActorSlot: (byte)SessionRole.Guest,
            AuthoritySlot: (byte)SessionRole.Guest,
            PlayerActionKind.Grapple,
            PlayerActionPhase.Active,
            semanticFlags,
            DurationMilliseconds: 1_000,
            PhaseElapsedMilliseconds: 250,
            WeaponHash: 0xAABBCCDD,
            VariantHash: 0x01020304,
            AnimationSampleSequence: 77,
            ActorAnchor: new Vector3(1, 2, 3),
            TargetPoint: new Vector3(4, 5, 6),
            FacingHeading: 45,
            NormalizedPhase: 0.25f);

        var encoded = BinaryPayloadCodec.EncodePlayerAction(guestIntent);
        Check.Equal(BinaryPayloadCodec.PlayerActionSize, encoded.Length);
        Check.Equal((byte)PlayerActionKind.Grapple, encoded[28]);
        Check.Equal((byte)PlayerActionPhase.Active, encoded[29]);
        Check.Equal(
            (uint)semanticFlags,
            BinaryPrimitives.ReadUInt32LittleEndian(encoded.AsSpan(32)));
        Check.Equal(
            guestIntent,
            BinaryPayloadCodec.DecodePlayerAction(encoded));

        var reserved = encoded.ToArray();
        reserved[30] = 1;
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodePlayerAction(reserved));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerAction(
                guestIntent with { Sequence = 0 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerAction(
                guestIntent with { ActionId = 0 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerAction(
                guestIntent with { Revision = 0 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerAction(
                guestIntent with
                {
                    Flags =
                        guestIntent.Flags |
                        PlayerActionFlags.Authoritative
                }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerAction(
                guestIntent with
                {
                    Flags =
                        guestIntent.Flags &
                        ~PlayerActionFlags.Intent
                }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerAction(
                guestIntent with { TargetEntityId = NetEntityId.None }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerAction(
                guestIntent with
                {
                    TargetEntityId = new NetEntityId(1),
                    Flags =
                        guestIntent.Flags &
                        ~PlayerActionFlags.TargetEntityValid &
                        ~PlayerActionFlags.PhysicalTargetEffect
                }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerAction(
                guestIntent with
                {
                    ActorAnchor = new Vector3(float.NaN, 0, 0)
                }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerAction(
                guestIntent with { NormalizedPhase = 1.01f }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerAction(
                guestIntent with
                {
                    PhaseElapsedMilliseconds = 1_001
                }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerAction(
                guestIntent with
                {
                    Flags =
                        guestIntent.Flags |
                        PlayerActionFlags.ResyncSnapshot
                }));

        ProtocolEnvelope Envelope(PlayerActionPayload action) =>
            new(
                MessageType.PlayerAction,
                action.Sequence,
                123,
                BinaryPayloadCodec.EncodePlayerAction(action));

        var guestIntentEnvelope = Envelope(guestIntent);
        Check.True(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Guest,
            guestIntentEnvelope));
        Check.True(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Host,
            guestIntentEnvelope));
        Check.False(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Host,
            guestIntentEnvelope));
        Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Guest,
            guestIntentEnvelope));

        var hostAuthoritative = guestIntent with
        {
            ActorEntityId = hostId,
            TargetEntityId = guestId,
            ActorSlot = (byte)SessionRole.Host,
            AuthoritySlot = (byte)SessionRole.Host,
            Flags =
                (guestIntent.Flags & ~PlayerActionFlags.Intent) |
                PlayerActionFlags.Authoritative
        };
        var hostAuthoritativeEnvelope = Envelope(hostAuthoritative);
        Check.True(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Host,
            hostAuthoritativeEnvelope));
        Check.True(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Guest,
            hostAuthoritativeEnvelope));
        Check.False(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Guest,
            hostAuthoritativeEnvelope));
        Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Host,
            hostAuthoritativeEnvelope));

        var hostResolvedGuest = hostAuthoritative with
        {
            ActorEntityId = guestId,
            TargetEntityId = hostId,
            ActorSlot = (byte)SessionRole.Guest
        };
        var hostResolvedGuestEnvelope = Envelope(hostResolvedGuest);
        Check.True(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Host,
            hostResolvedGuestEnvelope));
        Check.True(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Guest,
            hostResolvedGuestEnvelope));
        Check.False(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Guest,
            hostResolvedGuestEnvelope));
        Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Host,
            hostResolvedGuestEnvelope));

        var hostIntent = guestIntent with
        {
            ActorEntityId = hostId,
            TargetEntityId = guestId,
            ActorSlot = (byte)SessionRole.Host,
            AuthoritySlot = (byte)SessionRole.Host
        };
        var hostIntentEnvelope = Envelope(hostIntent);
        Check.False(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Host,
            hostIntentEnvelope));
        Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Guest,
            hostIntentEnvelope));

        // The host accepts only a legal, revisioned lifecycle before either
        // bridge is allowed to create a physics-sensitive task. This makes a
        // delayed combat/lasso packet harmless instead of letting it revive a
        // completed action on the other player's replica.
        var actionAuthority = new AuthoritativePlayerActionStateMachine();
        var grappleBegin = hostResolvedGuest with
        {
            ActionId = 51,
            Revision = 1,
            Kind = PlayerActionKind.Grapple,
            Phase = PlayerActionPhase.Begin,
            Flags = PlayerActionFlags.Authoritative |
                PlayerActionFlags.ActorAnchorValid,
            TargetEntityId = NetEntityId.None,
            TargetPoint = Vector3.Zero,
            DurationMilliseconds = 800,
            PhaseElapsedMilliseconds = 0,
            VariantHash = 0,
            AnimationSampleSequence = 0,
            NormalizedPhase = 0
        };
        Check.True(actionAuthority.TryAuthorize(grappleBegin, out _));
        var grappleImpact = grappleBegin with
        {
            Revision = 2,
            Phase = PlayerActionPhase.Impact,
            PhaseElapsedMilliseconds = 300
        };
        Check.True(actionAuthority.TryAuthorize(grappleImpact, out _));
        Check.False(actionAuthority.TryAuthorize(
            grappleImpact with { Revision = 3, Phase = PlayerActionPhase.Begin },
            out var invalidTransition));
        Check.Equal("illegal-action-phase-transition", invalidTransition);
        var grappleEnd = grappleImpact with
        {
            Revision = 3,
            Phase = PlayerActionPhase.End
        };
        Check.True(actionAuthority.TryAuthorize(grappleEnd, out _));
        Check.False(actionAuthority.TryAuthorize(
            grappleEnd with { Revision = 4, Phase = PlayerActionPhase.Sustain },
            out var afterTerminal));
        Check.Equal("continuation-after-terminal-action", afterTerminal);

        var lassoResync = grappleBegin with
        {
            ActionId = 52,
            Revision = 1,
            Kind = PlayerActionKind.Lasso,
            Phase = PlayerActionPhase.Snapshot,
            Flags = PlayerActionFlags.Authoritative |
                PlayerActionFlags.Persistent |
                PlayerActionFlags.ResyncSnapshot |
                PlayerActionFlags.ActorAnchorValid
        };
        Check.True(actionAuthority.TryAuthorize(lassoResync, out _));
        Check.False(actionAuthority.TryAuthorize(
            grappleBegin with
            {
                ActionId = 53,
                Kind = PlayerActionKind.Aim,
                Flags = PlayerActionFlags.Authoritative |
                    PlayerActionFlags.ActorAnchorValid |
                    PlayerActionFlags.PhysicalTargetEffect
            },
            out var invalidPhysicalEffect));
        Check.Equal("physical-effect-not-valid-for-action", invalidPhysicalEffect);

        return Task.CompletedTask;
    }

    private static Task InteractionAuthorityProtocolAsync()
    {
        Check.Equal((ushort)27, ProtocolConstants.Version);
        Check.Equal((ushort)32, (ushort)MessageType.InteractionIntent);
        Check.Equal((ushort)33, (ushort)MessageType.InteractionResult);
        Check.Equal((ushort)34, (ushort)MessageType.RestraintState);

        var hostId = NetEntityId.Create(0x55667788, 1);
        var guestId = NetEntityId.Create(0x55667788, 2);
        var revive = new InteractionIntentPayload(
            hostId,
            guestId,
            NetEntityId.None,
            InteractionId: 1,
            Revision: 1,
            ActorSlot: (byte)SessionRole.Host,
            InteractionKind.Revive,
            InteractionIntentPhase.Begin,
            InteractionIntentFlags.TargetPlayer |
                InteractionIntentFlags.HoldRequired,
            RequestedDurationMilliseconds: 4_000);
        var reviveBytes = BinaryPayloadCodec.EncodeInteractionIntent(revive);
        Check.Equal(BinaryPayloadCodec.InteractionIntentSize, reviveBytes.Length);
        Check.Equal(revive, BinaryPayloadCodec.DecodeInteractionIntent(reviveBytes));
        var badReserved = reviveBytes.ToArray();
        badReserved[34] = 1;
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.DecodeInteractionIntent(badReserved));
        Check.Throws<ProtocolException>(() =>
            BinaryPayloadCodec.EncodeInteractionIntent(
                revive with { ActorEntityId = guestId }));

        var receivedAt = Environment.TickCount64;
        var snapshots = new Dictionary<NetEntityId, ReplicatedPlayerSnapshot>
        {
            [hostId] = new(
                new PlayerStatePayload(
                    hostId,
                    (byte)SessionRole.Host,
                    PlayerLifecycle.Alive,
                    Vector3.Zero,
                    Vector3.Zero,
                    0,
                    1,
                    PlayerStateFlags.None),
                0,
                receivedAt),
            [guestId] = new(
                new PlayerStatePayload(
                    guestId,
                    (byte)SessionRole.Guest,
                    PlayerLifecycle.Downed,
                    new Vector3(1, 0, 0),
                    Vector3.Zero,
                    0,
                    0.01f,
                    PlayerStateFlags.None),
                0,
                receivedAt)
        };
        ReplicatedPlayerSnapshot? Lookup(NetEntityId id) =>
            snapshots.TryGetValue(id, out var snapshot) ? snapshot : null;

        var authority = new AuthoritativeInteractionRegistry();
        var begin = authority.Resolve(revive, 1_000, Lookup);
        Check.Equal(InteractionResultStatus.Accepted, begin.Result.Status);
        Check.Equal((uint)4_000, begin.Result.RequiredDurationMilliseconds);
        var progress = begin;
        for (var revision = 2; revision <= 11; revision++)
        {
            progress = authority.Resolve(
                revive with
                {
                    Revision = (ushort)revision,
                    Phase = InteractionIntentPhase.Sustain
                },
                1_000 + ((revision - 1) * 400),
                Lookup);
        }
        Check.Equal(InteractionResultStatus.Completed, progress.Result.Status);
        Check.Equal((uint)4_000, progress.Result.ProgressMilliseconds);
        var completedBytes = BinaryPayloadCodec.EncodeInteractionResult(
            progress.Result);
        Check.Equal(
            progress.Result,
            BinaryPayloadCodec.DecodeInteractionResult(completedBytes));

        var lasso = new PlayerActionPayload(
            hostId,
            guestId,
            Sequence: 1,
            ActionId: 44,
            Revision: 1,
            ActorSlot: (byte)SessionRole.Host,
            AuthoritySlot: (byte)SessionRole.Host,
            PlayerActionKind.Lasso,
            PlayerActionPhase.Active,
            PlayerActionFlags.Authoritative |
                PlayerActionFlags.TargetEntityValid |
                PlayerActionFlags.PhysicalTargetEffect,
            DurationMilliseconds: 0,
            PhaseElapsedMilliseconds: 0,
            WeaponHash: 0,
            VariantHash: 0,
            AnimationSampleSequence: 0,
            ActorAnchor: default,
            TargetPoint: default,
            FacingHeading: 0,
            NormalizedPhase: 0);
        var restrained = authority.ObserveAuthoritativePlayerAction(lasso);
        Check.NotNull(restrained);
        Check.Equal(PlayerRestraintState.Lassoed, restrained!.Value.State);
        var heartbeat = authority.ObserveAuthoritativePlayerAction(
            lasso with { Phase = PlayerActionPhase.Sustain, Revision = 2 });
        Check.NotNull(heartbeat);
        Check.Equal(restrained.Value.Revision, heartbeat!.Value.Revision);
        Check.True(heartbeat.Value.Flags.HasFlag(RestraintStateFlags.Snapshot));
        var restraintBytes = BinaryPayloadCodec.EncodeRestraintState(
            restrained.Value);
        Check.Equal(
            restrained.Value,
            BinaryPayloadCodec.DecodeRestraintState(restraintBytes));

        // The bridge intentionally sends terminal actions after RDR2 has
        // cleared its process-local target handle. Authority must still release
        // the restraint by actor + action id.
        var forgedGuestReleaseOfHostRestraint = lasso with
        {
            TargetEntityId = NetEntityId.None,
            Revision = 3,
            ActorSlot = (byte)SessionRole.Guest,
            Phase = PlayerActionPhase.Cancel,
            Flags = PlayerActionFlags.Intent
        };
        Check.False(
            authority.HasMatchingTerminalRestraint(
                forgedGuestReleaseOfHostRestraint),
            "Guest terminal proof must not match a host-owned restraint even when actor/action ids collide.");
        var terminalRelease = authority.ObserveAuthoritativePlayerAction(
            lasso with
            {
                TargetEntityId = NetEntityId.None,
                Revision = 3,
                Phase = PlayerActionPhase.End,
                Flags = PlayerActionFlags.Authoritative
            });
        Check.NotNull(terminalRelease);
        Check.Equal(PlayerRestraintState.Free, terminalRelease!.Value.State);

        restrained = authority.ObserveAuthoritativePlayerAction(
            lasso with { Sequence = 2, ActionId = 45, Revision = 1 });
        Check.NotNull(restrained);

        var release = new InteractionIntentPayload(
            hostId,
            guestId,
            NetEntityId.None,
            InteractionId: 2,
            Revision: 1,
            ActorSlot: (byte)SessionRole.Host,
            InteractionKind.ReleaseRestraint,
            InteractionIntentPhase.Begin,
            InteractionIntentFlags.TargetPlayer,
            RequestedDurationMilliseconds: 0);
        var released = authority.Resolve(release, 5_100, Lookup);
        Check.Equal(InteractionResultStatus.Completed, released.Result.Status);
        Check.Equal(PlayerRestraintState.Free, released.RestraintState!.Value.State);

        // A peer disconnect can release the restraint while the game pipe is
        // unavailable. The terminal Free tombstone must remain replayable on
        // the next Hello instead of disappearing from the snapshot.
        var disconnectedAuthority = new AuthoritativeInteractionRegistry();
        var disconnectedBound = restrained ?? throw new SelfTestException(
            "Expected a bound restraint before disconnect cleanup.");
        Check.True(disconnectedAuthority.ApplyAuthoritativeRestraint(
            disconnectedBound));
        var disconnectReleases = disconnectedAuthority.Clear(
            emitFreeStates: true);
        Check.Equal(1, disconnectReleases.Count);
        Check.Equal(
            PlayerRestraintState.Free,
            disconnectReleases[0].State);
        var reconnectSnapshot = disconnectedAuthority.CaptureRestraints();
        Check.Equal(1, reconnectSnapshot.Count);
        Check.Equal(
            PlayerRestraintState.Free,
            reconnectSnapshot[0].State);
        Check.Equal(
            disconnectReleases[0].Revision,
            reconnectSnapshot[0].Revision);
        Check.True(reconnectSnapshot[0].Flags.HasFlag(
            RestraintStateFlags.Snapshot));

        var recover = new InteractionIntentPayload(
            hostId,
            hostId,
            NetEntityId.None,
            InteractionId: 3,
            Revision: 1,
            ActorSlot: (byte)SessionRole.Host,
            InteractionKind.EmergencyRecover,
            InteractionIntentPhase.Begin,
            InteractionIntentFlags.TargetPlayer,
            RequestedDurationMilliseconds: 0);
        var recovered = authority.Resolve(recover, 5_200, Lookup);
        Check.Equal(InteractionResultStatus.Completed, recovered.Result.Status);
        var recoveredBytes = BinaryPayloadCodec.EncodeInteractionResult(
            recovered.Result);
        Check.Equal(
            recovered.Result,
            BinaryPayloadCodec.DecodeInteractionResult(recoveredBytes));
        var duplicate = authority.Resolve(recover, 5_201, Lookup);
        Check.Equal(recovered.Result, duplicate.Result);

        snapshots[guestId] = snapshots[guestId] with
        {
            State = snapshots[guestId].State with
            {
                Position = new Vector3(20, 0, 0),
                Lifecycle = PlayerLifecycle.Alive
            }
        };
        var tooFar = authority.Resolve(
            release with
            {
                InteractionId = 4,
                Kind = InteractionKind.DismountPeer
            },
            5_300,
            Lookup);
        Check.Equal(InteractionResultStatus.Rejected, tooFar.Result.Status);
        Check.Equal(InteractionRejectReason.TooFar, tooFar.Result.RejectReason);

        var snapshotState = authority.ReadSnapshot();
        Check.True(snapshotState.Completed >= 3);
        Check.True(snapshotState.Duplicate >= 1);
        Check.True(snapshotState.Rejected >= 1);
        return Task.CompletedTask;
    }

    private static async Task HostInteractionGenerationTransactionsAsync()
    {
        var replacementBeforeMutation = new GenerationBoundLanSession();
        Check.True(replacementBeforeMutation.TryCaptureControlPeer(
            out var negotiatedPeer));
        var unnegotiatedReplacement = replacementBeforeMutation.ReplacePeer();
        var rejectedMutations = 0;
        var rejectedDeliveries = 0;
        var rejected = await NegotiatedPeerMutationTransaction.RunAsync(
                new PeerControlSendGate(),
                () => replacementBeforeMutation,
                (network, peer) =>
                    network.IsControlPeerCurrent(peer) &&
                    peer == negotiatedPeer,
                () => Interlocked.Increment(ref rejectedMutations),
                (_, _, _, _) =>
                {
                    Interlocked.Increment(ref rejectedDeliveries);
                    return ValueTask.CompletedTask;
                })
            .ConfigureAwait(false);
        Check.False(rejected);
        Check.Equal(0, Volatile.Read(ref rejectedMutations));
        Check.Equal(0, Volatile.Read(ref rejectedDeliveries));
        Check.Equal(
            string.Empty,
            replacementBeforeMutation.DescribeControls(
                unnegotiatedReplacement.Generation));

        var replacementDuringDelivery = new GenerationBoundLanSession();
        Check.True(replacementDuringDelivery.TryCaptureControlPeer(
            out var originalPeer));
        replacementDuringDelivery.BlockNextBoundSend();
        var transactionGate = new PeerControlSendGate();
        var restraintState = "Free";
        var transactionOrder = new List<string>();
        var transaction = NegotiatedPeerMutationTransaction.RunAsync(
                transactionGate,
                () => replacementDuringDelivery,
                (network, peer) =>
                    network.IsControlPeerCurrent(peer) &&
                    peer == originalPeer,
                () =>
                {
                    restraintState = "Bound";
                    transactionOrder.Add("mutation:Bound");
                    return MessageType.RestraintState;
                },
                async (network, peer, type, token) =>
                {
                    transactionOrder.Add("delivery:start");
                    var delivered = await network.SendControlAsync(
                            peer,
                            type,
                            ReadOnlyMemory<byte>.Empty,
                            tick: 10,
                            token)
                        .ConfigureAwait(false);
                    transactionOrder.Add($"delivery:{delivered}");
                })
            .AsTask();
        await replacementDuringDelivery.WaitForBlockedSendAsync()
            .ConfigureAwait(false);

        var cleanupEntered = 0;
        var cleanup = transactionGate.RunAsync(
                _ =>
                {
                    Interlocked.Exchange(ref cleanupEntered, 1);
                    restraintState = "Free";
                    transactionOrder.Add("cleanup:Free");
                    return ValueTask.FromResult(true);
                })
            .AsTask();
        await Task.Yield();
        Check.Equal(0, Volatile.Read(ref cleanupEntered));

        var replacementPeer = replacementDuringDelivery.ReplacePeer();
        replacementDuringDelivery.ReleaseBlockedSend();
        Check.True(await transaction.ConfigureAwait(false));
        Check.True(await cleanup.ConfigureAwait(false));
        Check.Equal("Free", restraintState);
        Check.Equal(
            "mutation:Bound,delivery:start,delivery:True,cleanup:Free",
            string.Join(',', transactionOrder));
        Check.Equal(
            "RestraintState:10",
            replacementDuringDelivery.DescribeControls(
                originalPeer.Generation));
        Check.Equal(
            string.Empty,
            replacementDuringDelivery.DescribeControls(
                replacementPeer.Generation));

        var replayGate = new PeerControlSendGate();
        var replayState = "Bound";
        var bridgeTransitions = new List<string>();
        var replayEntered = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseReplay = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var replay = replayGate.RunAsync(
                async _ =>
                {
                    var captured = replayState;
                    replayEntered.TrySetResult(true);
                    await releaseReplay.Task.ConfigureAwait(false);
                    if (captured == "Bound")
                    {
                        bridgeTransitions.Add("Bound");
                    }
                    return true;
                })
            .AsTask();
        await replayEntered.Task.ConfigureAwait(false);
        cleanupEntered = 0;
        var replayCleanup = replayGate.RunAsync(
                _ =>
                {
                    Interlocked.Exchange(ref cleanupEntered, 1);
                    replayState = "Free";
                    bridgeTransitions.Add("Free");
                    return ValueTask.FromResult(true);
                })
            .AsTask();
        await Task.Yield();
        Check.Equal(0, Volatile.Read(ref cleanupEntered));
        releaseReplay.TrySetResult(true);
        Check.True(await replay.ConfigureAwait(false));
        Check.True(await replayCleanup.ConfigureAwait(false));
        Check.Equal("Free", replayState);
        Check.Equal("Bound,Free", string.Join(',', bridgeTransitions));

        replayState = "Bound";
        bridgeTransitions.Clear();
        _ = await replayGate.RunAsync(
                _ =>
                {
                    replayState = "Free";
                    bridgeTransitions.Add("Free");
                    return ValueTask.FromResult(true);
                })
            .ConfigureAwait(false);
        _ = await replayGate.RunAsync(
                _ =>
                {
                    if (replayState == "Bound")
                    {
                        bridgeTransitions.Add("Bound");
                    }
                    return ValueTask.FromResult(true);
                })
            .ConfigureAwait(false);
        Check.Equal("Free", string.Join(',', bridgeTransitions));

        var authorityNetwork = new GenerationBoundLanSession();
        Check.True(authorityNetwork.TryCaptureControlPeer(
            out var authorityPeer));
        using var bridgeBoundary = new BridgeSessionGenerationGate();
        var authoritySendGate = new PeerControlSendGate();
        var mappingGate = new RemoteBridgeMappingGate();
        var interactionRegistry = new AuthoritativeInteractionRegistry();
        var hostEntity = NetEntityId.Create(0xA1100001, 1);
        var guestEntity = NetEntityId.Create(0xA1100001, 2);
        var receivedAt = Environment.TickCount64;
        var authorityPlayers = new Dictionary<
            NetEntityId,
            ReplicatedPlayerSnapshot>
        {
            [hostEntity] = new(
                new PlayerStatePayload(
                    hostEntity,
                    (byte)SessionRole.Host,
                    PlayerLifecycle.Alive,
                    Vector3.Zero,
                    Vector3.Zero,
                    0,
                    1,
                    PlayerStateFlags.None),
                1,
                receivedAt),
            [guestEntity] = new(
                new PlayerStatePayload(
                    guestEntity,
                    (byte)SessionRole.Guest,
                    PlayerLifecycle.Alive,
                    new Vector3(1, 0, 0),
                    Vector3.Zero,
                    0,
                    1,
                    PlayerStateFlags.None),
                1,
                receivedAt)
        };
        ReplicatedPlayerSnapshot? LookupAuthorityPlayer(NetEntityId id) =>
            authorityPlayers.TryGetValue(id, out var player)
                ? player
                : null;

        var peerInteraction = new InteractionIntentPayload(
            guestEntity,
            hostEntity,
            NetEntityId.None,
            InteractionId: 700,
            Revision: 1,
            ActorSlot: (byte)SessionRole.Guest,
            InteractionKind.DismountPeer,
            InteractionIntentPhase.Begin,
            InteractionIntentFlags.TargetPlayer,
            RequestedDurationMilliseconds: 0);
        var peerLasso = new PlayerActionPayload(
            guestEntity,
            hostEntity,
            Sequence: 1,
            ActionId: 701,
            Revision: 1,
            ActorSlot: (byte)SessionRole.Guest,
            AuthoritySlot: (byte)SessionRole.Host,
            PlayerActionKind.Lasso,
            PlayerActionPhase.Active,
            PlayerActionFlags.Authoritative |
                PlayerActionFlags.TargetEntityValid |
                PlayerActionFlags.PhysicalTargetEffect,
            DurationMilliseconds: 0,
            PhaseElapsedMilliseconds: 0,
            WeaponHash: 0,
            VariantHash: 0,
            AnimationSampleSequence: 0,
            ActorAnchor: Vector3.Zero,
            TargetPoint: Vector3.Zero,
            FacingHeading: 0,
            NormalizedPhase: 0);
        long authorityBridgeGeneration = 0;

        BridgePipeConnectionToken? CaptureAuthorityBridge()
        {
            var generation = Interlocked.Read(
                ref authorityBridgeGeneration);
            return generation == 0
                ? null
                : new BridgePipeConnectionToken(generation);
        }

        bool AuthorityBridgeIsReady(BridgePipeConnectionToken bridge) =>
            bridge.Generation == Interlocked.Read(
                ref authorityBridgeGeneration);

        bool AuthorityMappingIsReady(
            ILanSession network,
            ControlPeerToken peer,
            BridgePipeConnectionToken bridge,
            NetEntityId actor) =>
            mappingGate.IsCurrent(
                network,
                peer,
                bridge,
                actor,
                Environment.TickCount64,
                maximumAgeMilliseconds: 5_000);

        ValueTask<bool> RunPeerInteractionTransactionAsync(
            ControlPeerToken peer,
            InteractionIntentPayload intent,
            Func<
                BridgePipeConnectionToken,
                PeerAuthorityControlFrame,
                CancellationToken,
                ValueTask<bool>> deliverBridge) =>
            PeerBridgeAuthorityTransaction.RunAsync<
                InteractionAuthorityResolution,
                PeerAuthorityControlFrame,
                AuthoritativeInteractionRegistry.TransactionSnapshot>(
                    bridgeBoundary,
                    authoritySendGate,
                    authorityNetwork,
                    peer,
                    () => authorityNetwork,
                    (network, expectedPeer) =>
                        network.IsControlPeerCurrent(expectedPeer),
                    CaptureAuthorityBridge,
                    AuthorityBridgeIsReady,
                    (network, expectedPeer, bridge) =>
                        AuthorityMappingIsReady(
                            network,
                            expectedPeer,
                            bridge,
                            intent.ActorEntityId),
                    interactionRegistry.CaptureTransactionSnapshot,
                    () => interactionRegistry.Resolve(
                        intent,
                        Environment.TickCount64,
                        LookupAuthorityPlayer),
                    interactionRegistry.RestoreTransactionSnapshot,
                    static resolution => resolution.RestraintState is
                        { } restraint
                            ?
                            [
                                new PeerAuthorityControlFrame(
                                    MessageType.InteractionResult,
                                    BinaryPayloadCodec
                                        .EncodeInteractionResult(
                                            resolution.Result)),
                                new PeerAuthorityControlFrame(
                                    MessageType.RestraintState,
                                    BinaryPayloadCodec.EncodeRestraintState(
                                        restraint))
                            ]
                            :
                            [
                                new PeerAuthorityControlFrame(
                                    MessageType.InteractionResult,
                                    BinaryPayloadCodec
                                        .EncodeInteractionResult(
                                            resolution.Result))
                            ],
                    deliverBridge,
                    (network, expectedPeer, control, token) =>
                        network.SendControlAsync(
                            expectedPeer,
                            control.Type,
                            control.Payload,
                            tick: 777,
                            token));

        ValueTask<bool> RunPeerActionTransactionAsync(
            ControlPeerToken peer,
            PlayerActionPayload action,
            Func<
                BridgePipeConnectionToken,
                PeerAuthorityControlFrame,
                CancellationToken,
                ValueTask<bool>> deliverBridge,
            Func<
                ILanSession,
                ControlPeerToken,
                PeerAuthorityControlFrame,
                CancellationToken,
                ValueTask<bool>>? deliverPeer = null)
        {
            var terminalRestraintCleanup = false;
            deliverPeer ??= static (network, expectedPeer, control, token) =>
                network.SendControlAsync(
                    expectedPeer,
                    control.Type,
                    control.Payload,
                    tick: 778,
                    token);
            return PeerBridgeAuthorityTransaction.RunAsync<
                PeerGuestPlayerActionAuthorityResolution,
                PeerAuthorityControlFrame,
                AuthoritativeInteractionRegistry.TransactionSnapshot>(
                    bridgeBoundary,
                    authoritySendGate,
                    authorityNetwork,
                    peer,
                    () => authorityNetwork,
                    (network, expectedPeer) =>
                        network.IsControlPeerCurrent(expectedPeer),
                    CaptureAuthorityBridge,
                    AuthorityBridgeIsReady,
                    (network, expectedPeer, bridge) =>
                    {
                        terminalRestraintCleanup =
                            interactionRegistry
                                .HasMatchingTerminalRestraint(action);
                        return terminalRestraintCleanup ||
                            AuthorityMappingIsReady(
                            network,
                            expectedPeer,
                            bridge,
                            action.ActorEntityId);
                    },
                    interactionRegistry.CaptureTransactionSnapshot,
                    () => new PeerGuestPlayerActionAuthorityResolution(
                        action,
                        interactionRegistry
                            .ObserveAuthoritativePlayerAction(action)),
                    interactionRegistry.RestoreTransactionSnapshot,
                    resolution => resolution.Restraint is { } restraint
                        ?
                        [
                            new PeerAuthorityControlFrame(
                                MessageType.PlayerAction,
                                BinaryPayloadCodec.EncodePlayerAction(
                                    action)),
                            new PeerAuthorityControlFrame(
                                MessageType.RestraintState,
                                BinaryPayloadCodec.EncodeRestraintState(
                                    restraint))
                        ]
                        :
                        [
                            new PeerAuthorityControlFrame(
                                MessageType.PlayerAction,
                                BinaryPayloadCodec.EncodePlayerAction(
                                    action))
                        ],
                    deliverBridge,
                    deliverPeer);
        }

        var untouchedAuthority = interactionRegistry.ReadSnapshot();
        var directBridgeCalls = 0;
        Check.False(await RunPeerInteractionTransactionAsync(
            authorityPeer,
            peerInteraction,
            (_, _, _) =>
            {
                Interlocked.Increment(ref directBridgeCalls);
                return ValueTask.FromResult(true);
            }));
        Check.Equal(0, directBridgeCalls);
        Check.Equal(
            untouchedAuthority,
            interactionRegistry.ReadSnapshot());

        Interlocked.Exchange(ref authorityBridgeGeneration, 10);
        Check.False(await RunPeerActionTransactionAsync(
            authorityPeer,
            peerLasso,
            (_, _, _) =>
            {
                Interlocked.Increment(ref directBridgeCalls);
                return ValueTask.FromResult(true);
            }));
        Check.Equal(0, directBridgeCalls);
        Check.Equal(0, interactionRegistry.CaptureRestraints().Count);
        Check.Equal(
            string.Empty,
            authorityNetwork.DescribeControls(authorityPeer.Generation));

        var pipeOrder = new List<string>();
        var firstPlayerDeliveryEntered = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirstPlayerDelivery = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var firstPlayerDelivery = 1;
        var mappingPump = new NetworkBridgeDeliveryPump(
            async envelope =>
            {
                if (envelope.Type == MessageType.PlayerState &&
                    Interlocked.Exchange(ref firstPlayerDelivery, 0) == 1)
                {
                    firstPlayerDeliveryEntered.TrySetResult(true);
                    await releaseFirstPlayerDelivery.Task
                        .ConfigureAwait(false);
                }
                pipeOrder.Add(envelope.Type.ToString());
                return true;
            });
        using var mappingPumpStop = new CancellationTokenSource();
        var mappingPumpTask = mappingPump.RunAsync(mappingPumpStop.Token);
        try
        {
            var playerEnvelope = new ProtocolEnvelope(
                MessageType.PlayerState,
                100,
                100,
                BinaryPayloadCodec.EncodePlayerState(
                    authorityPlayers[guestEntity].State));
            Check.Equal(
                NetworkBridgeEnqueueDisposition.Queued,
                mappingPump.TryEnqueue(
                    playerEnvelope,
                    () => authorityNetwork.IsControlPeerCurrent(
                        authorityPeer),
                    _ =>
                    {
                        mappingGate.MarkDelivered(
                            authorityNetwork,
                            authorityPeer,
                            new BridgePipeConnectionToken(10),
                            guestEntity,
                            Environment.TickCount64);
                        pipeOrder.Add("mapping-ready");
                    }).Disposition);
            await firstPlayerDeliveryEntered.Task
                .WaitAsync(TimeSpan.FromSeconds(2))
                .ConfigureAwait(false);

            Check.False(await RunPeerInteractionTransactionAsync(
                authorityPeer,
                peerInteraction,
                (_, _, _) => ValueTask.FromResult(true)));
            Check.Equal(
                untouchedAuthority,
                interactionRegistry.ReadSnapshot());

            var resync = new ProtocolEnvelope(
                MessageType.ResyncRequest,
                101,
                101,
                ReadOnlyMemory<byte>.Empty);
            mappingGate.BeginResync(
                authorityNetwork,
                authorityPeer);
            Check.False(await RunPeerActionTransactionAsync(
                authorityPeer,
                peerLasso,
                (_, _, _) => ValueTask.FromResult(true)));
            Check.Equal(
                NetworkBridgeEnqueueDisposition.Queued,
                mappingPump.TryEnqueue(
                    resync,
                    () => authorityNetwork.IsControlPeerCurrent(
                        authorityPeer),
                    deliveredEnvelope =>
                    {
                        _ = deliveredEnvelope;
                        _ = mappingGate.CompleteResync(
                            authorityNetwork,
                            authorityPeer);
                        pipeOrder.Add("mapping-clear");
                    }).Disposition);
            releaseFirstPlayerDelivery.TrySetResult(true);
            await WaitUntilAsync(
                    () => mappingPump.ReadSnapshot().Delivered == 2,
                    TimeSpan.FromSeconds(2),
                    "PlayerState/Resync callbacks did not complete.")
                .ConfigureAwait(false);
            Check.Equal(
                "PlayerState,mapping-ready,ResyncRequest,mapping-clear",
                string.Join(',', pipeOrder));
            Check.False(await RunPeerInteractionTransactionAsync(
                authorityPeer,
                peerInteraction,
                (_, _, _) => ValueTask.FromResult(true)));

            Check.Equal(
                NetworkBridgeEnqueueDisposition.Queued,
                mappingPump.TryEnqueue(
                    playerEnvelope with { Sequence = 102 },
                    () => authorityNetwork.IsControlPeerCurrent(
                        authorityPeer),
                    _ => mappingGate.MarkDelivered(
                        authorityNetwork,
                        authorityPeer,
                        new BridgePipeConnectionToken(10),
                        guestEntity,
                        Environment.TickCount64)).Disposition);
            await WaitUntilAsync(
                    () => mappingPump.ReadSnapshot().Delivered == 3,
                    TimeSpan.FromSeconds(2),
                    "Fresh PlayerState did not establish bridge mapping.")
                .ConfigureAwait(false);

            var beforeFailedBridge = interactionRegistry.ReadSnapshot();
            Check.False(await RunPeerInteractionTransactionAsync(
                authorityPeer,
                peerInteraction,
                (_, _, _) => ValueTask.FromResult(false)));
            Check.Equal(
                beforeFailedBridge,
                interactionRegistry.ReadSnapshot());
            Check.Equal(
                string.Empty,
                authorityNetwork.DescribeControls(
                    authorityPeer.Generation));

            Check.True(await RunPeerInteractionTransactionAsync(
                authorityPeer,
                peerInteraction,
                (_, control, _) =>
                {
                    pipeOrder.Add(control.Type.ToString());
                    return ValueTask.FromResult(true);
                }));
            var committedInteraction = interactionRegistry.ReadSnapshot();
            Check.Equal(1L, committedInteraction.Completed);
            Check.Equal(0L, committedInteraction.Duplicate);

            var beforeFailedAction = interactionRegistry.ReadSnapshot();
            Check.False(await RunPeerActionTransactionAsync(
                authorityPeer,
                peerLasso,
                (_, _, _) => ValueTask.FromResult(false)));
            Check.Equal(
                beforeFailedAction,
                interactionRegistry.ReadSnapshot());
            Check.Equal(0, interactionRegistry.CaptureRestraints().Count);
            var pinnedMappingBridgeFrames = 0;
            Check.True(await RunPeerActionTransactionAsync(
                authorityPeer,
                peerLasso,
                (_, control, _) =>
                {
                    pipeOrder.Add(control.Type.ToString());
                    if (Interlocked.Increment(
                            ref pinnedMappingBridgeFrames) == 1)
                    {
                        // Expire the five-second mapping lease between the two
                        // local frames. The generation gates pin the proof, so
                        // the batch must still complete atomically.
                        mappingGate.MarkDelivered(
                            authorityNetwork,
                            authorityPeer,
                            new BridgePipeConnectionToken(10),
                            guestEntity,
                            Environment.TickCount64 - 5_001);
                    }
                    return ValueTask.FromResult(true);
                }));
            Check.Equal(2, pinnedMappingBridgeFrames);
            Check.Equal(1, interactionRegistry.CaptureRestraints().Count);

            var terminalLasso = peerLasso with
            {
                TargetEntityId = NetEntityId.None,
                Sequence = 2,
                Revision = 2,
                Phase = PlayerActionPhase.Cancel,
                Flags = PlayerActionFlags.Intent
            };
            var terminalWithoutPlayerSnapshot =
                SidecarRuntime.EvaluateTerminalRestraintCleanup(
                    terminalLasso);
            Check.NotNull(terminalWithoutPlayerSnapshot.Resolved);
            Check.True(
                terminalWithoutPlayerSnapshot.Resolved!.Value.Flags
                    .HasFlag(PlayerActionFlags.Authoritative));
            Check.False(
                terminalWithoutPlayerSnapshot.Resolved.Value.Flags
                    .HasFlag(PlayerActionFlags.Intent));
            Check.True(await RunPeerActionTransactionAsync(
                authorityPeer,
                terminalWithoutPlayerSnapshot.Resolved.Value,
                (_, control, _) =>
                {
                    pipeOrder.Add(control.Type.ToString());
                    return ValueTask.FromResult(true);
                }));
            var releasedAfterExpiredMapping =
                interactionRegistry.CaptureRestraints().Single();
            Check.Equal(
                PlayerRestraintState.Free,
                releasedAfterExpiredMapping.State);

            mappingGate.MarkDelivered(
                authorityNetwork,
                authorityPeer,
                new BridgePipeConnectionToken(10),
                guestEntity,
                Environment.TickCount64);

            var rotationAction = peerLasso with
            {
                Sequence = 2,
                ActionId = 702
            };
            var blockedAuthoritySendEntered =
                new TaskCompletionSource<bool>(
                    TaskCreationOptions.RunContinuationsAsynchronously);
            var releaseBlockedAuthoritySend =
                new TaskCompletionSource<bool>(
                    TaskCreationOptions.RunContinuationsAsynchronously);
            var blockAuthoritySend = 1;
            var rotatingTransaction = RunPeerActionTransactionAsync(
                    authorityPeer,
                    rotationAction,
                    async (_, control, _) =>
                    {
                        if (Interlocked.Exchange(
                                ref blockAuthoritySend,
                                0) == 1)
                        {
                            blockedAuthoritySendEntered.TrySetResult(true);
                            await releaseBlockedAuthoritySend.Task
                                .ConfigureAwait(false);
                        }
                        pipeOrder.Add(control.Type.ToString());
                        return true;
                    })
                .AsTask();
            await blockedAuthoritySendEntered.Task
                .WaitAsync(TimeSpan.FromSeconds(2))
                .ConfigureAwait(false);

            var rotationEntered = 0;
            var replacementAuthorityPeer = default(ControlPeerToken);
            var rotation = Task.Run(async () =>
            {
                await using var boundary = await bridgeBoundary
                    .EnterBoundaryAsync()
                    .ConfigureAwait(false);
                Interlocked.Exchange(ref rotationEntered, 1);
                _ = await authoritySendGate.RunAsync(
                        token =>
                        {
                            _ = token;
                            Interlocked.Exchange(
                                ref authorityBridgeGeneration,
                                11);
                            mappingGate.Clear();
                            _ = interactionRegistry.Clear(
                                emitFreeStates: false);
                            replacementAuthorityPeer =
                                authorityNetwork.ReplacePeer();
                            return ValueTask.FromResult(true);
                        })
                    .ConfigureAwait(false);
            });
            await Task.Delay(25).ConfigureAwait(false);
            Check.Equal(0, Volatile.Read(ref rotationEntered));
            releaseBlockedAuthoritySend.TrySetResult(true);
            Check.True(await rotatingTransaction.ConfigureAwait(false));
            await rotation.ConfigureAwait(false);
            Check.True(replacementAuthorityPeer.IsValid);
            Check.Equal(0, interactionRegistry.CaptureRestraints().Count);
            Check.Equal(
                string.Empty,
                authorityNetwork.DescribeControls(
                    replacementAuthorityPeer.Generation));

            mappingGate.MarkDelivered(
                authorityNetwork,
                replacementAuthorityPeer,
                new BridgePipeConnectionToken(11),
                guestEntity,
                Environment.TickCount64);
            Check.True(await RunPeerActionTransactionAsync(
                replacementAuthorityPeer,
                rotationAction,
                (_, _, _) => ValueTask.FromResult(true)));
            Check.Equal(
                "PlayerAction:778,RestraintState:778",
                authorityNetwork.DescribeControls(
                    replacementAuthorityPeer.Generation));

            var partialLocalAction = rotationAction with
            {
                Sequence = 3,
                ActionId = 703
            };
            var partialLocalFrames = 0;
            Check.False(await RunPeerActionTransactionAsync(
                replacementAuthorityPeer,
                partialLocalAction,
                (_, _, _) => ValueTask.FromResult(
                    Interlocked.Increment(ref partialLocalFrames) == 1)));
            Check.Equal(2, partialLocalFrames);
            var retainedAfterPartialLocal =
                interactionRegistry.CaptureRestraints().Single();
            Check.Equal(
                partialLocalAction.ActionId,
                retainedAfterPartialLocal.SourceInteractionId);
            Check.Equal(
                PlayerRestraintState.Lassoed,
                retainedAfterPartialLocal.State);
            Check.Equal(
                PlayerRestraintState.Free,
                interactionRegistry.Clear(emitFreeStates: true)
                    .Single().State);

            var peerFailureAction = rotationAction with
            {
                Sequence = 4,
                ActionId = 704
            };
            Check.False(await RunPeerActionTransactionAsync(
                replacementAuthorityPeer,
                peerFailureAction,
                (_, _, _) => ValueTask.FromResult(true),
                deliverPeer: (_, _, _, _) => ValueTask.FromResult(false)));
            var retainedAfterPeerFailure =
                interactionRegistry.CaptureRestraints().Single();
            Check.Equal(
                peerFailureAction.ActionId,
                retainedAfterPeerFailure.SourceInteractionId);
            Check.Equal(
                PlayerRestraintState.Lassoed,
                retainedAfterPeerFailure.State);
            Check.Equal(
                PlayerRestraintState.Free,
                interactionRegistry.Clear(emitFreeStates: true)
                    .Single().State);
        }
        finally
        {
            await mappingPumpStop.CancelAsync().ConfigureAwait(false);
            await mappingPumpTask.ConfigureAwait(false);
        }
    }

    private static Task AnimationReplicationPayloadsAsync()
    {
        Check.Equal((ushort)27, ProtocolConstants.Version);
        Check.Equal((ushort)28, (ushort)MessageType.PlayerAnimationState);
        Check.Equal((ushort)29, (ushort)MessageType.MotionReplicationConfig);

        var entity = NetEntityId.Create(0x11223344, 7);
        const PlayerAnimationCapabilities capabilities =
            PlayerAnimationCapabilities.GraphIdentifier |
            PlayerAnimationCapabilities.StateIdentifier |
            PlayerAnimationCapabilities.ClipIdentifiers |
            PlayerAnimationCapabilities.NormalizedPhase |
            PlayerAnimationCapabilities.PlaybackRate |
            PlayerAnimationCapabilities.BlendWeights |
            PlayerAnimationCapabilities.TransitionProgress |
            PlayerAnimationCapabilities.RuntimeFlags;
        const PlayerAnimationStateFlags flags =
            PlayerAnimationStateFlags.GraphHashValid |
            PlayerAnimationStateFlags.StateHashValid |
            PlayerAnimationStateFlags.PrimaryClipHashValid |
            PlayerAnimationStateFlags.SecondaryClipHashValid |
            PlayerAnimationStateFlags.PrimaryPhaseValid |
            PlayerAnimationStateFlags.SecondaryPhaseValid |
            PlayerAnimationStateFlags.PrimaryPlaybackRateValid |
            PlayerAnimationStateFlags.SecondaryPlaybackRateValid |
            PlayerAnimationStateFlags.PrimaryBlendWeightValid |
            PlayerAnimationStateFlags.SecondaryBlendWeightValid |
            PlayerAnimationStateFlags.TransitionProgressValid |
            PlayerAnimationStateFlags.Transitioning |
            PlayerAnimationStateFlags.RootMotionActive |
            PlayerAnimationStateFlags.Looping;
        var sample = new PlayerAnimationStatePayload(
            entity,
            (byte)SessionRole.Guest,
            AnimationReplicationPayloadCodec.PlayerAnimationStateSchemaVersion,
            PlayerAnimationSampleSource.VersionedMemoryReader,
            LocomotionEpoch: 17,
            SampleSequence: 0xA1B2C3D4,
            capabilities,
            flags,
            GraphHash: 0x01020304,
            StateHash: 0x11121314,
            PrimaryClipHash: 0x21222324,
            SecondaryClipHash: 0x31323334,
            PrimaryNormalizedPhase: 0.25f,
            SecondaryNormalizedPhase: 0.75f,
            PrimaryPlaybackRate: 1.1f,
            SecondaryPlaybackRate: -0.5f,
            PrimaryBlendWeight: 0.6f,
            SecondaryBlendWeight: 0.4f,
            TransitionProgress: 0.35f);
        var encoded =
            AnimationReplicationPayloadCodec.EncodePlayerAnimationState(sample);
        Check.Equal(
            AnimationReplicationPayloadCodec.PlayerAnimationStateSize,
            encoded.Length);
        Check.Equal(
            sample,
            AnimationReplicationPayloadCodec.DecodePlayerAnimationState(encoded));
        Check.Equal(
            (byte)PlayerAnimationSampleSource.VersionedMemoryReader,
            encoded[68]);
        Check.True(encoded.AsSpan(69, 3).SequenceEqual(new byte[3]));

        var capabilityProbe = sample with
        {
            Capabilities = PlayerAnimationCapabilities.None,
            Flags = PlayerAnimationStateFlags.None,
            Source = PlayerAnimationSampleSource.None,
            GraphHash = 0,
            StateHash = 0,
            PrimaryClipHash = 0,
            SecondaryClipHash = 0,
            PrimaryNormalizedPhase = 0,
            SecondaryNormalizedPhase = 0,
            PrimaryPlaybackRate = 0,
            SecondaryPlaybackRate = 0,
            PrimaryBlendWeight = 0,
            SecondaryBlendWeight = 0,
            TransitionProgress = 0
        };
        Check.Equal(
            capabilityProbe,
            AnimationReplicationPayloadCodec.DecodePlayerAnimationState(
                AnimationReplicationPayloadCodec.EncodePlayerAnimationState(
                    capabilityProbe)));

        Check.Throws<ProtocolException>(
            () => AnimationReplicationPayloadCodec.EncodePlayerAnimationState(
                sample with
                {
                    Capabilities =
                        capabilities &
                        ~PlayerAnimationCapabilities.NormalizedPhase
                }));
        Check.Throws<ProtocolException>(
            () => AnimationReplicationPayloadCodec.EncodePlayerAnimationState(
                sample with
                {
                    Flags = flags & ~PlayerAnimationStateFlags.GraphHashValid
                }));
        Check.Throws<ProtocolException>(
            () => AnimationReplicationPayloadCodec.EncodePlayerAnimationState(
                sample with { PrimaryNormalizedPhase = 1.01f }));
        Check.Throws<ProtocolException>(
            () => AnimationReplicationPayloadCodec.EncodePlayerAnimationState(
                sample with { PrimaryPlaybackRate = float.NaN }));
        Check.Throws<ProtocolException>(
            () => AnimationReplicationPayloadCodec.EncodePlayerAnimationState(
                sample with { SampleSequence = 0 }));
        Check.Throws<ProtocolException>(
            () => AnimationReplicationPayloadCodec.EncodePlayerAnimationState(
                sample with
                {
                    Flags =
                        flags &
                        ~PlayerAnimationStateFlags.TransitionProgressValid
                }));
        Check.Throws<ProtocolException>(
            () => AnimationReplicationPayloadCodec.DecodePlayerAnimationState(
                encoded.AsSpan(0, encoded.Length - 1)));
        var reserved = encoded.ToArray();
        reserved[69] = 1;
        Check.Throws<ProtocolException>(
            () => AnimationReplicationPayloadCodec.DecodePlayerAnimationState(
                reserved));

        var config = new MotionReplicationConfigPayload(
            AnimationReplicationPayloadCodec.MotionReplicationConfigSchemaVersion,
            MotionReplicationWireMode.AnimGraphReplica,
            MotionReplicationConfigFlags.AllowTaskNavmeshFallback |
                MotionReplicationConfigFlags.EnableAnimSceneStoryVmProbe,
            Revision: 3);
        var encodedConfig =
            AnimationReplicationPayloadCodec.EncodeMotionReplicationConfig(config);
        Check.Equal(
            AnimationReplicationPayloadCodec.MotionReplicationConfigSize,
            encodedConfig.Length);
        Check.Equal(
            config,
            AnimationReplicationPayloadCodec.DecodeMotionReplicationConfig(
                encodedConfig));
        Check.Throws<ProtocolException>(
            () => AnimationReplicationPayloadCodec.EncodeMotionReplicationConfig(
                config with { Revision = 0 }));
        Check.Throws<ProtocolException>(
            () => AnimationReplicationPayloadCodec.EncodeMotionReplicationConfig(
                config with { Mode = (MotionReplicationWireMode)255 }));
        Check.Throws<ProtocolException>(
            () => AnimationReplicationPayloadCodec.DecodeMotionReplicationConfig(
                encodedConfig.AsSpan(1)));
        foreach (var role in new[] { SessionRole.Host, SessionRole.Guest })
        {
            Check.False(
                SidecarRuntime.IsPeerMessageAuthorized(
                    role,
                    MessageType.MotionReplicationConfig),
                "Peer motion-mode negotiation must be consumed by the sidecar, not forwarded to the bridge.");
            Check.False(
                SidecarRuntime.IsLocalBridgeMessageAuthorized(
                    role,
                    MessageType.MotionReplicationConfig),
                "A bridge must never originate peer motion-mode negotiation.");
        }

        var syntheticState = new PlayerStatePayload(
            entity,
            Slot: (byte)SessionRole.Guest,
            PlayerLifecycle.Alive,
            Vector3.Zero,
            Vector3.Zero,
            Heading: 0,
            HealthFraction: 1,
            PlayerStateFlags.SyntheticTest,
            DesiredMoveBlend: 0,
            LocomotionEpoch: 42);
        Check.True(
            LocalGameTestSession.CreateSyntheticAnimationState(
                MotionReplicationMode.TaskNavmesh,
                syntheticState,
                sampleSequence: 1) is null,
            "Task/Navmesh solo test must not emit AnimGraph samples.");
        var locomotionCases = new[]
        {
            (Blend: 0f, Hash: 0x9072A713U),
            (Blend: 1f, Hash: 0xD827C3DBU),
            (Blend: 2f, Hash: 0xFFF7E7A4U),
            (Blend: 3f, Hash: 0xBD8817DBU)
        };
        for (var index = 0; index < locomotionCases.Length; index++)
        {
            var locomotionCase = locomotionCases[index];
            var generated =
                LocalGameTestSession.CreateSyntheticAnimationState(
                    MotionReplicationMode.AnimGraphReplica,
                    syntheticState with
                    {
                        DesiredMoveBlend = locomotionCase.Blend
                    },
                    sampleSequence: (uint)index + 1) ??
                throw new SelfTestException(
                    "AnimGraph solo test did not create a locomotion sample.");
            Check.Equal(
                PlayerAnimationSampleSource.LocomotionNative,
                generated.Source);
            Check.Equal((ushort)42, generated.LocomotionEpoch);
            Check.Equal((uint)index + 1, generated.SampleSequence);
            Check.Equal(locomotionCase.Hash, generated.StateHash);
            Check.Equal(
                PlayerAnimationCapabilities.StateIdentifier |
                    PlayerAnimationCapabilities.RuntimeFlags,
                generated.Capabilities);
            Check.Equal(
                PlayerAnimationStateFlags.StateHashValid |
                    PlayerAnimationStateFlags.Looping,
                generated.Flags);
        }
        return Task.CompletedTask;
    }

    private static Task WorldAndEquipmentAsync()
    {
        Check.Equal((ushort)27, ProtocolConstants.Version);
        Check.Equal((ushort)23, (ushort)MessageType.WorldState);
        Check.Equal((ushort)24, (ushort)MessageType.EquipmentState);
        Check.Equal((ushort)25, (ushort)MessageType.PauseVote);

        var world = new WorldStatePayload(
            Hour: 21,
            Minute: 34,
            Second: 56,
            WorldStateFlags.WeatherValid,
            WeatherFrom: 0x11223344,
            WeatherTo: 0xA1B2C3D4,
            Blend: 0.5f,
            Day: 28,
            Month: 6,
            Year: 1899);
        var encodedWorld = BinaryPayloadCodec.EncodeWorldState(world);
        Check.Equal(BinaryPayloadCodec.WorldStateSize, encodedWorld.Length);
        Check.SequenceEqual(
            new byte[]
            {
                21, 34, 56, 1,
                0x44, 0x33, 0x22, 0x11,
                0xD4, 0xC3, 0xB2, 0xA1,
                0x00, 0x00, 0x00, 0x3F,
                28, 6, 0x6B, 0x07,
                0x00, 0x00, 0x00, 0x00
            },
            encodedWorld);
        Check.Equal(world, BinaryPayloadCodec.DecodeWorldState(encodedWorld));
        var clockOnly = world with
        {
            Flags = WorldStateFlags.None,
            WeatherFrom = 0,
            WeatherTo = 0,
            Blend = 0
        };
        Check.Equal(
            clockOnly,
            BinaryPayloadCodec.DecodeWorldState(
                BinaryPayloadCodec.EncodeWorldState(clockOnly)));

        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldState(
                world with { Hour = 24 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldState(
                world with { Minute = 60 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldState(
                world with { Second = 60 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldState(
                world with { Day = 0 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldState(
                world with { Day = 32 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldState(
                world with { Month = 12 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldState(
                world with { Year = 1799 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldState(
                world with { Year = 2201 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldState(
                world with { Flags = (WorldStateFlags)2 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldState(
                world with { Blend = float.NaN }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldState(
                world with { Blend = -0.01f }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldState(
                world with { Blend = 1.01f }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldState(
                world with { Flags = WorldStateFlags.None }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldState(
                world with { WeatherFrom = 0 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodeWorldState(
                encodedWorld.AsSpan(0, encodedWorld.Length - 1)));
        var worldWithUnknownFlags = encodedWorld.ToArray();
        worldWithUnknownFlags[3] = 0x80;
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodeWorldState(worldWithUnknownFlags));
        var worldWithReservedData = encodedWorld.ToArray();
        worldWithReservedData[20] = 1;
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodeWorldState(worldWithReservedData));

        var mission = new MissionStatePayload(
            new NetEntityId(0x0102030405060708),
            MissionEpoch: 0x11223344,
            Revision: 0x55667788,
            CheckpointGeneration: 0xA1B2C3D4,
            MissionPhase.Recovery,
            MissionStateFlags.AnchorValid |
                MissionStateFlags.MissionActive |
                MissionStateFlags.CheckpointRecovery,
            new Vector3(1f, 2f, -4f),
            HostHeading: 90f);
        var encodedMission = BinaryPayloadCodec.EncodeMissionState(mission);
        Check.Equal(BinaryPayloadCodec.MissionStateSize, encodedMission.Length);
        Check.SequenceEqual(
            new byte[]
            {
                0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
                0x44, 0x33, 0x22, 0x11,
                0x88, 0x77, 0x66, 0x55,
                0xD4, 0xC3, 0xB2, 0xA1,
                0x04, 0x07, 0x00, 0x00,
                0x00, 0x00, 0x80, 0x3F,
                0x00, 0x00, 0x00, 0x40,
                0x00, 0x00, 0x80, 0xC0,
                0x00, 0x00, 0xB4, 0x42,
                0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00
            },
            encodedMission);
        Check.Equal(
            mission,
            BinaryPayloadCodec.DecodeMissionState(encodedMission));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeMissionState(
                mission with { HostEntityId = NetEntityId.None }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeMissionState(
                mission with { MissionEpoch = 0 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeMissionState(
                mission with { Revision = 0 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeMissionState(
                mission with { Phase = (MissionPhase)0xFF }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeMissionState(
                mission with { Flags = (MissionStateFlags)0x80 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeMissionState(
                mission with { HostHeading = float.NaN }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeMissionState(
                mission with
                {
                    Flags = mission.Flags & ~MissionStateFlags.AnchorValid
                }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeMissionState(
                mission with
                {
                    Phase = MissionPhase.Idle,
                    Flags = MissionStateFlags.MissionActive
                }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeMissionState(
                mission with
                {
                    Phase = MissionPhase.Active,
                    Flags = MissionStateFlags.AnchorValid
                }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeMissionState(
                mission with
                {
                    Flags = mission.Flags &
                        ~MissionStateFlags.CheckpointRecovery
                }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodeMissionState(
                encodedMission.AsSpan(0, encodedMission.Length - 1)));
        var missionWithUnknownPhase = encodedMission.ToArray();
        missionWithUnknownPhase[20] = 0xFF;
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodeMissionState(
                missionWithUnknownPhase));
        var missionWithReservedData = encodedMission.ToArray();
        missionWithReservedData[22] = 1;
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodeMissionState(
                missionWithReservedData));
        var missionWithTrailingReservedData = encodedMission.ToArray();
        missionWithTrailingReservedData[40] = 1;
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodeMissionState(
                missionWithTrailingReservedData));

        var equipment = new EquipmentStatePayload(
            new NetEntityId(0x0102030405060708),
            WeaponHash: 0x11223344,
            Ammo: 0x55667788,
            EquipmentStateFlags.Equipped | EquipmentStateFlags.Reloading);
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeEquipmentState(
                equipment with { EntityId = new NetEntityId(1) }));
        var encodedEquipment =
            BinaryPayloadCodec.EncodeEquipmentState(equipment);
        Check.Equal(
            BinaryPayloadCodec.EquipmentStateSize,
            encodedEquipment.Length);
        Check.SequenceEqual(
            new byte[]
            {
                0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
                0x44, 0x33, 0x22, 0x11,
                0x88, 0x77, 0x66, 0x55,
                0x03, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00
            },
            encodedEquipment);
        Check.Equal(
            equipment,
            BinaryPayloadCodec.DecodeEquipmentState(encodedEquipment));

        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeEquipmentState(
                equipment with { EntityId = NetEntityId.None }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeEquipmentState(
                equipment with { Flags = (EquipmentStateFlags)4 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodeEquipmentState(
                encodedEquipment.AsSpan(0, encodedEquipment.Length - 1)));
        var equipmentWithUnknownFlags = encodedEquipment.ToArray();
        equipmentWithUnknownFlags[16] = 4;
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodeEquipmentState(
                equipmentWithUnknownFlags));
        var equipmentWithReservedData = encodedEquipment.ToArray();
        equipmentWithReservedData[20] = 1;
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodeEquipmentState(
                equipmentWithReservedData));

        var pauseState = new PauseVotePayload(
            PauseVoteKind.AuthoritativeState,
            (byte)SessionRole.Host,
            PauseVoteFlags.GuestVoted | PauseVoteFlags.Paused,
            Generation: 42);
        var encodedPause =
            BinaryPayloadCodec.EncodePauseVote(pauseState);
        Check.Equal(BinaryPayloadCodec.PauseVoteSize, encodedPause.Length);
        Check.Equal(
            pauseState,
            BinaryPayloadCodec.DecodePauseVote(encodedPause));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePauseVote(
                pauseState with
                {
                    Kind = PauseVoteKind.RequestToggle
                }));
        var pauseWithReservedData = encodedPause.ToArray();
        pauseWithReservedData[8] = 1;
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodePauseVote(
                pauseWithReservedData));

        var guestPauseRequest = new ProtocolEnvelope(
            MessageType.PauseVote,
            1,
            1,
            BinaryPayloadCodec.EncodePauseVote(
                new PauseVotePayload(
                    PauseVoteKind.RequestToggle,
                    (byte)SessionRole.Guest,
                    PauseVoteFlags.None,
                    Generation: 42)));
        var hostPauseState = new ProtocolEnvelope(
            MessageType.PauseVote,
            2,
            2,
            encodedPause);
        Check.True(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Guest,
            guestPauseRequest));
        Check.True(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Host,
            guestPauseRequest));
        Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Guest,
            guestPauseRequest));
        Check.True(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Host,
            hostPauseState));
        Check.True(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Guest,
            hostPauseState));
        Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Host,
            hostPauseState));

        Check.False(SidecarRuntime.IsPeerMessageAuthorized(
            SessionRole.Host,
            MessageType.WorldState));
        Check.True(SidecarRuntime.IsPeerMessageAuthorized(
            SessionRole.Guest,
            MessageType.WorldState));
        Check.True(SidecarRuntime.IsPeerMessageAuthorized(
            SessionRole.Host,
            MessageType.EquipmentState));
        Check.True(SidecarRuntime.IsPeerMessageAuthorized(
            SessionRole.Guest,
            MessageType.EquipmentState));
        foreach (var hostAuthoritative in new[]
                 {
                     MessageType.WorldState,
                     MessageType.EntitySpawn,
                     MessageType.EntityUpdate,
                     MessageType.EntityDespawn,
                     MessageType.MissionState
                 })
        {
            Check.False(SidecarRuntime.IsPeerMessageAuthorized(
                SessionRole.Host,
                hostAuthoritative));
            Check.True(SidecarRuntime.IsPeerMessageAuthorized(
                SessionRole.Guest,
                hostAuthoritative));
            Check.True(SidecarRuntime.IsLocalBridgeMessageAuthorized(
                SessionRole.Host,
                hostAuthoritative));
            Check.False(SidecarRuntime.IsLocalBridgeMessageAuthorized(
                SessionRole.Guest,
                hostAuthoritative));
        }
        Check.False(SidecarRuntime.IsPeerMessageAuthorized(
            SessionRole.Guest,
            MessageType.DamageIntent));
        Check.True(SidecarRuntime.IsPeerMessageAuthorized(
            SessionRole.Host,
            MessageType.DamageIntent));
        Check.False(SidecarRuntime.IsLocalBridgeMessageAuthorized(
            SessionRole.Host,
            MessageType.DamageIntent));
        Check.True(SidecarRuntime.IsLocalBridgeMessageAuthorized(
            SessionRole.Guest,
            MessageType.DamageIntent));

        var progression = new MissionProgressionPayload(
            0x2C3469ED,
            7,
            0x700000001,
            MissionProgressionPhase.Offer,
            MissionProgressionFlags.None);
        var progressionEnvelope = new ProtocolEnvelope(
            MessageType.MissionProgression,
            1,
            2,
            BinaryPayloadCodec.EncodeMissionProgression(progression));
        SidecarRuntime.ValidateBinaryControlPayload(progressionEnvelope);
        Check.True(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Host, progressionEnvelope));
        Check.True(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Guest, progressionEnvelope));
        Check.False(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Guest, progressionEnvelope));
        Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Host, progressionEnvelope));
        var completionProgression = new MissionProgressionPayload(
            0x2C3469ED,
            7,
            0x700000001,
            MissionProgressionPhase.Completion,
            MissionProgressionFlags.VerifiedCompletionMapping,
            CompletionRating: 4,
            CompletionCashAward: 140);
        var decodedCompletionProgression = BinaryPayloadCodec.DecodeMissionProgression(
            BinaryPayloadCodec.EncodeMissionProgression(completionProgression));
        Check.Equal(completionProgression, decodedCompletionProgression);

        ProtocolEnvelope CommandEnvelope(CommandOpcode opcode) =>
            new(
                MessageType.Command,
                1,
                2,
                BinaryPayloadCodec.EncodeCommand(
                    new CommandPayload(
                        opcode,
                        CommandFlags.None,
                        NetEntityId.None,
                        Vector3.Zero,
                        0,
                        0)));

        foreach (var localOnlyOrHostUnsafe in new[]
                 {
                     CommandOpcode.Unload,
                     CommandOpcode.ToggleDiagnostics
                 })
        {
            var envelope = CommandEnvelope(localOnlyOrHostUnsafe);
            Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
                SessionRole.Host,
                envelope));
            Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
                SessionRole.Guest,
                envelope));
            Check.False(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
                SessionRole.Host,
                envelope));
            Check.False(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
                SessionRole.Guest,
                envelope));
        }

        foreach (var hostOnly in new[]
                 {
                     CommandOpcode.RetryCheckpoint,
                     CommandOpcode.SoloOverrideOn,
                     CommandOpcode.TeleportGuest
                 })
        {
            var envelope = CommandEnvelope(hostOnly);
            Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
                SessionRole.Host,
                envelope));
            Check.True(SidecarRuntime.IsPeerEnvelopeAuthorized(
                SessionRole.Guest,
                envelope));
            Check.True(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
                SessionRole.Host,
                envelope));
            Check.False(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
                SessionRole.Guest,
                envelope));
        }

        ProtocolEnvelope MarkerEnvelope(
            SessionRole origin,
            uint markerId)
        {
            var roleCode = origin == SessionRole.Host ? 1UL : 2UL;
            var correlation =
                (roleCode << 48) | (0x1234UL << 24) | markerId;
            return new ProtocolEnvelope(
                MessageType.Command,
                markerId,
                0x1234,
                BinaryPayloadCodec.EncodeCommand(
                    new CommandPayload(
                        CommandOpcode.DiagnosticMarker,
                        CommandFlags.None,
                        new NetEntityId(correlation),
                        new Vector3(1, 2, 3),
                        90,
                        markerId)));
        }

        var hostMarker = MarkerEnvelope(SessionRole.Host, 1);
        Check.True(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Host,
            hostMarker));
        Check.True(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Guest,
            hostMarker));
        Check.False(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Guest,
            hostMarker));
        Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Host,
            hostMarker));

        var guestMarker = MarkerEnvelope(SessionRole.Guest, 2);
        Check.True(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Guest,
            guestMarker));
        Check.True(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Host,
            guestMarker));
        Check.False(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Host,
            guestMarker));
        Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Guest,
            guestMarker));

        var damageApplied = new ProtocolEnvelope(
            MessageType.DamageApplied,
            1,
            2,
            ReadOnlyMemory<byte>.Empty);
        Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Host,
            damageApplied));
        Check.True(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Guest,
            damageApplied));
        Check.True(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Host,
            damageApplied));
        Check.False(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Guest,
            damageApplied));

        return Task.CompletedTask;
    }

    private static Task WorldMirrorPayloadsAsync()
    {
        var entity = new WorldEntityStatePayload(
            new NetEntityId(0x0102030405060708),
            ModelHash: 0x11223344,
            WorldEntityKind.Ped,
            WorldEntityStateFlags.Human |
            WorldEntityStateFlags.InCombat |
            WorldEntityStateFlags.Firing |
            WorldEntityStateFlags.Aiming,
            WorldCombatTargetSlot.Guest,
            Position: new Vector3(1f, -2f, 3.5f),
            Velocity: new Vector3(-0.5f, 4f, 0f),
            Heading: 270f,
            HealthFraction: 0.25f,
            WeaponHash: 0xA1B2C3D4,
            TaskKind: WorldTaskKind.Combat,
            ParentEntityId: NetEntityId.None,
            TaskTarget: new Vector3(9f, 8f, 7f));
        var encodedEntity =
            BinaryPayloadCodec.EncodeWorldEntityState(entity);
        Check.Equal(
            BinaryPayloadCodec.WorldEntityStateSize,
            encodedEntity.Length);
        Check.SequenceEqual(
            new byte[]
            {
                0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
                0x44, 0x33, 0x22, 0x11,
                0x01, 0x39, 0x02, 0x04,
                0x00, 0x00, 0x80, 0x3F,
                0x00, 0x00, 0x00, 0xC0,
                0x00, 0x00, 0x60, 0x40,
                0x00, 0x00, 0x00, 0xBF,
                0x00, 0x00, 0x80, 0x40,
                0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x87, 0x43,
                0x00, 0x00, 0x80, 0x3E,
                0xD4, 0xC3, 0xB2, 0xA1,
                0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x10, 0x41,
                0x00, 0x00, 0x00, 0x41,
                0x00, 0x00, 0xE0, 0x40,
                0x00, 0x00, 0x00, 0x00
            },
            encodedEntity);
        Check.Equal(
            entity,
            BinaryPayloadCodec.DecodeWorldEntityState(encodedEntity));

        var wildlife = entity with
        {
            Flags = WorldEntityStateFlags.None,
            CombatTargetSlot = WorldCombatTargetSlot.None,
            WeaponHash = 0
        };
        Check.Equal(
            wildlife,
            BinaryPayloadCodec.DecodeWorldEntityState(
                BinaryPayloadCodec.EncodeWorldEntityState(wildlife)));
        var horse = wildlife with
        {
            Flags = WorldEntityStateFlags.Horse
        };
        Check.Equal(
            horse,
            BinaryPayloadCodec.DecodeWorldEntityState(
                BinaryPayloadCodec.EncodeWorldEntityState(horse)));
        var animSceneObject = wildlife with
        {
            Kind = WorldEntityKind.Object,
            Flags = WorldEntityStateFlags.ScriptOwned,
            TaskKind = WorldTaskKind.Cinematic
        };
        Check.Equal(
            animSceneObject,
            BinaryPayloadCodec.DecodeWorldEntityState(
                BinaryPayloadCodec.EncodeWorldEntityState(animSceneObject)));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldEntityState(
                animSceneObject with
                {
                    Flags = WorldEntityStateFlags.Human
                }));

        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldEntityState(
                entity with { EntityId = new NetEntityId(1) }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldEntityState(
                entity with { ModelHash = 0 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldEntityState(
                entity with { Kind = (WorldEntityKind)0 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldEntityState(
                entity with { Flags = (WorldEntityStateFlags)0x80 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldEntityState(
                entity with
                {
                    Flags =
                        WorldEntityStateFlags.Human |
                        WorldEntityStateFlags.Horse
                }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldEntityState(
                wildlife with { WeaponHash = 1 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldEntityState(
                horse with
                {
                    Flags =
                        WorldEntityStateFlags.Horse |
                        WorldEntityStateFlags.Firing
                }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldEntityState(
                entity with
                {
                    Flags = WorldEntityStateFlags.Human,
                    CombatTargetSlot = WorldCombatTargetSlot.Guest
                }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldEntityState(
                entity with
                {
                    CombatTargetSlot = (WorldCombatTargetSlot)3
                }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldEntityState(
                entity with
                {
                    Flags =
                        WorldEntityStateFlags.Human |
                        WorldEntityStateFlags.Aiming,
                    CombatTargetSlot = WorldCombatTargetSlot.None,
                    WeaponHash = 0
                }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldEntityState(
                entity with
                {
                    Position = new Vector3(float.NaN, 0, 0)
                }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldEntityState(
                entity with
                {
                    Velocity = new Vector3(0, float.PositiveInfinity, 0)
                }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldEntityState(
                entity with { Heading = 360f }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeWorldEntityState(
                entity with { HealthFraction = -0.01f }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodeWorldEntityState(
                encodedEntity.AsSpan(0, encodedEntity.Length - 1)));
        var entityWithReservedData = encodedEntity.ToArray();
        entityWithReservedData[72] = 1;
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodeWorldEntityState(
                entityWithReservedData));

        var despawn = new EntityDespawnPayload(entity.EntityId);
        var encodedDespawn =
            BinaryPayloadCodec.EncodeEntityDespawn(despawn);
        Check.Equal(8, encodedDespawn.Length);
        Check.SequenceEqual(
            encodedEntity.AsSpan(0, 8),
            encodedDespawn);
        Check.Equal(
            despawn,
            BinaryPayloadCodec.DecodeEntityDespawn(encodedDespawn));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeEntityDespawn(
                new EntityDespawnPayload(NetEntityId.None)));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodeEntityDespawn(
                encodedDespawn.AsSpan(0, 7)));

        var mount = new PlayerMountStatePayload(
            new NetEntityId(0x0102030405060708),
            new NetEntityId(0x010203040506070B),
            (byte)SessionRole.Guest,
            PlayerMountStateFlags.Present |
            PlayerMountStateFlags.Mounted |
            PlayerMountStateFlags.BorrowedPeerMount,
            0x88776655,
            new Vector3(10f, 11f, 12f),
            new Vector3(1f, 0f, 0f),
            45f,
            0.75f,
            2);
        var encodedMount =
            BinaryPayloadCodec.EncodePlayerMountState(mount);
        Check.Equal(
            BinaryPayloadCodec.PlayerMountStateSize,
            encodedMount.Length);
        Check.Equal(
            mount,
            BinaryPayloadCodec.DecodePlayerMountState(
                encodedMount));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerMountState(
                mount with
                {
                    Flags = PlayerMountStateFlags.None,
                    ModelHash = 0
                }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerMountState(
                mount with
                {
                    Flags = PlayerMountStateFlags.Present |
                            PlayerMountStateFlags.BorrowedPeerMount
                }));
        var wagon = mount with
        {
            Flags = PlayerMountStateFlags.Present |
                    PlayerMountStateFlags.Mounted |
                    PlayerMountStateFlags.Vehicle |
                    PlayerMountStateFlags.VehicleDriver
        };
        Check.Equal(
            wagon,
            BinaryPayloadCodec.DecodePlayerMountState(
                BinaryPayloadCodec.EncodePlayerMountState(wagon)));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerMountState(
                wagon with
                {
                    Flags = wagon.Flags |
                            PlayerMountStateFlags.VehiclePassenger
                }));

        var damage = new DamageIntentPayload(
            AttackerId: entity.EntityId,
            TargetId: NetEntityId.Create(0xAABBCCDD, 2),
            WeaponHash: 0x11223344,
            Damage: 12.5f,
            ShotSequence: 0x55667788);
        var encodedDamage =
            BinaryPayloadCodec.EncodeDamageIntent(damage);
        Check.Equal(
            BinaryPayloadCodec.DamageIntentSize,
            encodedDamage.Length);
        Check.SequenceEqual(
            new byte[]
            {
                0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
                0x02, 0x00, 0x00, 0x00, 0xDD, 0xCC, 0xBB, 0xAA,
                0x44, 0x33, 0x22, 0x11,
                0x00, 0x00, 0x48, 0x41,
                0x88, 0x77, 0x66, 0x55,
                0x00, 0x00, 0x00, 0x00
            },
            encodedDamage);
        Check.Equal(
            damage,
            BinaryPayloadCodec.DecodeDamageIntent(encodedDamage));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeDamageIntent(
                damage with { AttackerId = NetEntityId.None }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeDamageIntent(
                damage with { TargetId = new NetEntityId(1) }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeDamageIntent(
                damage with { WeaponHash = 0 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeDamageIntent(
                damage with { Damage = float.NaN }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeDamageIntent(
                damage with { Damage = 0 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeDamageIntent(
                damage with { Damage = 100.01f }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodeDamageIntent(
                damage with { ShotSequence = 0 }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodeDamageIntent(
                encodedDamage.AsSpan(0, encodedDamage.Length - 1)));
        var damageWithReservedData = encodedDamage.ToArray();
        damageWithReservedData[28] = 1;
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodeDamageIntent(
                damageWithReservedData));

        return Task.CompletedTask;
    }

    private static Task AuthoritativeWorldGraphAsync()
    {
        var parentId = NetEntityId.Create(0x12345678, 1_000);
        var childId = NetEntityId.Create(0x12345678, 1_001);
        var parent = new WorldEntityStatePayload(
            parentId,
            0x10000001,
            WorldEntityKind.Ped,
            WorldEntityStateFlags.Horse,
            WorldCombatTargetSlot.None,
            new Vector3(1, 2, 3),
            Vector3.Zero,
            0,
            1,
            0);
        var child = new WorldEntityStatePayload(
            childId,
            0x10000002,
            WorldEntityKind.Ped,
            WorldEntityStateFlags.Human |
            WorldEntityStateFlags.Mounted,
            WorldCombatTargetSlot.None,
            new Vector3(1, 2, 4),
            Vector3.Zero,
            0,
            1,
            0,
            WorldTaskKind.Mounted,
            parentId,
            new Vector3(1, 2, 4));
        static ProtocolEnvelope StateEnvelope(
            WorldEntityStatePayload state,
            uint sequence) =>
            new(
                MessageType.EntityUpdate,
                sequence,
                sequence * 10UL,
                BinaryPayloadCodec.EncodeWorldEntityState(state));

        var graph = new AuthoritativeWorldGraphRegistry(4);
        Check.Equal(
            WorldGraphApplyDisposition.Applied,
            graph.Apply(StateEnvelope(child, 10)));
        Check.Equal(
            WorldGraphApplyDisposition.Applied,
            graph.Apply(StateEnvelope(parent, 11)));
        var snapshot = graph.CaptureSpawnSnapshot();
        Check.Equal(2, snapshot.Count);
        Check.Equal(MessageType.EntitySpawn, snapshot[0].Type);
        Check.Equal(
            parentId,
            BinaryPayloadCodec.DecodeWorldEntityState(
                snapshot[0].Payload.Span).EntityId);
        Check.Equal(
            childId,
            BinaryPayloadCodec.DecodeWorldEntityState(
                snapshot[1].Payload.Span).EntityId);

        Check.Equal(
            WorldGraphApplyDisposition.Stale,
            graph.Apply(StateEnvelope(child, 9)));
        var despawn = new ProtocolEnvelope(
            MessageType.EntityDespawn,
            20,
            200,
            BinaryPayloadCodec.EncodeEntityDespawn(
                new EntityDespawnPayload(parentId)));
        Check.Equal(
            WorldGraphApplyDisposition.Applied,
            graph.Apply(despawn));
        Check.Equal(0, graph.CaptureSpawnSnapshot().Count);
        Check.Equal(
            WorldGraphApplyDisposition.Stale,
            graph.Apply(StateEnvelope(child, 19)));
        var stats = graph.ReadSnapshot();
        Check.Equal(0, stats.Nodes);
        Check.Equal(1L, stats.CascadedDespawns);
        Check.Equal(2L, stats.Stale);

        return Task.CompletedTask;
    }

    private static async Task PlayerIdentityAsync()
    {
        var entityId = NetEntityId.Create(0x10203040, 7);
        var identity = new PlayerIdentityPayload(
            entityId,
            (byte)SessionRole.Guest,
            "Ranger 🤠");
        var encoded = BinaryPayloadCodec.EncodePlayerIdentity(identity);
        Check.Equal(
            BinaryPayloadCodec.PlayerIdentityHeaderSize +
            System.Text.Encoding.UTF8.GetByteCount(identity.Nickname),
            encoded.Length);
        Check.Equal(identity, BinaryPayloadCodec.DecodePlayerIdentity(encoded));

        var maximumUtf8 = new string('é', 24);
        var maximumEncoded = BinaryPayloadCodec.EncodePlayerIdentity(
            identity with { Nickname = maximumUtf8 });
        Check.Equal(
            BinaryPayloadCodec.PlayerIdentityHeaderSize + 48,
            maximumEncoded.Length);
        Check.Equal(
            maximumUtf8,
            BinaryPayloadCodec.DecodePlayerIdentity(maximumEncoded).Nickname);
        var maximumBytes = string.Concat(Enumerable.Repeat("🤠", 16));
        var maximumByteEncoded = BinaryPayloadCodec.EncodePlayerIdentity(
            identity with { Nickname = maximumBytes });
        Check.Equal(
            BinaryPayloadCodec.PlayerIdentityMaximumSize,
            maximumByteEncoded.Length);
        Check.Equal(
            maximumBytes,
            BinaryPayloadCodec.DecodePlayerIdentity(maximumByteEncoded).Nickname);

        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerIdentity(
                identity with { Nickname = string.Empty }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerIdentity(
                identity with { Nickname = new string('x', 25) }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerIdentity(
                identity with { Nickname = string.Concat(
                    Enumerable.Repeat("🤠", 17)) }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerIdentity(
                identity with { Nickname = " line" }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerIdentity(
                identity with { Nickname = "line\nbreak" }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerIdentity(
                identity with { Nickname = "left\u202Eright" }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerIdentity(
                identity with { Nickname = "bad~n~name" }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerIdentity(
                identity with { EntityId = NetEntityId.None }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerIdentity(
                identity with { EntityId = new NetEntityId(1) }));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.EncodePlayerIdentity(
                identity with { Slot = 2 }));

        var badLength = encoded.ToArray();
        badLength[9]++;
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodePlayerIdentity(badLength));
        var badUtf8 = encoded.ToArray();
        badUtf8[^1] = 0xFF;
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodePlayerIdentity(badUtf8));
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodePlayerIdentity(
                new byte[BinaryPayloadCodec.PlayerIdentityHeaderSize - 1]));

        long now = 1_000;
        var publisher = new PlayerIdentityPublisher(
            identity.Nickname,
            () => now);
        var network = new RecordingLanSession
        {
            IsConnected = false
        };
        var state = new PlayerStatePayload(
            entityId,
            (byte)SessionRole.Guest,
            PlayerLifecycle.Alive,
            Vector3.Zero,
            Vector3.Zero,
            0,
            1,
            PlayerStateFlags.None);
        publisher.ObservePlayerState(state);

        Check.Equal(
            IdentityPublishResult.NotReady,
            await publisher.PublishIfDueAsync(
                network,
                tick: 10,
                force: false,
                CancellationToken.None).ConfigureAwait(false));
        network.IsConnected = true;
        Check.Equal(
            IdentityPublishResult.Delivered,
            await publisher.PublishIfDueAsync(
                network,
                tick: 11,
                force: false,
                CancellationToken.None).ConfigureAwait(false));
        Check.Equal(
            IdentityPublishResult.NotDue,
            await publisher.PublishIfDueAsync(
                network,
                tick: 12,
                force: false,
                CancellationToken.None).ConfigureAwait(false));

        now += PlayerIdentityPublisher.RefreshIntervalMilliseconds - 1;
        Check.Equal(
            IdentityPublishResult.NotDue,
            await publisher.PublishIfDueAsync(
                network,
                tick: 13,
                force: false,
                CancellationToken.None).ConfigureAwait(false));
        now++;
        Check.Equal(
            IdentityPublishResult.Delivered,
            await publisher.PublishIfDueAsync(
                network,
                tick: 14,
                force: false,
                CancellationToken.None).ConfigureAwait(false));

        network.IsConnected = false;
        network.IsConnected = true;
        Check.Equal(
            IdentityPublishResult.Delivered,
            await publisher.PublishIfDueAsync(
                network,
                tick: 15,
                force: true,
                CancellationToken.None).ConfigureAwait(false));
        Check.Equal(3, network.Controls.Count);
        Check.True(network.Controls.All(
            envelope => envelope.Type == MessageType.PlayerIdentity));
        Check.Equal(
            identity,
            BinaryPayloadCodec.DecodePlayerIdentity(
                network.Controls[0].Payload.Span));
    }

    private static Task PlayerInventoryAsync()
    {
        var host = NetEntityId.Create(77, 1);
        var guest = NetEntityId.Create(77, 2);
        var inventory = new PlayerInventoryRegistry();
        var loot = new MapLoot(
            "valentine-general-store-drawer-01",
            12.50m,
            [
                new InventoryItemStack("tonic.health", 1),
                new InventoryItemStack("ammo.revolver", 12)
            ]);

        var hostFirstClaim = inventory.ClaimLoot(host, loot);
        Check.Equal(LootClaimStatus.Granted, hostFirstClaim.Status);
        Check.Equal(12.50m, hostFirstClaim.Inventory.Money);
        Check.Equal(12, hostFirstClaim.Inventory.Items["ammo.revolver"]);

        var hostReplay = inventory.ClaimLoot(host, loot);
        Check.Equal(LootClaimStatus.AlreadyClaimed, hostReplay.Status);
        Check.Equal(12.50m, hostReplay.Inventory.Money);
        Check.Equal(1, hostReplay.Inventory.Items["tonic.health"]);

        var guestFirstClaim = inventory.ClaimLoot(guest, loot);
        Check.Equal(LootClaimStatus.Granted, guestFirstClaim.Status);
        Check.Equal(12.50m, guestFirstClaim.Inventory.Money);
        Check.Equal(1, guestFirstClaim.Inventory.Items["tonic.health"]);

        var hostSnapshot = inventory.GetSnapshot(host);
        Check.Equal(12.50m, hostSnapshot.Money);
        Check.Equal(2, hostSnapshot.Items.Count);
        Check.Equal(0m, inventory.GetSnapshot(NetEntityId.Create(77, 3)).Money);

        var rollback = inventory.CaptureTransactionSnapshot();
        _ = inventory.ClaimLoot(host, new MapLoot("rollback-test", 5m));
        Check.Equal(17.50m, inventory.GetSnapshot(host).Money);
        inventory.RestoreTransactionSnapshot(rollback);
        Check.Equal(12.50m, inventory.GetSnapshot(host).Money);

        var reconnect = inventory.CaptureReconnectState();
        var restored = new PlayerInventoryRegistry();
        restored.RestoreReconnectState(reconnect);
        Check.Equal(12.50m, restored.GetSnapshot(host).Money);
        Check.Equal(LootClaimStatus.AlreadyClaimed, restored.ClaimLoot(host, loot).Status);
        return Task.CompletedTask;
    }

    private static Task PickupCollectionProtocolAsync()
    {
        var payload = new PickupCollectedPayload(
            NetEntityId.Create(77, 2), 0xABCDEF0123456789UL, 0x11223344U);
        var encoded = BinaryPayloadCodec.EncodePickupCollected(payload);
        Check.Equal(BinaryPayloadCodec.PickupCollectedSize, encoded.Length);
        Check.Equal(payload, BinaryPayloadCodec.DecodePickupCollected(encoded));
        encoded[20] = 1;
        Check.Throws<ProtocolException>(
            () => BinaryPayloadCodec.DecodePickupCollected(encoded));
        Check.Throws<ProtocolException>(() => BinaryPayloadCodec.EncodePickupCollected(
            payload with { CollectionId = 0 }));
        return Task.CompletedTask;
    }

    private static async Task CapabilityJournalRecoveryAsync()
    {
        var verifiedCapability = new CampaignCapabilityPayload(
            CampaignCapabilityKind.Recipe,
            0x366089E7U,
            9U,
            1_700_000_000_000L);
        var unknownCapability = verifiedCapability with
        {
            RecordHash = 0xDEADBEEFU
        };
        var verifiedEnvelope = new ProtocolEnvelope(
            MessageType.CampaignCapability,
            1U,
            1U,
            BinaryPayloadCodec.EncodeCampaignCapability(verifiedCapability));
        var unknownEnvelope = new ProtocolEnvelope(
            MessageType.CampaignCapability,
            2U,
            2U,
            BinaryPayloadCodec.EncodeCampaignCapability(unknownCapability));
        Check.True(SidecarRuntime.IsSupportedCampaignCapability(
            verifiedCapability));
        Check.False(SidecarRuntime.IsSupportedCampaignCapability(
            unknownCapability));
        Check.True(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Host,
            verifiedEnvelope));
        Check.True(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Guest,
            verifiedEnvelope));
        Check.False(SidecarRuntime.IsLocalBridgeEnvelopeAuthorized(
            SessionRole.Host,
            unknownEnvelope));
        Check.False(SidecarRuntime.IsPeerEnvelopeAuthorized(
            SessionRole.Guest,
            unknownEnvelope));

        var root = Path.Combine(Path.GetTempPath(), "CoopStory.SelfTest", Guid.NewGuid().ToString("N"));
        var path = Path.Combine(root, "capabilities.json");
        Directory.CreateDirectory(root);
        try
        {
            var journal = new CapabilityJournal();
            var weapon = new CapabilityGrant("weapon:repeating", CapabilityKind.WeaponShopEligibility,
                1_674_213_418U, 10U, 1_700_000_000_000L);
            var replacement = new CapabilityGrant("weapon:repeating", CapabilityKind.WeaponShopEligibility,
                1_674_213_418U, 11U, 1_700_000_000_100L);
            Check.True(journal.Record(weapon));
            Check.False(journal.Record(weapon));
            Check.True(journal.Record(replacement));
            Check.Equal(2, journal.CaptureState().Count);
            Check.Equal(11UL, journal.CaptureReplay().Single().HostEventId);
            Check.True(journal.Acknowledge(11U, 1_700_000_000_200L));
            Check.False(journal.Acknowledge(11U, 1_700_000_000_201L));
            Check.Equal(1_700_000_000_200L,
                journal.CaptureReplay().Single().GuestAcknowledgedAtUnixMilliseconds!.Value);

            var store = new CapabilityJournalStore();
            await store.SaveAsync(path, journal).ConfigureAwait(false);
            var loaded = await store.LoadAsync(path).ConfigureAwait(false);
            Check.Equal(2, loaded.CaptureState().Count);
            Check.Equal(11UL, loaded.CaptureReplay().Single().HostEventId);
            Check.Equal(1_700_000_000_200L,
                loaded.CaptureReplay().Single().GuestAcknowledgedAtUnixMilliseconds!.Value);

            // A second write creates a recoverable backup. Corrupting the
            // primary must never cause the store to invent capabilities.
            await store.SaveAsync(path, loaded).ConfigureAwait(false);
            await File.WriteAllTextAsync(path, "{bad-json").ConfigureAwait(false);
            var recovered = await store.LoadAsync(path).ConfigureAwait(false);
            Check.Equal(2, recovered.CaptureState().Count);
            Check.Equal(11UL, recovered.CaptureReplay().Single().HostEventId);

            var empty = await store.LoadAsync(Path.Combine(root, "missing.json"))
                .ConfigureAwait(false);
            Check.Equal(0, empty.CaptureState().Count);
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    private static async Task ProfileRecoveryAsync()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "CoopStory.SelfTest",
            Guid.NewGuid().ToString("N"));
        var profilePath = Path.Combine(root, "guest-profile.json");
        Directory.CreateDirectory(root);
        try
        {
            using var store = new GuestProfileStore();
            var guestId = Guid.NewGuid();
            var first = GuestProfile.Create(guestId, "Tester") with
            {
                Money = 10m,
                Ammunition = new Dictionary<string, int>
                {
                    ["AMMO_REVOLVER"] = 24
                }
            };
            await store.SaveAsync(profilePath, first).ConfigureAwait(false);
            var loadedFirst = await store.LoadAsync(profilePath).ConfigureAwait(false);
            Check.False(loadedFirst.RecoveredFromBackup);
            Check.Equal(10m, loadedFirst.Profile.Money);

            var second = first with { Money = 25m };
            await store.SaveAsync(profilePath, second).ConfigureAwait(false);
            Check.True(File.Exists(GuestProfileStore.GetBackupPath(profilePath)));
            var loadedSecond = await store.LoadAsync(profilePath).ConfigureAwait(false);
            Check.Equal(25m, loadedSecond.Profile.Money);

            await File.WriteAllTextAsync(profilePath, "{not-json").ConfigureAwait(false);
            var recovered = await store.LoadAsync(profilePath).ConfigureAwait(false);
            Check.True(recovered.RecoveredFromBackup);
            Check.Equal(10m, recovered.Profile.Money);

            await store.SaveAsync(
                profilePath,
                recovered.Profile with { Money = 11m }).ConfigureAwait(false);
            var healed = await store.LoadAsync(profilePath).ConfigureAwait(false);
            Check.Equal(11m, healed.Profile.Money);
            Check.False(
                Directory.EnumerateFiles(root, "*.tmp", SearchOption.TopDirectoryOnly).Any());
        }
        finally
        {
            var resolvedRoot = Path.GetFullPath(root);
            var expectedParent = Path.GetFullPath(
                Path.Combine(Path.GetTempPath(), "CoopStory.SelfTest"));
            if (resolvedRoot.StartsWith(expectedParent, StringComparison.OrdinalIgnoreCase) &&
                Directory.Exists(resolvedRoot))
            {
                Directory.Delete(resolvedRoot, recursive: true);
            }
        }
    }

    private static Task DownedStateAsync()
    {
        var hostId = NetEntityId.Create(10, 1);
        var guestId = NetEntityId.Create(10, 2);
        var host = new DownedStateMachine(hostId);
        var guest = new DownedStateMachine(guestId);

        guest.ApplyHealth(0, 1000);
        Check.Equal(PlayerLifecycle.Downed, guest.State);
        Check.False(guest.TryBeginRevive(hostId, 2.01f, 5000));
        Check.Equal(PlayerLifecycle.Downed, guest.State);
        Check.True(guest.TryBeginRevive(hostId, 1.5f, 10_000));
        Check.False(guest.UpdateRevive(hostId, 1.5f, true, 13_999));
        Check.Equal(PlayerLifecycle.Reviving, guest.State);
        Check.False(guest.UpdateRevive(hostId, 1.5f, false, 14_000));
        Check.Equal(PlayerLifecycle.Downed, guest.State);
        Check.True(guest.TryBeginRevive(hostId, 1f, 20_000));
        Check.True(guest.UpdateRevive(hostId, 1f, true, 24_000));
        Check.Equal(PlayerLifecycle.Alive, guest.State);
        Check.Near(DownedStateMachine.RevivedHealthFraction, guest.HealthFraction);

        host.EnterDowned(30_000);
        guest.EnterDowned(30_000);
        Check.True(DownedStateMachine.ShouldRetryCheckpoint(host, guest));
        return Task.CompletedTask;
    }

    private static async Task NetworkImpairmentAsync()
    {
        Check.Throws<ArgumentOutOfRangeException>(
            () => new NetworkImpairmentProfile { LossRate = 1.1 }.Validate());

        var profile = new NetworkImpairmentProfile
        {
            LatencyMs = 100,
            JitterMs = 20,
            LossRate = 0.25,
            ReorderRate = 0.2,
            Seed = 12345
        };
        var first = new NetworkImpairmentModel(profile);
        var second = new NetworkImpairmentModel(profile);
        var dropped = 0;
        var reordered = 0;
        for (var index = 0; index < 200; index++)
        {
            var left = first.Next();
            var right = second.Next();
            Check.Equal(left, right);
            if (left.Drop)
            {
                dropped++;
            }

            if (left.Reorder)
            {
                reordered++;
            }

            Check.True(left.Delay >= TimeSpan.FromMilliseconds(80));
            Check.True(left.Delay <= TimeSpan.FromMilliseconds(140));
        }

        Check.True(dropped is > 20 and < 90);
        Check.True(reordered > 0);

        var dropAll = new NetworkImpairmentModel(
            new NetworkImpairmentProfile { LossRate = 1, Seed = 7 });
        var invoked = false;
        var delivered = await dropAll.TransmitAsync(
            1,
            (value, token) =>
            {
                _ = value;
                _ = token;
                invoked = true;
                return ValueTask.CompletedTask;
            }).ConfigureAwait(false);
        Check.False(delivered);
        Check.False(invoked);
    }

    private static async Task TcpReconnectLoopbackAsync()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "CoopStory.SelfTest",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);

        JsonLineLogger? logger = null;
        LanSessionHost? firstHost = null;
        LanSessionHost? secondHost = null;
        LanSessionGuest? guest = null;
        CancellationTokenSource? firstHostStop = null;
        CancellationTokenSource? secondHostStop = null;
        CancellationTokenSource? guestStop = null;
        Task? firstHostTask = null;
        Task? secondHostTask = null;
        Task? guestTask = null;

        try
        {
            var tcpPort = FindFreeTcpPort();
            var udpPort = FindFreeUdpPort();
            var credentials = SessionCredentials.Generate();
            var network = new NetworkConfig
            {
                HeartbeatIntervalMs = 100,
                HeartbeatTimeoutMs = 600,
                ReconnectMinMs = 100,
                ReconnectMaxMs = 400
            };
            var baseConfig = new SidecarConfig
            {
                HostAddress = "127.0.0.1",
                TcpPort = tcpPort,
                UdpPort = udpPort,
                SessionToken = credentials.ExportToken(),
                Network = network,
                ProfilePath = Path.Combine(root, "guest-profile.json"),
                LogPath = Path.Combine(root, "network.jsonl")
            };
            var hostConfig = baseConfig with { Role = SessionRole.Host };
            var guestConfig = baseConfig with { Role = SessionRole.Guest };

            logger = new JsonLineLogger(baseConfig.LogPath);
            firstHost = new LanSessionHost(
                hostConfig,
                credentials,
                logger,
                IPAddress.Loopback);
            firstHostStop = new CancellationTokenSource();
            firstHostTask = firstHost.RunAsync(firstHostStop.Token);
            await Task.Delay(50).ConfigureAwait(false);

            var badGuestConfig = guestConfig with
            {
                SessionToken = SessionCredentials.Generate().ExportToken()
            };
            await using (var badGuest = new LanSessionGuest(
                             badGuestConfig,
                             SessionCredentials.ParseToken(badGuestConfig.SessionToken),
                             logger,
                             IPAddress.Loopback))
            {
                var rejectionObserved = new TaskCompletionSource<string>(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                badGuest.AuthenticationRejected += (reason, _) =>
                {
                    rejectionObserved.TrySetResult(reason);
                    return ValueTask.CompletedTask;
                };
                using var badGuestStop = new CancellationTokenSource();
                var badGuestTask = badGuest.RunAsync(badGuestStop.Token);
                await WaitUntilAsync(
                    () => LogContains(
                        baseConfig.LogPath,
                        "Guest handshake rejected: session-id"),
                    TimeSpan.FromSeconds(3),
                    "Host did not record rejection of the mismatched session token.")
                    .ConfigureAwait(false);
                var rejectionReason = await rejectionObserved.Task
                    .WaitAsync(TimeSpan.FromSeconds(2))
                    .ConfigureAwait(false);
                Check.True(
                    rejectionReason.Contains(
                        "session-id",
                        StringComparison.OrdinalIgnoreCase));
                Check.False(badGuest.IsConnected);
                Check.False(firstHost.IsConnected);
                await badGuestStop.CancelAsync().ConfigureAwait(false);
                await IgnoreCancellationAsync(badGuestTask).ConfigureAwait(false);
            }

            guest = new LanSessionGuest(
                guestConfig,
                credentials,
                logger,
                IPAddress.Loopback);
            var localGuestResyncCount = 0;
            var guestConnectedCount = 0;
            var guestDisconnectedCount = 0;
            guest.ConnectionChanged += (connected, _) =>
            {
                if (connected)
                {
                    Interlocked.Increment(ref guestConnectedCount);
                }
                else
                {
                    Interlocked.Increment(ref guestDisconnectedCount);
                }

                return ValueTask.CompletedTask;
            };
            guest.EnvelopeReceived += (envelope, _, _) =>
            {
                if (envelope.Type == MessageType.ResyncRequest)
                {
                    Interlocked.Increment(ref localGuestResyncCount);
                }

                return ValueTask.CompletedTask;
            };
            guestStop = new CancellationTokenSource();
            guestTask = guest.RunAsync(guestStop.Token);
            await WaitUntilAsync(
                () => firstHost.IsConnected && guest.IsConnected,
                TimeSpan.FromSeconds(5),
                "Initial authenticated loopback connection timed out.").ConfigureAwait(false);
            await WaitUntilAsync(
                () => Volatile.Read(ref localGuestResyncCount) >= 1,
                TimeSpan.FromSeconds(2),
                "Guest did not emit automatic local resync after the initial handshake.")
                .ConfigureAwait(false);
            Check.Equal(1, Volatile.Read(ref guestConnectedCount));

            await firstHostStop.CancelAsync().ConfigureAwait(false);
            await IgnoreCancellationAsync(firstHostTask).ConfigureAwait(false);
            await firstHost.DisposeAsync().ConfigureAwait(false);
            await WaitUntilAsync(
                () => !guest.IsConnected,
                TimeSpan.FromSeconds(3),
                "Guest did not observe the forced disconnect.").ConfigureAwait(false);
            await WaitUntilAsync(
                () => Volatile.Read(ref guestDisconnectedCount) >= 1,
                TimeSpan.FromSeconds(2),
                "Guest did not publish its disconnected transition.")
                .ConfigureAwait(false);

            secondHost = new LanSessionHost(
                hostConfig,
                credentials,
                logger,
                IPAddress.Loopback);
            secondHostStop = new CancellationTokenSource();
            var receivedAfterReconnect = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            secondHost.EnvelopeReceived += (envelope, _, _) =>
            {
                if (envelope.Type == MessageType.ResyncRequest)
                {
                    receivedAfterReconnect.TrySetResult();
                }

                return ValueTask.CompletedTask;
            };

            var stopwatch = Stopwatch.StartNew();
            secondHostTask = secondHost.RunAsync(secondHostStop.Token);
            await WaitUntilAsync(
                () => secondHost.IsConnected && guest.IsConnected,
                TimeSpan.FromSeconds(10),
                "Guest did not reconnect and re-authenticate within ten seconds.")
                .ConfigureAwait(false);
            stopwatch.Stop();
            Check.True(
                stopwatch.Elapsed < TimeSpan.FromSeconds(10),
                $"Reconnect took {stopwatch.Elapsed.TotalSeconds:F2} seconds.");

            await receivedAfterReconnect.Task
                .WaitAsync(TimeSpan.FromSeconds(2))
                .ConfigureAwait(false);
            await WaitUntilAsync(
                () => Volatile.Read(ref localGuestResyncCount) >= 2,
                TimeSpan.FromSeconds(2),
                "Guest did not emit automatic local resync after reconnect.")
                .ConfigureAwait(false);
            await WaitUntilAsync(
                () => Volatile.Read(ref guestConnectedCount) >= 2,
                TimeSpan.FromSeconds(2),
                "Guest did not publish its reconnected transition.")
                .ConfigureAwait(false);
            Check.Equal(2, Volatile.Read(ref guestConnectedCount));
        }
        finally
        {
            if (guestStop is not null)
            {
                await guestStop.CancelAsync().ConfigureAwait(false);
            }

            if (secondHostStop is not null)
            {
                await secondHostStop.CancelAsync().ConfigureAwait(false);
            }

            if (firstHostStop is not null)
            {
                await firstHostStop.CancelAsync().ConfigureAwait(false);
            }

            await IgnoreCancellationAsync(guestTask, secondHostTask, firstHostTask)
                .ConfigureAwait(false);

            if (guest is not null)
            {
                await guest.DisposeAsync().ConfigureAwait(false);
            }

            if (secondHost is not null)
            {
                await secondHost.DisposeAsync().ConfigureAwait(false);
            }

            if (firstHost is not null)
            {
                await firstHost.DisposeAsync().ConfigureAwait(false);
            }

            guestStop?.Dispose();
            secondHostStop?.Dispose();
            firstHostStop?.Dispose();

            if (logger is not null)
            {
                await logger.DisposeAsync().ConfigureAwait(false);
            }

            var resolvedRoot = Path.GetFullPath(root);
            var expectedParent = Path.GetFullPath(
                Path.Combine(Path.GetTempPath(), "CoopStory.SelfTest"));
            if (resolvedRoot.StartsWith(expectedParent, StringComparison.OrdinalIgnoreCase) &&
                Directory.Exists(resolvedRoot))
            {
                Directory.Delete(resolvedRoot, recursive: true);
            }
        }
    }

    private static async Task UdpBindingAndReplayAsync()
    {
        var animationBinding = new UdpPeerBinding(
            IPAddress.Loopback,
            expectedPort: 43121);
        var animationEndpoint = new IPEndPoint(
            IPAddress.Loopback,
            43121);
        Check.True(
            animationBinding.TryAccept(
                animationEndpoint,
                new ProtocolEnvelope(
                    MessageType.PlayerAnimationState,
                    1,
                    1,
                    ReadOnlyMemory<byte>.Empty),
                out var animationRejection),
            $"UDP binding rejected AnimGraph snapshot: {animationRejection}");

        var root = Path.Combine(
            Path.GetTempPath(),
            "CoopStory.SelfTest",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);

        JsonLineLogger? logger = null;
        LanSessionHost? host = null;
        LanSessionGuest? guest = null;
        CancellationTokenSource? hostStop = null;
        CancellationTokenSource? guestStop = null;
        Task? hostTask = null;
        Task? guestTask = null;
        TcpClient? firstTcpGuest = null;
        TcpClient? secondTcpGuest = null;
        try
        {
            var credentials = SessionCredentials.Generate();
            var config = new SidecarConfig
            {
                Role = SessionRole.Host,
                HostAddress = "127.0.0.1",
                TcpPort = FindFreeTcpPort(),
                UdpPort = FindFreeUdpPort(),
                SessionToken = credentials.ExportToken(),
                ProfilePath = Path.Combine(root, "guest-profile.json"),
                LogPath = Path.Combine(root, "udp-binding.jsonl"),
                Network = new NetworkConfig
                {
                    HeartbeatIntervalMs = 1000,
                    HeartbeatTimeoutMs = 8000,
                    ReconnectMinMs = 100,
                    ReconnectMaxMs = 400
                }
            };

            logger = new JsonLineLogger(config.LogPath);
            host = new LanSessionHost(
                config,
                credentials,
                logger,
                IPAddress.Loopback);
            var hostUdpReceived = 0;
            host.EnvelopeReceived += (envelope, _, _) =>
            {
                if (envelope.Type == MessageType.PlayerState)
                {
                    Interlocked.Increment(ref hostUdpReceived);
                }

                return ValueTask.CompletedTask;
            };
            hostStop = new CancellationTokenSource();
            hostTask = host.RunAsync(hostStop.Token);
            await WaitUntilAsync(
                () => LogContains(config.LogPath, "network.host.started"),
                TimeSpan.FromSeconds(2),
                "UDP binding host did not start.").ConfigureAwait(false);

            var hostUdpEndpoint = new IPEndPoint(
                IPAddress.Loopback,
                config.UdpPort);
            var rawGuestInstanceId = Guid.NewGuid();
            using var firstUdpGuest = new UdpClient(
                new IPEndPoint(IPAddress.Loopback, 0));
            await SendAuthenticatedUdpAsync(
                firstUdpGuest,
                hostUdpEndpoint,
                credentials,
                2,
                rawGuestInstanceId).ConfigureAwait(false);
            await Task.Delay(100).ConfigureAwait(false);
            Check.Equal(0, Volatile.Read(ref hostUdpReceived));

            firstTcpGuest = await ConnectRawGuestAsync(
                config,
                credentials,
                rawGuestInstanceId).ConfigureAwait(false);
            await WaitUntilAsync(
                () => host.IsConnected,
                TimeSpan.FromSeconds(2),
                "Raw guest did not authenticate.").ConfigureAwait(false);

            await SendAuthenticatedUdpAsync(
                firstUdpGuest,
                hostUdpEndpoint,
                credentials,
                2,
                rawGuestInstanceId).ConfigureAwait(false);
            await WaitUntilAsync(
                () => Volatile.Read(ref hostUdpReceived) == 1,
                TimeSpan.FromSeconds(2),
                "First bound UDP datagram was not accepted.").ConfigureAwait(false);

            await SendAuthenticatedUdpAsync(
                firstUdpGuest,
                hostUdpEndpoint,
                credentials,
                2,
                rawGuestInstanceId).ConfigureAwait(false);
            await Task.Delay(100).ConfigureAwait(false);
            Check.Equal(1, Volatile.Read(ref hostUdpReceived));

            await SendAuthenticatedUdpAsync(
                firstUdpGuest,
                hostUdpEndpoint,
                credentials,
                4,
                rawGuestInstanceId).ConfigureAwait(false);
            await WaitUntilAsync(
                () => Volatile.Read(ref hostUdpReceived) == 2,
                TimeSpan.FromSeconds(2),
                "Newer UDP sequence was not accepted.").ConfigureAwait(false);

            await SendAuthenticatedUdpAsync(
                firstUdpGuest,
                hostUdpEndpoint,
                credentials,
                3,
                rawGuestInstanceId).ConfigureAwait(false);
            await WaitUntilAsync(
                () => Volatile.Read(ref hostUdpReceived) == 3,
                TimeSpan.FromSeconds(2),
                "Unseen in-window reordered UDP datagram was not accepted.")
                .ConfigureAwait(false);

            await SendAuthenticatedUdpAsync(
                firstUdpGuest,
                hostUdpEndpoint,
                credentials,
                3,
                rawGuestInstanceId).ConfigureAwait(false);
            await Task.Delay(100).ConfigureAwait(false);
            Check.Equal(3, Volatile.Read(ref hostUdpReceived));

            using var wrongPort = new UdpClient(
                new IPEndPoint(IPAddress.Loopback, 0));
            await SendAuthenticatedUdpAsync(
                wrongPort,
                hostUdpEndpoint,
                credentials,
                1,
                rawGuestInstanceId).ConfigureAwait(false);
            using var wrongAddress = new UdpClient(
                new IPEndPoint(IPAddress.Parse("127.0.0.2"), 0));
            await SendAuthenticatedUdpAsync(
                wrongAddress,
                hostUdpEndpoint,
                credentials,
                1,
                rawGuestInstanceId).ConfigureAwait(false);
            await Task.Delay(100).ConfigureAwait(false);
            Check.Equal(3, Volatile.Read(ref hostUdpReceived));

            await SendAuthenticatedUdpAsync(
                firstUdpGuest,
                hostUdpEndpoint,
                credentials,
                5,
                rawGuestInstanceId).ConfigureAwait(false);
            await WaitUntilAsync(
                () => Volatile.Read(ref hostUdpReceived) == 4,
                TimeSpan.FromSeconds(2),
                "Wrong endpoint advanced the replay window or stole the binding.")
                .ConfigureAwait(false);

            await SendAuthenticatedUdpAsync(
                firstUdpGuest,
                hostUdpEndpoint,
                credentials,
                uint.MaxValue - 64,
                rawGuestInstanceId).ConfigureAwait(false);
            await Task.Delay(100).ConfigureAwait(false);
            Check.Equal(4, Volatile.Read(ref hostUdpReceived));

            firstTcpGuest.Dispose();
            firstTcpGuest = null;
            await WaitUntilAsync(
                () => !host.IsConnected,
                TimeSpan.FromSeconds(2),
                "Host retained UDP binding after TCP disconnect.")
                .ConfigureAwait(false);
            await SendAuthenticatedUdpAsync(
                firstUdpGuest,
                hostUdpEndpoint,
                credentials,
                6,
                rawGuestInstanceId).ConfigureAwait(false);
            await Task.Delay(100).ConfigureAwait(false);
            Check.Equal(4, Volatile.Read(ref hostUdpReceived));

            var staleRawGuestInstanceId = rawGuestInstanceId;
            rawGuestInstanceId = Guid.NewGuid();
            secondTcpGuest = await ConnectRawGuestAsync(
                config,
                credentials,
                rawGuestInstanceId).ConfigureAwait(false);
            await WaitUntilAsync(
                () => host.IsConnected,
                TimeSpan.FromSeconds(2),
                "Raw guest did not reconnect.").ConfigureAwait(false);
            await SendAuthenticatedUdpAsync(
                firstUdpGuest,
                hostUdpEndpoint,
                credentials,
                7,
                staleRawGuestInstanceId).ConfigureAwait(false);
            await Task.Delay(100).ConfigureAwait(false);
            Check.True(
                Volatile.Read(ref hostUdpReceived) == 4,
                "A delayed datagram from the replaced TCP instance was accepted.");
            using var secondUdpGuest = new UdpClient(
                new IPEndPoint(IPAddress.Loopback, 0));
            await SendAuthenticatedUdpAsync(
                secondUdpGuest,
                hostUdpEndpoint,
                credentials,
                77,
                rawGuestInstanceId).ConfigureAwait(false);
            await WaitUntilAsync(
                () => Volatile.Read(ref hostUdpReceived) == 5,
                TimeSpan.FromSeconds(2),
                "Reconnect did not reset and re-pin the UDP peer.")
                .ConfigureAwait(false);

            secondTcpGuest.Dispose();
            secondTcpGuest = null;
            await WaitUntilAsync(
                () => !host.IsConnected,
                TimeSpan.FromSeconds(2),
                "Second raw guest did not disconnect.").ConfigureAwait(false);

            var guestConfig = config with { Role = SessionRole.Guest };
            guest = new LanSessionGuest(
                guestConfig,
                credentials,
                logger,
                IPAddress.Loopback);
            var guestUdpReceived = 0;
            guest.EnvelopeReceived += (envelope, _, _) =>
            {
                if (envelope.Type == MessageType.PlayerState)
                {
                    Interlocked.Increment(ref guestUdpReceived);
                }

                return ValueTask.CompletedTask;
            };
            guestStop = new CancellationTokenSource();
            guestTask = guest.RunAsync(guestStop.Token);
            await WaitUntilAsync(
                () => host.IsConnected && guest.IsConnected,
                TimeSpan.FromSeconds(3),
                "Managed guest did not connect for endpoint validation.")
                .ConfigureAwait(false);

            Check.True(await guest.SendSnapshotAsync(
                MessageType.PlayerState,
                ReadOnlyMemory<byte>.Empty,
                unchecked((ulong)Environment.TickCount64)).ConfigureAwait(false));
            await WaitUntilAsync(
                () => Volatile.Read(ref hostUdpReceived) == 6,
                TimeSpan.FromSeconds(2),
                "Managed guest did not establish its UDP endpoint.")
                .ConfigureAwait(false);

            var guestUdpEndpoint = guest.LocalUdpEndpoint;
            Check.NotNull(guestUdpEndpoint);
            await SendAuthenticatedUdpAsync(
                wrongPort,
                guestUdpEndpoint!,
                credentials,
                123_456,
                Guid.NewGuid()).ConfigureAwait(false);
            await Task.Delay(100).ConfigureAwait(false);
            Check.Equal(0, Volatile.Read(ref guestUdpReceived));

            Check.True(await host.SendSnapshotAsync(
                MessageType.PlayerState,
                ReadOnlyMemory<byte>.Empty,
                unchecked((ulong)Environment.TickCount64)).ConfigureAwait(false));
            await WaitUntilAsync(
                () => Volatile.Read(ref guestUdpReceived) == 1,
                TimeSpan.FromSeconds(2),
                "Guest rejected a valid datagram from the exact host endpoint.")
                .ConfigureAwait(false);
        }
        finally
        {
            firstTcpGuest?.Dispose();
            secondTcpGuest?.Dispose();
            if (guestStop is not null)
            {
                await guestStop.CancelAsync().ConfigureAwait(false);
            }
            if (hostStop is not null)
            {
                await hostStop.CancelAsync().ConfigureAwait(false);
            }

            await IgnoreCancellationAsync(guestTask, hostTask).ConfigureAwait(false);
            if (guest is not null)
            {
                await guest.DisposeAsync().ConfigureAwait(false);
            }
            if (host is not null)
            {
                await host.DisposeAsync().ConfigureAwait(false);
            }

            guestStop?.Dispose();
            hostStop?.Dispose();
            if (logger is not null)
            {
                await logger.DisposeAsync().ConfigureAwait(false);
            }

            DeleteOwnedSelfTestRoot(root);
        }
    }

    private static async Task<TcpClient> ConnectRawGuestAsync(
        SidecarConfig config,
        SessionCredentials credentials,
        Guid instanceId)
    {
        var client = new TcpClient(AddressFamily.InterNetwork);
        try
        {
            await client.ConnectAsync(
                IPAddress.Loopback,
                config.TcpPort).ConfigureAwait(false);
            var nonce = SessionCredentials.CreateNonce();
            var hello = new HelloPayload(
                credentials.SessionId,
                instanceId,
                SessionRole.Guest,
                nonce,
                credentials.CreateClientProof(
                    instanceId,
                    SessionRole.Guest,
                    nonce));
            await ProtocolCodec.WriteAsync(
                client.GetStream(),
                new ProtocolEnvelope(
                    MessageType.Hello,
                    1,
                    unchecked((ulong)Environment.TickCount64),
                    PayloadJson.Serialize(hello))).ConfigureAwait(false);
            var acknowledgement = await ProtocolCodec.ReadAsync(
                    client.GetStream())
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(2))
                .ConfigureAwait(false);
            Check.NotNull(acknowledgement);
            Check.Equal(MessageType.HelloAck, acknowledgement!.Type);
            var ack = PayloadJson.Deserialize<HelloAckPayload>(
                acknowledgement.Payload.Span);
            Check.True(ack.Accepted);
            Check.Equal(credentials.SessionId, ack.SessionId);
            Check.True(credentials.VerifyServerProof(
                ack.HostInstanceId,
                instanceId,
                nonce,
                ack.ServerNonce,
                ack.Proof));
            return client;
        }
        catch
        {
            client.Dispose();
            throw;
        }
    }

    private static async Task SendAuthenticatedUdpAsync(
        UdpClient sender,
        IPEndPoint destination,
        SessionCredentials credentials,
        uint sequence,
        Guid senderInstanceId)
    {
        var datagram = AuthenticatedDatagramCodec.Encode(
            new ProtocolEnvelope(
                MessageType.PlayerState,
                sequence,
                unchecked((ulong)Environment.TickCount64),
                ReadOnlyMemory<byte>.Empty),
            credentials,
            senderInstanceId);
        _ = await sender.SendAsync(
            datagram,
            destination).ConfigureAwait(false);
    }

    private static async Task NetworkBridgeDeliveryPumpAsync()
    {
        await ExerciseBoundedCoalescingPumpAsync().ConfigureAwait(false);
        await ExerciseEntityUpdateCoalescingPumpAsync().ConfigureAwait(false);
        await ExercisePlayerActionFifoPumpAsync().ConfigureAwait(false);
        await ExerciseCinematicPumpLanesAsync().ConfigureAwait(false);
        await ExerciseCrossLaneReplayOrderPumpAsync().ConfigureAwait(false);
        await ExerciseCoalescedReplacementOrderPumpAsync()
            .ConfigureAwait(false);
        await ExerciseValidCoalescedRetainsCausalOrderPumpAsync()
            .ConfigureAwait(false);
        await ExerciseDeliveryBarrierAsync().ConfigureAwait(false);
        await ExercisePeerGenerationInvalidationPumpAsync()
            .ConfigureAwait(false);
        await ExerciseUncancelledPumpFrameAsync().ConfigureAwait(false);
    }

    private static async Task ExercisePeerGenerationInvalidationPumpAsync()
    {
        var firstDeliveryStarted = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirstDelivery = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var delivered = new List<ProtocolEnvelope>();
        var deliveryCalls = 0;
        async ValueTask<bool> DeliverAsync(ProtocolEnvelope envelope)
        {
            if (Interlocked.Increment(ref deliveryCalls) == 1)
            {
                firstDeliveryStarted.TrySetResult(true);
                await releaseFirstDelivery.Task.ConfigureAwait(false);
            }

            lock (delivered)
            {
                delivered.Add(envelope);
            }
            return true;
        }

        var peer = new GenerationBoundLanSession();
        Check.True(peer.TryCaptureControlPeer(out var oldPeer));
        var pump = new NetworkBridgeDeliveryPump(DeliverAsync);
        using var stop = new CancellationTokenSource();
        var run = pump.RunAsync(stop.Token);
        _ = pump.TryEnqueue(PumpEnvelope(MessageType.Command, 1));
        await firstDeliveryStarted.Task
            .WaitAsync(TimeSpan.FromSeconds(2))
            .ConfigureAwait(false);

        Func<bool> oldPeerIsCurrent = () =>
            peer.IsControlPeerCurrent(oldPeer);
        _ = pump.TryEnqueue(
            PumpEnvelope(MessageType.EntitySpawn, 2),
            oldPeerIsCurrent);
        _ = pump.TryEnqueue(
            PumpEnvelope(MessageType.PlayerState, 3),
            oldPeerIsCurrent);
        _ = pump.TryEnqueue(
            PumpEntityUpdate(NetEntityId.Create(88, 1), 4),
            oldPeerIsCurrent);

        var newPeer = peer.ReplacePeer();
        _ = pump.TryEnqueue(
            PumpEnvelope(MessageType.PlayerAnimationState, 5),
            () => peer.IsControlPeerCurrent(newPeer));
        releaseFirstDelivery.TrySetResult(true);

        await WaitUntilAsync(
                () =>
                {
                    lock (delivered)
                    {
                        return delivered.Count == 2 &&
                            pump.ReadSnapshot().Backlog == 0;
                    }
                },
                TimeSpan.FromSeconds(2),
                "Generation-bound pump did not discard the stale peer backlog.")
            .ConfigureAwait(false);
        await stop.CancelAsync().ConfigureAwait(false);
        await run.WaitAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(false);

        lock (delivered)
        {
            Check.Equal(
                "Command:1,PlayerAnimationState:5",
                string.Join(',', delivered.Select(static envelope =>
                    $"{envelope.Type}:{envelope.Sequence}")));
        }
        var snapshot = pump.ReadSnapshot();
        Check.Equal(3L, snapshot.Invalidated);
        Check.Equal(2L, snapshot.Delivered);
        Check.Equal(0L, snapshot.Unavailable);
    }

    private static async Task ExerciseDeliveryBarrierAsync()
    {
        var firstStarted = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirst = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var delivered = new List<string>();
        var deliveryCalls = 0;
        async ValueTask<bool> DeliverAsync(ProtocolEnvelope envelope)
        {
            if (Interlocked.Increment(ref deliveryCalls) == 1)
            {
                firstStarted.TrySetResult(true);
                await releaseFirst.Task.ConfigureAwait(false);
            }
            lock (delivered)
            {
                delivered.Add(envelope.Sequence.ToString());
            }
            return true;
        }

        var pump = new NetworkBridgeDeliveryPump(DeliverAsync);
        using var stop = new CancellationTokenSource();
        var run = pump.RunAsync(stop.Token);
        _ = pump.TryEnqueue(PumpEnvelope(MessageType.Command, 1));
        await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(2))
            .ConfigureAwait(false);
        _ = pump.TryEnqueue(PumpEnvelope(MessageType.EntitySpawn, 2));

        var barrierTask = pump.EnterDeliveryBarrierAsync()
            .AsTask();
        await Task.Yield();
        Check.False(barrierTask.IsCompleted);
        _ = pump.TryEnqueue(PumpEnvelope(MessageType.MissionState, 3));
        releaseFirst.TrySetResult(true);
        await using (var barrier = await barrierTask.ConfigureAwait(false))
        {
            lock (delivered)
            {
                Check.Equal("1", string.Join(',', delivered));
                delivered.Add("reset");
            }
            await Task.Delay(25).ConfigureAwait(false);
            lock (delivered)
            {
                Check.Equal("1,reset", string.Join(',', delivered));
            }
        }

        await WaitUntilAsync(
                () =>
                {
                    lock (delivered)
                    {
                        return delivered.Count == 3;
                    }
                },
                TimeSpan.FromSeconds(2),
                "Delivery barrier did not resume post-reset traffic.")
            .ConfigureAwait(false);
        await stop.CancelAsync().ConfigureAwait(false);
        await run.WaitAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(false);
        lock (delivered)
        {
            Check.Equal("1,reset,3", string.Join(',', delivered));
        }
    }

    private static async Task ExerciseCinematicPumpLanesAsync()
    {
        var firstDeliveryStarted = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirstDelivery = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var delivered = new List<ProtocolEnvelope>();
        var deliveryCalls = 0;
        async ValueTask<bool> DeliverAsync(ProtocolEnvelope envelope)
        {
            if (Interlocked.Increment(ref deliveryCalls) == 1)
            {
                firstDeliveryStarted.TrySetResult(true);
                await releaseFirstDelivery.Task.ConfigureAwait(false);
            }
            lock (delivered)
            {
                delivered.Add(envelope);
            }
            return true;
        }

        var pump = new NetworkBridgeDeliveryPump(
            DeliverAsync,
            criticalCapacity: 4);
        using var stop = new CancellationTokenSource();
        var run = pump.RunAsync(stop.Token);
        _ = pump.TryEnqueue(PumpEnvelope(MessageType.Command, 1));
        await firstDeliveryStarted.Task.WaitAsync(TimeSpan.FromSeconds(2))
            .ConfigureAwait(false);
        Check.Equal(NetworkBridgeEnqueueDisposition.Queued,
            pump.TryEnqueue(PumpEnvelope(
                MessageType.MissionCinematicState, 10)).Disposition);
        Check.Equal(NetworkBridgeEnqueueDisposition.Queued,
            pump.TryEnqueue(PumpEnvelope(
                MessageType.MissionCinematicState, 11)).Disposition);
        Check.Equal(NetworkBridgeEnqueueDisposition.Queued,
            pump.TryEnqueue(PumpEnvelope(
                MessageType.MissionCinematicAction, 20)).Disposition);
        Check.Equal(NetworkBridgeEnqueueDisposition.Queued,
            pump.TryEnqueue(PumpEnvelope(
                MessageType.MissionCinematicAction, 21)).Disposition);
        releaseFirstDelivery.TrySetResult(true);
        await WaitUntilAsync(
                () =>
                {
                    lock (delivered)
                    {
                        return delivered.Count == 5;
                    }
                },
                TimeSpan.FromSeconds(2),
                "Cinematic pump lanes did not drain.")
            .ConfigureAwait(false);
        await stop.CancelAsync().ConfigureAwait(false);
        await run.WaitAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(false);
        ProtocolEnvelope[] observed;
        lock (delivered)
        {
            observed = delivered.ToArray();
        }
        Check.Equal("10,11", string.Join(',', observed
            .Where(item => item.Type == MessageType.MissionCinematicState)
            .Select(item => item.Sequence)));
        Check.Equal("20,21", string.Join(',', observed
            .Where(item => item.Type == MessageType.MissionCinematicAction)
            .Select(item => item.Sequence)));
    }

    private static async Task ExerciseCrossLaneReplayOrderPumpAsync()
    {
        var firstDeliveryStarted = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirstDelivery = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var delivered = new List<uint>();
        var deliveryCalls = 0;
        async ValueTask<bool> DeliverAsync(ProtocolEnvelope envelope)
        {
            if (Interlocked.Increment(ref deliveryCalls) == 1)
            {
                firstDeliveryStarted.TrySetResult(true);
                await releaseFirstDelivery.Task.ConfigureAwait(false);
            }
            lock (delivered)
            {
                delivered.Add(envelope.Sequence);
            }
            return true;
        }

        var pump = new NetworkBridgeDeliveryPump(DeliverAsync);
        using var stop = new CancellationTokenSource();
        var run = pump.RunAsync(stop.Token);
        _ = pump.TryEnqueue(PumpEnvelope(MessageType.Command, 1));
        await firstDeliveryStarted.Task.WaitAsync(TimeSpan.FromSeconds(2))
            .ConfigureAwait(false);
        _ = pump.TryEnqueue(PumpEnvelope(MessageType.MissionState, 100));
        _ = pump.TryEnqueue(PumpEnvelope(
            MessageType.MissionCinematicState,
            101));
        _ = pump.TryEnqueue(PumpEnvelope(MessageType.EntitySpawn, 102));
        _ = pump.TryEnqueue(PumpEnvelope(
            MessageType.AnimSceneDefinition,
            103));
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Queued,
            pump.TryEnqueue(PumpEnvelope(
                MessageType.MissionState,
                104)).Disposition);

        releaseFirstDelivery.TrySetResult(true);
        await WaitUntilAsync(
                () =>
                {
                    lock (delivered)
                    {
                        return delivered.Count == 6;
                    }
                },
                TimeSpan.FromSeconds(2),
                "Mission/world/definition replay crossed pump lanes out of order.")
            .ConfigureAwait(false);
        await stop.CancelAsync().ConfigureAwait(false);
        await run.WaitAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(false);
        lock (delivered)
        {
            Check.Equal(
                "1,100,101,102,103,104",
                string.Join(',', delivered));
        }
    }

    private static async Task ExerciseCoalescedReplacementOrderPumpAsync()
    {
        var firstDeliveryStarted = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirstDelivery = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var delivered = new List<string>();
        var deliveryCalls = 0;
        async ValueTask<bool> DeliverAsync(ProtocolEnvelope envelope)
        {
            if (Interlocked.Increment(ref deliveryCalls) == 1)
            {
                firstDeliveryStarted.TrySetResult(true);
                await releaseFirstDelivery.Task.ConfigureAwait(false);
            }
            lock (delivered)
            {
                delivered.Add($"{envelope.Type}:{envelope.Sequence}");
            }
            return true;
        }

        var pump = new NetworkBridgeDeliveryPump(DeliverAsync);
        using var stop = new CancellationTokenSource();
        var run = pump.RunAsync(stop.Token);
        _ = pump.TryEnqueue(PumpEnvelope(MessageType.Command, 1));
        await firstDeliveryStarted.Task.WaitAsync(TimeSpan.FromSeconds(2))
            .ConfigureAwait(false);

        var oldPeerCurrent = true;
        _ = pump.TryEnqueue(
            PumpEnvelope(MessageType.PlayerState, 100),
            () => oldPeerCurrent);
        _ = pump.TryEnqueue(PumpEnvelope(MessageType.ResyncRequest, 101));
        oldPeerCurrent = false;
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Coalesced,
            pump.TryEnqueue(
                PumpEnvelope(MessageType.PlayerState, 2),
                static () => true).Disposition);

        releaseFirstDelivery.TrySetResult(true);
        await WaitUntilAsync(
                () =>
                {
                    lock (delivered)
                    {
                        return delivered.Count == 3;
                    }
                },
                TimeSpan.FromSeconds(2),
                "A fresh coalesced snapshot crossed its preceding resync boundary.")
            .ConfigureAwait(false);
        await stop.CancelAsync().ConfigureAwait(false);
        await run.WaitAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(false);
        lock (delivered)
        {
            Check.Equal(
                "Command:1,ResyncRequest:101,PlayerState:2",
                string.Join(',', delivered));
        }
    }

    private static async Task ExerciseValidCoalescedRetainsCausalOrderPumpAsync()
    {
        var firstDeliveryStarted = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirstDelivery = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var delivered = new List<string>();
        var deliveryCalls = 0;
        async ValueTask<bool> DeliverAsync(ProtocolEnvelope envelope)
        {
            if (Interlocked.Increment(ref deliveryCalls) == 1)
            {
                firstDeliveryStarted.TrySetResult(true);
                await releaseFirstDelivery.Task.ConfigureAwait(false);
            }
            lock (delivered)
            {
                delivered.Add($"{envelope.Type}:{envelope.Sequence}");
            }
            return true;
        }

        var pump = new NetworkBridgeDeliveryPump(DeliverAsync);
        using var stop = new CancellationTokenSource();
        var run = pump.RunAsync(stop.Token);
        _ = pump.TryEnqueue(PumpEnvelope(MessageType.Command, 1));
        await firstDeliveryStarted.Task.WaitAsync(TimeSpan.FromSeconds(2))
            .ConfigureAwait(false);

        _ = pump.TryEnqueue(
            PumpEnvelope(MessageType.PlayerState, 100),
            static () => true);
        _ = pump.TryEnqueue(PumpEnvelope(MessageType.PlayerAction, 101));
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Coalesced,
            pump.TryEnqueue(
                PumpEnvelope(MessageType.PlayerState, 102),
                static () => true).Disposition);

        releaseFirstDelivery.TrySetResult(true);
        await WaitUntilAsync(
                () =>
                {
                    lock (delivered)
                    {
                        return delivered.Count == 3;
                    }
                },
                TimeSpan.FromSeconds(2),
                "A current peer's latest PlayerState moved behind its dependent action.")
            .ConfigureAwait(false);
        await stop.CancelAsync().ConfigureAwait(false);
        await run.WaitAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(false);
        lock (delivered)
        {
            Check.Equal(
                "Command:1,PlayerState:102,PlayerAction:101",
                string.Join(',', delivered));
        }
    }

    private static async Task ExerciseBoundedCoalescingPumpAsync()
    {
        var firstDeliveryStarted = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirstDelivery = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var delivered = new List<ProtocolEnvelope>();
        var deliveredSync = new object();
        var deliveryCalls = 0;

        async ValueTask<bool> DeliverAsync(ProtocolEnvelope envelope)
        {
            if (Interlocked.Increment(ref deliveryCalls) == 1)
            {
                firstDeliveryStarted.TrySetResult(true);
                await releaseFirstDelivery.Task.ConfigureAwait(false);
            }

            lock (deliveredSync)
            {
                delivered.Add(envelope);
            }

            return true;
        }

        var pump = new NetworkBridgeDeliveryPump(
            DeliverAsync,
            criticalCapacity: 3);
        using var stop = new CancellationTokenSource();
        var run = pump.RunAsync(stop.Token);

        Check.Equal(
            NetworkBridgeEnqueueDisposition.Queued,
            pump.TryEnqueue(PumpEnvelope(MessageType.Command, 1)).Disposition);
        await firstDeliveryStarted.Task
            .WaitAsync(TimeSpan.FromSeconds(2))
            .ConfigureAwait(false);

        Check.Equal(
            NetworkBridgeEnqueueDisposition.Queued,
            pump.TryEnqueue(PumpEnvelope(MessageType.EntitySpawn, 2)).Disposition);
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Queued,
            pump.TryEnqueue(PumpEnvelope(MessageType.EntityDespawn, 3)).Disposition);
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Queued,
            pump.TryEnqueue(PumpEnvelope(MessageType.DamageIntent, 4)).Disposition);
        var rejected = pump.TryEnqueue(
            PumpEnvelope(MessageType.ReviveRequest, 5));
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Rejected,
            rejected.Disposition);

        Check.Equal(
            NetworkBridgeEnqueueDisposition.Queued,
            pump.TryEnqueue(PumpEnvelope(MessageType.PlayerState, 10)).Disposition);
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Coalesced,
            pump.TryEnqueue(PumpEnvelope(MessageType.PlayerState, 11)).Disposition);
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Coalesced,
            pump.TryEnqueue(PumpEnvelope(MessageType.PlayerState, 10)).Disposition);
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Queued,
            pump.TryEnqueue(PumpEnvelope(
                MessageType.WorldState,
                uint.MaxValue)).Disposition);
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Coalesced,
            pump.TryEnqueue(PumpEnvelope(MessageType.WorldState, 1)).Disposition);
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Coalesced,
            pump.TryEnqueue(PumpEnvelope(
                MessageType.WorldState,
                uint.MaxValue)).Disposition);
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Queued,
            pump.TryEnqueue(PumpEnvelope(MessageType.EquipmentState, 30)).Disposition);
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Queued,
            pump.TryEnqueue(PumpEnvelope(MessageType.PlayerIdentity, 40)).Disposition);
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Queued,
            pump.TryEnqueue(PumpEnvelope(
                MessageType.PlayerAnimationState,
                50)).Disposition);
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Coalesced,
            pump.TryEnqueue(PumpEnvelope(
                MessageType.PlayerAnimationState,
                51)).Disposition);
        var blocked = pump.ReadSnapshot();
        Check.Equal(9, blocked.Backlog);
        Check.Equal(
            MessageType.Command,
            blocked.ActiveType ??
            throw new SelfTestException("Delivery pump did not report its active frame."));
        Check.True(blocked.MaxBacklog <= 9);

        releaseFirstDelivery.TrySetResult(true);
        await WaitUntilAsync(
                () =>
                {
                    lock (deliveredSync)
                    {
                        return delivered.Count == 9;
                    }
                },
                TimeSpan.FromSeconds(2),
                "Delivery pump did not drain its bounded backlog.")
            .ConfigureAwait(false);

        await stop.CancelAsync().ConfigureAwait(false);
        await run.WaitAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(false);

        ProtocolEnvelope[] observed;
        lock (deliveredSync)
        {
            observed = delivered.ToArray();
        }

        var orderedCritical = observed
            .Where(item => item.Type is not (
                MessageType.PlayerState or
                MessageType.PlayerAnimationState or
                MessageType.WorldState or
                MessageType.EquipmentState or
                MessageType.PlayerIdentity))
            .Select(static item => item.Sequence)
            .ToArray();
        Check.Equal("1,2,3,4", string.Join(',', orderedCritical));
        Check.False(observed.Any(static item =>
            item.Type == MessageType.PlayerState && item.Sequence == 10));
        Check.True(observed.Any(static item =>
            item.Type == MessageType.PlayerState && item.Sequence == 11));
        Check.False(observed.Any(static item =>
            item.Type == MessageType.PlayerAnimationState &&
            item.Sequence == 50));
        Check.True(observed.Any(static item =>
            item.Type == MessageType.PlayerAnimationState &&
            item.Sequence == 51));
        Check.False(observed.Any(static item =>
            item.Type == MessageType.WorldState && item.Sequence == 20));
        Check.True(observed.Any(static item =>
            item.Type == MessageType.WorldState && item.Sequence == 1));
        Check.False(observed.Any(static item =>
            item.Type == MessageType.WorldState &&
            item.Sequence == uint.MaxValue));

        var final = pump.ReadSnapshot();
        Check.Equal(14L, final.Queued);
        Check.Equal(5L, final.Coalesced);
        Check.Equal(1L, final.Rejected);
        Check.Equal(9L, final.Dequeued);
        Check.Equal(9L, final.Delivered);
        Check.Equal(0L, final.Unavailable);
        Check.Equal(0, final.Backlog);
        Check.Null(final.ActiveType);
    }

    private static async Task ExerciseEntityUpdateCoalescingPumpAsync()
    {
        var firstDeliveryStarted = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirstDelivery = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var delivered = new List<ProtocolEnvelope>();
        var deliveredSync = new object();
        var deliveryCalls = 0;

        async ValueTask<bool> DeliverAsync(ProtocolEnvelope envelope)
        {
            if (Interlocked.Increment(ref deliveryCalls) == 1)
            {
                firstDeliveryStarted.TrySetResult(true);
                await releaseFirstDelivery.Task.ConfigureAwait(false);
            }

            lock (deliveredSync)
            {
                delivered.Add(envelope);
            }

            return true;
        }

        var pump = new NetworkBridgeDeliveryPump(
            DeliverAsync,
            criticalCapacity: 3,
            entityUpdateCapacity: 2);
        using var stop = new CancellationTokenSource();
        var run = pump.RunAsync(stop.Token);
        _ = pump.TryEnqueue(PumpEnvelope(MessageType.Command, 1));
        await firstDeliveryStarted.Task
            .WaitAsync(TimeSpan.FromSeconds(2))
            .ConfigureAwait(false);

        var firstEntity = NetEntityId.Create(70, 1);
        var secondEntity = NetEntityId.Create(70, 2);
        var overflowEntity = NetEntityId.Create(70, 3);
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Queued,
            pump.TryEnqueue(PumpEntityUpdate(firstEntity, 10)).Disposition);
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Coalesced,
            pump.TryEnqueue(PumpEntityUpdate(firstEntity, 11)).Disposition);
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Coalesced,
            pump.TryEnqueue(PumpEntityUpdate(firstEntity, 10)).Disposition);
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Queued,
            pump.TryEnqueue(PumpEntityUpdate(
                secondEntity,
                uint.MaxValue)).Disposition);
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Coalesced,
            pump.TryEnqueue(PumpEntityUpdate(secondEntity, 1)).Disposition);
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Coalesced,
            pump.TryEnqueue(PumpEntityUpdate(
                secondEntity,
                uint.MaxValue)).Disposition);
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Rejected,
            pump.TryEnqueue(PumpEntityUpdate(overflowEntity, 30)).Disposition);

        var blocked = pump.ReadSnapshot();
        Check.Equal(3, blocked.Backlog);
        Check.Equal(3, blocked.MaxBacklog);

        releaseFirstDelivery.TrySetResult(true);
        await WaitUntilAsync(
                () =>
                {
                    lock (deliveredSync)
                    {
                        return delivered.Count == 3;
                    }
                },
                TimeSpan.FromSeconds(2),
                "Entity-update lane did not drain its bounded backlog.")
            .ConfigureAwait(false);
        await stop.CancelAsync().ConfigureAwait(false);
        await run.WaitAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(false);

        ProtocolEnvelope[] observed;
        lock (deliveredSync)
        {
            observed = delivered.ToArray();
        }

        var entitySequences = observed
            .Where(static item => item.Type == MessageType.EntityUpdate)
            .Select(static item => item.Sequence)
            .ToArray();
        Check.Equal("11,1", string.Join(',', entitySequences));
        Check.False(observed.Any(static item => item.Sequence == 10));
        Check.False(observed.Any(static item =>
            item.Type == MessageType.EntityUpdate &&
            item.Sequence == uint.MaxValue));
        Check.False(observed.Any(static item => item.Sequence == 30));

        var final = pump.ReadSnapshot();
        Check.Equal(7L, final.Queued);
        Check.Equal(4L, final.Coalesced);
        Check.Equal(1L, final.Rejected);
        Check.Equal(3L, final.Delivered);
        Check.Equal(0, final.Backlog);
    }

    private static async Task ExerciseUncancelledPumpFrameAsync()
    {
        var deliveryStarted = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseDelivery = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);

        async ValueTask<bool> DeliverAsync(ProtocolEnvelope envelope)
        {
            _ = envelope;
            deliveryStarted.TrySetResult(true);
            await releaseDelivery.Task.ConfigureAwait(false);
            return true;
        }

        var pump = new NetworkBridgeDeliveryPump(DeliverAsync);
        using var stop = new CancellationTokenSource();
        var run = pump.RunAsync(stop.Token);
        _ = pump.TryEnqueue(PumpEnvelope(MessageType.Command, 1));
        await deliveryStarted.Task
            .WaitAsync(TimeSpan.FromSeconds(2))
            .ConfigureAwait(false);

        await stop.CancelAsync().ConfigureAwait(false);
        await Task.Delay(50).ConfigureAwait(false);
        Check.False(
            run.IsCompleted,
            "Cancellation interrupted an in-flight protocol-frame delivery.");

        releaseDelivery.TrySetResult(true);
        await run.WaitAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(false);
        Check.Equal(1L, pump.ReadSnapshot().Delivered);
    }

    private static async Task ExercisePlayerActionFifoPumpAsync()
    {
        var firstDeliveryStarted = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirstDelivery = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var delivered = new List<ProtocolEnvelope>();
        var deliveredSync = new object();
        var deliveryCalls = 0;

        async ValueTask<bool> DeliverAsync(ProtocolEnvelope envelope)
        {
            if (Interlocked.Increment(ref deliveryCalls) == 1)
            {
                firstDeliveryStarted.TrySetResult(true);
                await releaseFirstDelivery.Task.ConfigureAwait(false);
            }

            lock (deliveredSync)
            {
                delivered.Add(envelope);
            }
            return true;
        }

        var pump = new NetworkBridgeDeliveryPump(
            DeliverAsync,
            criticalCapacity: 3);
        using var stop = new CancellationTokenSource();
        var run = pump.RunAsync(stop.Token);
        _ = pump.TryEnqueue(PumpEnvelope(MessageType.Command, 1));
        await firstDeliveryStarted.Task
            .WaitAsync(TimeSpan.FromSeconds(2))
            .ConfigureAwait(false);

        Check.Equal(
            NetworkBridgeEnqueueDisposition.Queued,
            pump.TryEnqueue(
                PumpEnvelope(MessageType.PlayerAction, 2)).Disposition);
        Check.Equal(
            NetworkBridgeEnqueueDisposition.Queued,
            pump.TryEnqueue(
                PumpEnvelope(MessageType.PlayerAction, 3)).Disposition);
        Check.Equal(3, pump.ReadSnapshot().Backlog);

        releaseFirstDelivery.TrySetResult(true);
        await WaitUntilAsync(
                () =>
                {
                    lock (deliveredSync)
                    {
                        return delivered.Count == 3;
                    }
                },
                TimeSpan.FromSeconds(2),
                "Player-action transactions did not drain in FIFO order.")
            .ConfigureAwait(false);
        await stop.CancelAsync().ConfigureAwait(false);
        await run.WaitAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(false);

        ProtocolEnvelope[] observed;
        lock (deliveredSync)
        {
            observed = delivered.ToArray();
        }
        Check.Equal("1,2,3", string.Join(',', observed.Select(
            static envelope => envelope.Sequence)));
        Check.Equal(3L, pump.ReadSnapshot().Delivered);
        Check.Equal(0L, pump.ReadSnapshot().Coalesced);
    }

    private static ProtocolEnvelope PumpEnvelope(
        MessageType type,
        uint sequence) =>
        new(
            type,
            sequence,
            sequence,
            new[] { unchecked((byte)sequence) });

    private static ProtocolEnvelope PumpEntityUpdate(
        NetEntityId entityId,
        uint sequence)
    {
        var payload = new WorldEntityStatePayload(
            entityId,
            ModelHash: 0x12345678,
            WorldEntityKind.Ped,
            WorldEntityStateFlags.Human,
            WorldCombatTargetSlot.None,
            Position: new Vector3(sequence, 0f, 0f),
            Velocity: Vector3.Zero,
            Heading: 0f,
            HealthFraction: 1f,
            WeaponHash: 0,
            TaskKind: WorldTaskKind.Locomotion,
            ParentEntityId: NetEntityId.None,
            TaskTarget: new Vector3(sequence + 1, 0f, 0f));
        return new ProtocolEnvelope(
            MessageType.EntityUpdate,
            sequence,
            sequence,
            BinaryPayloadCodec.EncodeWorldEntityState(payload));
    }

    private static async Task IpcRoleNegotiationAsync()
    {
        Check.SequenceEqual(
            new byte[] { 0 },
            BridgeRolePayloadCodec.Encode(SessionRole.Host));
        Check.SequenceEqual(
            new byte[] { 1 },
            BridgeRolePayloadCodec.Encode(SessionRole.Guest));
        Check.Equal(
            SessionRole.Host,
            BridgeRolePayloadCodec.Decode(new byte[] { 0 }));
        Check.Equal(
            SessionRole.Guest,
            BridgeRolePayloadCodec.Decode(new byte[] { 1 }));
        Check.Throws<ProtocolException>(
            () => BridgeRolePayloadCodec.Encode((SessionRole)2));
        Check.Throws<ProtocolException>(
            () => BridgeRolePayloadCodec.Decode(ReadOnlySpan<byte>.Empty));
        Check.Throws<ProtocolException>(
            () => BridgeRolePayloadCodec.Decode(new byte[] { 0, 1 }));
        Check.Throws<ProtocolException>(
            () => BridgeRolePayloadCodec.Decode(new byte[] { 2 }));

        await ExerciseRoleNegotiationAsync(SessionRole.Host).ConfigureAwait(false);
        await ExerciseRoleNegotiationAsync(SessionRole.Guest).ConfigureAwait(false);
    }

    private static async Task BridgePipeStallRecoveryAsync()
    {
        await using var server = new BridgePipeServer(
            $"CoopStory.SelfTest.Stall.{Guid.NewGuid():N}");
        using var stop = new CancellationTokenSource();
        var firstOpened =
            new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
        var secondOpened =
            new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
        var openCount = 0;
        server.ConnectionOpened += () =>
        {
            var count = Interlocked.Increment(ref openCount);
            if (count == 1)
            {
                firstOpened.TrySetResult();
            }
            else if (count == 2)
            {
                secondOpened.TrySetResult();
            }
            return ValueTask.CompletedTask;
        };

        var runTask = server.RunAsync(stop.Token);
        Task<bool>? stalledSend = null;
        try
        {
            await using var firstClient = new NamedPipeClientStream(
                ".",
                server.PipeName,
                PipeDirection.InOut,
                PipeOptions.Asynchronous);
            await firstClient.ConnectAsync(2_000, stop.Token)
                .ConfigureAwait(false);
            await firstOpened.Task.WaitAsync(TimeSpan.FromSeconds(2))
                .ConfigureAwait(false);

            stalledSend = server.SendAsync(
                    new ProtocolEnvelope(
                        MessageType.ResyncSnapshot,
                        1,
                        1,
                        new byte[256 * 1024]))
                .AsTask();
            long abortedGeneration = 0;
            long activeMilliseconds = 0;
            await WaitUntilAsync(
                    () => server.AbortStalledSend(
                        25,
                        out abortedGeneration,
                        out activeMilliseconds),
                    TimeSpan.FromSeconds(2),
                    "A full game pipe did not enter the recoverable stalled state.")
                .ConfigureAwait(false);
            Check.True(abortedGeneration > 0);
            Check.True(activeMilliseconds >= 25);

            await using var secondClient = new NamedPipeClientStream(
                ".",
                server.PipeName,
                PipeDirection.InOut,
                PipeOptions.Asynchronous);
            await secondClient.ConnectAsync(2_000, stop.Token)
                .ConfigureAwait(false);
            await secondOpened.Task.WaitAsync(TimeSpan.FromSeconds(2))
                .ConfigureAwait(false);

            Check.False(
                server.AbortStalledSend(
                    1,
                    out _,
                    out _),
                "A reconnect must not inherit the previous pipe generation's stall.");
            Check.False(await stalledSend.ConfigureAwait(false));

            var heartbeat = new ProtocolEnvelope(
                MessageType.Heartbeat,
                2,
                2,
                ReadOnlyMemory<byte>.Empty);
            var readTask =
                ProtocolCodec.ReadAsync(secondClient).AsTask();
            Check.True(
                await server.SendAsync(heartbeat).ConfigureAwait(false));
            var received = await readTask.WaitAsync(TimeSpan.FromSeconds(2))
                .ConfigureAwait(false);
            Check.NotNull(received);
            Check.Equal(heartbeat.Type, received!.Type);
            Check.Equal(heartbeat.Sequence, received.Sequence);
        }
        finally
        {
            await server.StopAsync().ConfigureAwait(false);
            await stop.CancelAsync().ConfigureAwait(false);
            await IgnoreCancellationAsync(
                    runTask,
                    stalledSend)
                .ConfigureAwait(false);
        }
    }

    private static async Task BridgePipeGenerationBoundAsync()
    {
        await using var server = new BridgePipeServer(
            $"CoopStory.SelfTest.Generation.{Guid.NewGuid():N}");
        using var stop = new CancellationTokenSource();
        var firstOpened = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var secondOpened = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var openCount = 0;
        server.ConnectionOpened += () =>
        {
            if (Interlocked.Increment(ref openCount) == 1)
            {
                firstOpened.TrySetResult();
            }
            else
            {
                secondOpened.TrySetResult();
            }
            return ValueTask.CompletedTask;
        };

        Check.False(server.TryCaptureConnection(out var disconnectedToken));
        Check.False(disconnectedToken.IsValid);
        Check.False(server.IsConnectionCurrent(default));

        var runTask = server.RunAsync(stop.Token);
        try
        {
            await using var firstClient = new NamedPipeClientStream(
                ".",
                server.PipeName,
                PipeDirection.InOut,
                PipeOptions.Asynchronous);
            await firstClient.ConnectAsync(2_000, stop.Token)
                .ConfigureAwait(false);
            await firstOpened.Task.WaitAsync(TimeSpan.FromSeconds(2))
                .ConfigureAwait(false);

            Check.True(server.TryCaptureConnection(out var firstToken));
            Check.True(firstToken.IsValid);
            Check.True(server.IsConnectionCurrent(firstToken));

            var rotatedToken = await server.RotateConnectionTokenAsync(
                    firstToken)
                .ConfigureAwait(false);
            Check.True(rotatedToken.HasValue);
            Check.True(rotatedToken.GetValueOrDefault().IsValid);
            Check.False(firstToken == rotatedToken.GetValueOrDefault());
            Check.False(server.IsConnectionCurrent(firstToken));
            Check.True(server.IsConnectionCurrent(rotatedToken.GetValueOrDefault()));
            var rotatedFrame = new ProtocolEnvelope(
                MessageType.Heartbeat,
                40,
                40,
                ReadOnlyMemory<byte>.Empty);
            Check.False(
                await server.SendAsync(firstToken, rotatedFrame)
                    .ConfigureAwait(false));
            var firstRead = ProtocolCodec.ReadAsync(firstClient).AsTask();
            Check.True(
                await server.SendAsync(rotatedToken.GetValueOrDefault(), rotatedFrame)
                    .ConfigureAwait(false));
            var firstReceived = await firstRead
                .WaitAsync(TimeSpan.FromSeconds(2))
                .ConfigureAwait(false);
            Check.NotNull(firstReceived);
            Check.Equal(rotatedFrame.Sequence, firstReceived!.Sequence);

            Check.True(server.AbortCurrentConnection());

            await using var secondClient = new NamedPipeClientStream(
                ".",
                server.PipeName,
                PipeDirection.InOut,
                PipeOptions.Asynchronous);
            await secondClient.ConnectAsync(2_000, stop.Token)
                .ConfigureAwait(false);
            await secondOpened.Task.WaitAsync(TimeSpan.FromSeconds(2))
                .ConfigureAwait(false);

            Check.False(server.IsConnectionCurrent(firstToken));
            Check.True(server.TryCaptureConnection(out var secondToken));
            Check.True(secondToken.IsValid);
            Check.True(server.IsConnectionCurrent(secondToken));
            Check.False(firstToken == secondToken);
            Check.False(rotatedToken.GetValueOrDefault() == secondToken);
            Check.False(
                (await server.RotateConnectionTokenAsync(
                        rotatedToken.GetValueOrDefault())
                    .ConfigureAwait(false)).HasValue,
                "A stale logical token rotated its replacement pipe.");
            Check.True(server.IsConnectionCurrent(secondToken));

            var stale = new ProtocolEnvelope(
                MessageType.Heartbeat,
                41,
                41,
                ReadOnlyMemory<byte>.Empty);
            var current = new ProtocolEnvelope(
                MessageType.Heartbeat,
                42,
                42,
                ReadOnlyMemory<byte>.Empty);
            var readTask = ProtocolCodec.ReadAsync(secondClient).AsTask();

            Check.False(
                await server.SendAsync(firstToken, stale)
                    .ConfigureAwait(false),
                "A send for the disconnected pipe generation targeted its replacement.");
            Check.False(
                await server.SendAsync(rotatedToken.GetValueOrDefault(), stale)
                    .ConfigureAwait(false),
                "A logically rotated token targeted a later physical pipe.");
            Check.True(
                await server.SendAsync(secondToken, current)
                    .ConfigureAwait(false));

            var received = await readTask.WaitAsync(TimeSpan.FromSeconds(2))
                .ConfigureAwait(false);
            Check.NotNull(received);
            Check.Equal(current.Type, received!.Type);
            Check.Equal(current.Sequence, received.Sequence);
            Check.Equal(current.Tick, received.Tick);
        }
        finally
        {
            await server.StopAsync().ConfigureAwait(false);
            await stop.CancelAsync().ConfigureAwait(false);
            await IgnoreCancellationAsync(runTask).ConfigureAwait(false);
        }
    }

    private static async Task BridgeInboundGenerationBoundaryAsync()
    {
        await using var server = new BridgePipeServer(
            $"CoopStory.SelfTest.InboundGeneration.{Guid.NewGuid():N}");
        using var generationGate = new BridgeSessionGenerationGate();
        using var stop = new CancellationTokenSource();
        var opened = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        server.ConnectionOpened += () =>
        {
            opened.TrySetResult();
            return ValueTask.CompletedTask;
        };

        var controlSync = new object();
        (TaskCompletionSource<BridgePipeConnectionToken> Entered,
            TaskCompletionSource<bool> Release,
            TaskCompletionSource<bool> Done)? activeControl = null;
        var stateSync = new object();
        ProtocolEnvelope? mission = null;
        ProtocolEnvelope? animScene = null;
        ProtocolEnvelope? player = null;
        var forwarded = new List<MessageType>();
        var rejected = 0;
        long readyGeneration = 0;

        void ClearSimulatedSessionState()
        {
            lock (stateSync)
            {
                mission = null;
                animScene = null;
                player = null;
                forwarded.Clear();
            }
        }

        server.MessageReceived += async (
            envelope,
            receiveConnection,
            cancellationToken) =>
        {
            (TaskCompletionSource<BridgePipeConnectionToken> Entered,
                TaskCompletionSource<bool> Release,
                TaskCompletionSource<bool> Done) control;
            lock (controlSync)
            {
                control = activeControl ??
                    throw new SelfTestException(
                        "Inbound bridge frame arrived without test control.");
            }

            control.Entered.TrySetResult(receiveConnection);
            await control.Release.Task.WaitAsync(cancellationToken)
                .ConfigureAwait(false);
            await using var inbound =
                await generationGate.TryEnterInboundAsync(
                        receiveConnection,
                        connection =>
                            Volatile.Read(ref readyGeneration) ==
                                connection.Generation &&
                            server.IsConnectionCurrent(connection),
                        cancellationToken)
                    .ConfigureAwait(false);
            if (inbound is null)
            {
                _ = Interlocked.Increment(ref rejected);
                control.Done.TrySetResult(true);
                return;
            }

            lock (stateSync)
            {
                switch (envelope.Type)
                {
                    case MessageType.MissionState:
                        mission = envelope;
                        break;
                    case MessageType.AnimSceneDefinition:
                        animScene = envelope;
                        break;
                    case MessageType.PlayerState:
                        player = envelope;
                        break;
                    default:
                        throw new SelfTestException(
                            $"Unexpected inbound generation test type {envelope.Type}.");
                }
                forwarded.Add(envelope.Type);
            }
            control.Done.TrySetResult(true);
        };

        var runTask = server.RunAsync(stop.Token);
        try
        {
            await using var client = new NamedPipeClientStream(
                ".",
                server.PipeName,
                PipeDirection.InOut,
                PipeOptions.Asynchronous);
            await client.ConnectAsync(2_000, stop.Token)
                .ConfigureAwait(false);
            await opened.Task.WaitAsync(TimeSpan.FromSeconds(2))
                .ConfigureAwait(false);
            Check.True(server.TryCaptureConnection(out var currentConnection));
            Volatile.Write(
                ref readyGeneration,
                currentConnection.Generation);

            var protectedTypes = new[]
            {
                MessageType.MissionState,
                MessageType.AnimSceneDefinition,
                MessageType.PlayerState
            };
            uint sequence = 100;
            foreach (var type in protectedTypes)
            {
                var entered = new TaskCompletionSource<
                    BridgePipeConnectionToken>(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                var release = new TaskCompletionSource<bool>(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                var done = new TaskCompletionSource<bool>(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                lock (controlSync)
                {
                    activeControl = (entered, release, done);
                }

                await ProtocolCodec.WriteAsync(
                        client,
                        new ProtocolEnvelope(
                            type,
                            sequence++,
                            unchecked((ulong)Environment.TickCount64),
                            ReadOnlyMemory<byte>.Empty),
                        stop.Token)
                    .ConfigureAwait(false);
                var receiveConnection = await entered.Task
                    .WaitAsync(TimeSpan.FromSeconds(2))
                    .ConfigureAwait(false);
                Check.Equal(currentConnection, receiveConnection);

                await using (await generationGate.EnterBoundaryAsync(stop.Token)
                                 .ConfigureAwait(false))
                {
                    Volatile.Write(ref readyGeneration, 0);
                    var rotated = await server.RotateConnectionTokenAsync(
                            currentConnection,
                            stop.Token)
                        .ConfigureAwait(false);
                    Check.True(rotated.HasValue);
                    currentConnection = rotated.GetValueOrDefault();
                    ClearSimulatedSessionState();
                    Volatile.Write(
                        ref readyGeneration,
                        currentConnection.Generation);
                }

                release.TrySetResult(true);
                await done.Task.WaitAsync(TimeSpan.FromSeconds(2))
                    .ConfigureAwait(false);
                lock (controlSync)
                {
                    activeControl = null;
                }
                lock (stateSync)
                {
                    Check.Null(mission);
                    Check.Null(animScene);
                    Check.Null(player);
                    Check.Equal(0, forwarded.Count);
                }
            }
            Check.Equal(protectedTypes.Length, Volatile.Read(ref rejected));

            foreach (var type in protectedTypes)
            {
                var entered = new TaskCompletionSource<
                    BridgePipeConnectionToken>(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                var release = new TaskCompletionSource<bool>(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                var done = new TaskCompletionSource<bool>(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                release.TrySetResult(true);
                lock (controlSync)
                {
                    activeControl = (entered, release, done);
                }

                await ProtocolCodec.WriteAsync(
                        client,
                        new ProtocolEnvelope(
                            type,
                            sequence++,
                            unchecked((ulong)Environment.TickCount64),
                            ReadOnlyMemory<byte>.Empty),
                        stop.Token)
                    .ConfigureAwait(false);
                Check.Equal(
                    currentConnection,
                    await entered.Task.WaitAsync(TimeSpan.FromSeconds(2))
                        .ConfigureAwait(false));
                await done.Task.WaitAsync(TimeSpan.FromSeconds(2))
                    .ConfigureAwait(false);
                lock (controlSync)
                {
                    activeControl = null;
                }
            }

            lock (stateSync)
            {
                Check.NotNull(mission);
                Check.NotNull(animScene);
                Check.NotNull(player);
                Check.Equal(
                    string.Join(',', protectedTypes),
                    string.Join(',', forwarded));
            }
        }
        finally
        {
            await server.StopAsync().ConfigureAwait(false);
            await stop.CancelAsync().ConfigureAwait(false);
            await IgnoreCancellationAsync(runTask).ConfigureAwait(false);
        }
    }

    private static async Task PlayerIdentityRelayAsync()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "CoopStory.SelfTest",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);

        JsonLineLogger? hostLogger = null;
        JsonLineLogger? guestLogger = null;
        SidecarRuntime? hostRuntime = null;
        SidecarRuntime? guestRuntime = null;
        CancellationTokenSource? stop = null;
        Task? hostTask = null;
        Task? guestTask = null;
        try
        {
            var credentials = SessionCredentials.Generate();
            var tcpPort = FindFreeTcpPort();
            var udpPort = FindFreeUdpPort();
            var network = new NetworkConfig
            {
                HeartbeatIntervalMs = 100,
                HeartbeatTimeoutMs = 1_000,
                ReconnectMinMs = 100,
                ReconnectMaxMs = 400,
                DiagnosticsIntervalMs = 250
            };
            var hostConfig = new SidecarConfig
            {
                Role = SessionRole.Host,
                Nickname = "Arthur",
                HostAddress = "127.0.0.1",
                TcpPort = tcpPort,
                UdpPort = udpPort,
                PipeName = $"CoopStory.SelfTest.Identity.Host.{Guid.NewGuid():N}",
                SessionToken = credentials.ExportToken(),
                ProfilePath = Path.Combine(root, "host-profile.json"),
                LogPath = Path.Combine(root, "host.jsonl"),
                Network = network
            };
            var guestConfig = hostConfig with
            {
                Role = SessionRole.Guest,
                Nickname = "Sadie",
                PipeName = $"CoopStory.SelfTest.Identity.Guest.{Guid.NewGuid():N}",
                ProfilePath = Path.Combine(root, "guest-profile.json"),
                LogPath = Path.Combine(root, "guest.jsonl")
            };

            hostLogger = new JsonLineLogger(hostConfig.LogPath);
            guestLogger = new JsonLineLogger(guestConfig.LogPath);
            hostRuntime = new SidecarRuntime(
                hostConfig,
                credentials,
                hostLogger,
                IPAddress.Loopback);
            guestRuntime = new SidecarRuntime(
                guestConfig,
                credentials,
                guestLogger);
            stop = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            hostTask = hostRuntime.RunAsync(stop.Token);
            guestTask = guestRuntime.RunAsync(stop.Token);

            await using var hostPipe = new NamedPipeClientStream(
                ".",
                PipeNameResolver.ResolveForCurrentUser(hostConfig.PipeName),
                PipeDirection.InOut,
                PipeOptions.Asynchronous);
            await using var guestPipe = new NamedPipeClientStream(
                ".",
                PipeNameResolver.ResolveForCurrentUser(guestConfig.PipeName),
                PipeDirection.InOut,
                PipeOptions.Asynchronous);
            await Task.WhenAll(
                hostPipe.ConnectAsync(3_000, CancellationToken.None),
                guestPipe.ConnectAsync(3_000, CancellationToken.None))
                .ConfigureAwait(false);

            await WaitUntilAsync(
                    () => LogContains(
                              hostConfig.LogPath,
                              "\"event\":\"network.motion-mode.negotiated\"") &&
                          LogContains(
                              guestConfig.LogPath,
                              "\"event\":\"network.motion-mode.negotiated\""),
                    TimeSpan.FromSeconds(3),
                    "Sidecars did not finish peer motion-mode negotiation before the identity samples.")
                .ConfigureAwait(false);

            var hostEntity = NetEntityId.Create(50, 1);
            var guestEntity = NetEntityId.Create(51, 1);
            await SendBridgeHelloAsync(hostPipe).ConfigureAwait(false);
            await SendBridgeHelloAsync(guestPipe).ConfigureAwait(false);
            await ExpectBridgeRoleAcknowledgementAsync(
                    hostPipe,
                    SessionRole.Host)
                .ConfigureAwait(false);
            await SendBridgePlayerStateAsync(
                hostPipe,
                hostEntity,
                SessionRole.Host).ConfigureAwait(false);
            await ExpectBridgeRoleAcknowledgementAsync(
                    guestPipe,
                    SessionRole.Guest)
                .ConfigureAwait(false);
            await SendBridgePlayerStateAsync(
                guestPipe,
                guestEntity,
                SessionRole.Guest).ConfigureAwait(false);
            // The bridge writes its first PlayerState as soon as it consumes
            // HelloAck, before the client reads MotionReplicationConfig. The
            // serial pipe receive loop must buffer, not drop, both samples until
            // the Ack -> MotionConfig -> ready transaction completes.
            await ExpectBridgeMotionConfigAsync(hostPipe)
                .ConfigureAwait(false);
            await ExpectBridgeMotionConfigAsync(guestPipe)
                .ConfigureAwait(false);

            var hostSawGuest = WaitForPlayerIdentityAsync(
                hostPipe,
                new PlayerIdentityPayload(
                    guestEntity,
                    (byte)SessionRole.Guest,
                    guestConfig.Nickname),
                stop.Token);
            var guestSawHost = WaitForPlayerIdentityAsync(
                guestPipe,
                new PlayerIdentityPayload(
                    hostEntity,
                    (byte)SessionRole.Host,
                    hostConfig.Nickname),
                stop.Token);
            await Task.WhenAll(hostSawGuest, guestSawHost)
                .WaitAsync(TimeSpan.FromSeconds(5))
                .ConfigureAwait(false);

            Check.True(LogContains(
                hostConfig.LogPath,
                "\"event\":\"network.identity.delivered\""));
            Check.True(LogContains(
                guestConfig.LogPath,
                "\"event\":\"network.identity.delivered\""));
        }
        finally
        {
            if (stop is not null)
            {
                await stop.CancelAsync().ConfigureAwait(false);
            }
            await IgnoreCancellationAsync(hostTask, guestTask)
                .ConfigureAwait(false);
            if (hostRuntime is not null)
            {
                await hostRuntime.DisposeAsync().ConfigureAwait(false);
            }
            if (guestRuntime is not null)
            {
                await guestRuntime.DisposeAsync().ConfigureAwait(false);
            }
            stop?.Dispose();
            if (hostLogger is not null)
            {
                await hostLogger.DisposeAsync().ConfigureAwait(false);
            }
            if (guestLogger is not null)
            {
                await guestLogger.DisposeAsync().ConfigureAwait(false);
            }
            DeleteOwnedSelfTestRoot(root);
        }
    }

    private static async Task GuestReconnectResyncGateAsync()
    {
        var gate = new GuestReconnectResyncGate();
        var localReset = new ProtocolEnvelope(
            MessageType.ResyncRequest,
            Sequence: 500,
            Tick: 5_000,
            ReadOnlyMemory<byte>.Empty);
        Check.Equal(
            GuestReconnectResyncDeferDisposition.Stored,
            gate.Defer(localReset));
        Check.Equal(
            GuestReconnectResyncDeferDisposition.Duplicate,
            gate.Defer(localReset with
            {
                Sequence = 501,
                Tick = 5_001
            }));

        var reconnectGuardedTypes = new[]
        {
            MessageType.PlayerState,
            MessageType.PlayerAction,
            MessageType.InteractionIntent
        };
        foreach (var messageType in reconnectGuardedTypes)
        {
            Check.True(
                SidecarRuntime.ShouldDeferOutboundForSessionBoundary(
                    SessionRole.Guest,
                    messageType,
                    networkConnected: true,
                    motionModeNegotiated: true,
                    guestReconnectResetPending: gate.HasPendingRequest),
                $"Guest {messageType} crossed a pending reconnect reset.");
        }

        var staleGraph = new AuthoritativeWorldGraphRegistry();
        var staleEntity = new WorldEntityStatePayload(
            NetEntityId.Create(88, 1),
            ModelHash: 0x10203040,
            WorldEntityKind.Ped,
            WorldEntityStateFlags.Human |
                WorldEntityStateFlags.ScriptOwned,
            WorldCombatTargetSlot.None,
            new Vector3(1f, 2f, 3f),
            Vector3.Zero,
            Heading: 45f,
            HealthFraction: 1f,
            WeaponHash: 0,
            WorldTaskKind.Locomotion);
        Check.Equal(
            WorldGraphApplyDisposition.Applied,
            staleGraph.Apply(new ProtocolEnvelope(
                MessageType.EntitySpawn,
                Sequence: 510,
                Tick: 5_100,
                BinaryPayloadCodec.EncodeWorldEntityState(staleEntity))));

        var definition = CreateCanonicalAnimSceneDefinition(
            definitionRevision: 10,
            sceneFlags: 0);
        var mission = new MissionStatePayload(
            definition.HostEntityId,
            definition.MissionEpoch,
            Revision: 10,
            CheckpointGeneration: 7,
            MissionPhase.Cutscene,
            MissionStateFlags.MissionActive,
            HostAnchor: Vector3.Zero,
            HostHeading: 0f);
        var cinematic = new MissionCinematicStatePayload(
            definition.HostEntityId,
            definition.MissionEpoch,
            definition.CinematicGeneration,
            Revision: 10,
            CheckpointGeneration: mission.CheckpointGeneration,
            MissionCinematicPhase.Playing,
            MissionCinematicStateFlags.CameraExpected,
            ResumeAnchor: Vector3.Zero,
            ResumeHeading: 0f);
        var missionCache = new AuthoritativeMissionStateCache();
        var cinematicCache =
            new AuthoritativeMissionCinematicStateCache();
        var definitionCache =
            new AuthoritativeAnimSceneDefinitionCache();
        _ = missionCache.Apply(new ProtocolEnvelope(
            MessageType.MissionState,
            Sequence: 511,
            Tick: 5_110,
            BinaryPayloadCodec.EncodeMissionState(mission)));
        _ = cinematicCache.Apply(new ProtocolEnvelope(
            MessageType.MissionCinematicState,
            Sequence: 512,
            Tick: 5_120,
            BinaryPayloadCodec.EncodeMissionCinematicState(cinematic)));
        _ = definitionCache.Apply(new ProtocolEnvelope(
            MessageType.AnimSceneDefinition,
            Sequence: 513,
            Tick: 5_130,
            BinaryPayloadCodec.EncodeAnimSceneDefinition(definition)));

        var operations = new List<string>();
        ProtocolEnvelope? bridgeReset = null;
        var replay = await gate.ReplayOnceAsync(
            () =>
            {
                operations.Add("clear");
                staleGraph.Clear();
                missionCache.Clear();
                cinematicCache.Clear();
                definitionCache.Clear();
            },
            (envelope, _) =>
            {
                operations.Add("bridge");
                bridgeReset = envelope;
                return ValueTask.FromResult(true);
            },
            _ =>
            {
                operations.Add("peer");
                return ValueTask.FromResult(true);
            });
        Check.Equal(
            GuestReconnectResyncReplayDisposition.Completed,
            replay.Disposition);
        Check.True(replay.BridgeDelivered);
        Check.True(replay.PeerRequested);
        Check.Equal("clear,bridge,peer", string.Join(',', operations));
        Check.Equal(0, staleGraph.CaptureSpawnSnapshot().Count);
        Check.Null(missionCache.Capture());
        Check.Null(cinematicCache.Capture());
        Check.Null(definitionCache.Capture());
        Check.True(bridgeReset?.Type == MessageType.ResyncRequest);
        Check.True(bridgeReset?.Payload.IsEmpty == true);
        Check.True(bridgeReset?.Sequence == 500);
        foreach (var messageType in reconnectGuardedTypes)
        {
            Check.False(
                SidecarRuntime.ShouldDeferOutboundForSessionBoundary(
                    SessionRole.Guest,
                    messageType,
                    networkConnected: true,
                    motionModeNegotiated: true,
                    guestReconnectResetPending: gate.HasPendingRequest),
                $"Guest {messageType} remained blocked after reconnect completion.");
        }

        var secondReplay = await gate.ReplayOnceAsync(
            () => operations.Add("unexpected-clear"),
            (_, _) =>
            {
                operations.Add("unexpected-bridge");
                return ValueTask.FromResult(true);
            },
            _ =>
            {
                operations.Add("unexpected-peer");
                return ValueTask.FromResult(true);
            });
        Check.Equal(
            GuestReconnectResyncReplayDisposition.NoPendingRequest,
            secondReplay.Disposition);
        Check.Equal("clear,bridge,peer", string.Join(',', operations));

        Check.Throws<ProtocolException>(() => gate.Defer(localReset with
        {
            Payload = new byte[] { 1 }
        }));
        Check.Throws<ProtocolException>(() => gate.Defer(localReset with
        {
            Version = (ushort)(ProtocolConstants.Version - 1)
        }));

        var rejectedGate = new GuestReconnectResyncGate();
        _ = rejectedGate.Defer(localReset with { Sequence = 520 });
        var peerCalledAfterRejection = false;
        var rejectedOperations = new List<string>();
        var rejected = await rejectedGate.ReplayOnceAsync(
            () => rejectedOperations.Add("clear"),
            (_, _) =>
            {
                rejectedOperations.Add("bridge-rejected");
                return ValueTask.FromResult(false);
            },
            _ =>
            {
                peerCalledAfterRejection = true;
                return ValueTask.FromResult(true);
            });
        Check.Equal(
            GuestReconnectResyncReplayDisposition.BridgeDeliveryFailed,
            rejected.Disposition);
        Check.False(rejected.BridgeDelivered);
        Check.False(rejected.PeerRequested);
        Check.False(peerCalledAfterRejection);
        Check.Equal(
            "clear,bridge-rejected",
            string.Join(',', rejectedOperations));
        Check.True(rejectedGate.HasPendingRequest);
        var retried = await rejectedGate.ReplayOnceAsync(
            () => rejectedOperations.Add("retry-clear"),
            (_, _) =>
            {
                rejectedOperations.Add("retry-bridge");
                return ValueTask.FromResult(true);
            },
            _ =>
            {
                rejectedOperations.Add("retry-peer");
                return ValueTask.FromResult(true);
            });
        Check.Equal(
            GuestReconnectResyncReplayDisposition.Completed,
            retried.Disposition);
        Check.False(rejectedGate.HasPendingRequest);

        var peerFailureGate = new GuestReconnectResyncGate();
        _ = peerFailureGate.Defer(localReset with { Sequence = 530 });
        var peerAttempts = 0;
        var peerFailure = await peerFailureGate.ReplayOnceAsync(
            static () => { },
            static (_, _) => ValueTask.FromResult(true),
            _ => ValueTask.FromResult(
                Interlocked.Increment(ref peerAttempts) > 1));
        Check.Equal(
            GuestReconnectResyncReplayDisposition.PeerRequestFailed,
            peerFailure.Disposition);
        Check.True(peerFailure.BridgeDelivered);
        Check.True(peerFailureGate.HasPendingRequest);
        var peerRetry = await peerFailureGate.ReplayOnceAsync(
            static () => { },
            static (_, _) => ValueTask.FromResult(true),
            _ => ValueTask.FromResult(
                Interlocked.Increment(ref peerAttempts) > 1));
        Check.Equal(
            GuestReconnectResyncReplayDisposition.Completed,
            peerRetry.Disposition);
        Check.Equal(2, peerAttempts);

        var concurrentGate = new GuestReconnectResyncGate();
        _ = concurrentGate.Defer(localReset with { Sequence = 540 });
        var deliveryEntered = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirstDelivery = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var deliveryAttempts = 0;
        async ValueTask<bool> DeliverConcurrentAsync(
            ProtocolEnvelope _,
            CancellationToken token)
        {
            var attempt = Interlocked.Increment(ref deliveryAttempts);
            if (attempt == 1)
            {
                deliveryEntered.TrySetResult(true);
                await releaseFirstDelivery.Task.WaitAsync(token)
                    .ConfigureAwait(false);
                return false;
            }
            return true;
        }
        var firstConcurrent = concurrentGate.ReplayOnceAsync(
            static () => { },
            DeliverConcurrentAsync,
            static _ => ValueTask.FromResult(true)).AsTask();
        await deliveryEntered.Task.WaitAsync(TimeSpan.FromSeconds(2))
            .ConfigureAwait(false);
        var secondConcurrent = concurrentGate.ReplayOnceAsync(
            static () => { },
            DeliverConcurrentAsync,
            static _ => ValueTask.FromResult(true)).AsTask();
        await Task.Yield();
        Check.False(secondConcurrent.IsCompleted);
        releaseFirstDelivery.TrySetResult(true);
        Check.Equal(
            GuestReconnectResyncReplayDisposition.BridgeDeliveryFailed,
            (await firstConcurrent.ConfigureAwait(false)).Disposition);
        Check.Equal(
            GuestReconnectResyncReplayDisposition.Completed,
            (await secondConcurrent.ConfigureAwait(false)).Disposition);
        Check.Equal(2, deliveryAttempts);
        Check.False(concurrentGate.HasPendingRequest);

        var clearedGate = new GuestReconnectResyncGate();
        _ = clearedGate.Defer(localReset with { Sequence = 550 });
        var clearDeliveryEntered = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var neverDeliver = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var peerCalledAfterClear = false;
        var clearedReplay = clearedGate.ReplayOnceAsync(
            static () => { },
            async (_, token) =>
            {
                clearDeliveryEntered.TrySetResult(true);
                return await neverDeliver.Task.WaitAsync(token)
                    .ConfigureAwait(false);
            },
            _ =>
            {
                peerCalledAfterClear = true;
                return ValueTask.FromResult(true);
            }).AsTask();
        await clearDeliveryEntered.Task.WaitAsync(TimeSpan.FromSeconds(2))
            .ConfigureAwait(false);
        clearedGate.Clear();
        Check.Equal(
            GuestReconnectResyncReplayDisposition.Invalidated,
            (await clearedReplay.ConfigureAwait(false)).Disposition);
        Check.False(peerCalledAfterClear);
        Check.False(clearedGate.HasPendingRequest);
    }

    private static async Task MotionModeMismatchStopsRuntimeAsync()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "CoopStory.SelfTest",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);

        JsonLineLogger? hostLogger = null;
        JsonLineLogger? guestLogger = null;
        SidecarRuntime? hostRuntime = null;
        SidecarRuntime? guestRuntime = null;
        CancellationTokenSource? stop = null;
        Task? hostTask = null;
        Task? guestTask = null;
        try
        {
            var credentials = SessionCredentials.Generate();
            var tcpPort = FindFreeTcpPort();
            var udpPort = FindFreeUdpPort();
            var network = new NetworkConfig
            {
                HeartbeatIntervalMs = 100,
                HeartbeatTimeoutMs = 1_000,
                ReconnectMinMs = 100,
                ReconnectMaxMs = 400,
                DiagnosticsIntervalMs = 250
            };
            var hostConfig = new SidecarConfig
            {
                Role = SessionRole.Host,
                HostAddress = "127.0.0.1",
                TcpPort = tcpPort,
                UdpPort = udpPort,
                PipeName =
                    $"CoopStory.SelfTest.MotionMode.Host.{Guid.NewGuid():N}",
                SessionToken = credentials.ExportToken(),
                MotionReplicationMode = MotionReplicationMode.TaskNavmesh,
                ProfilePath = Path.Combine(root, "host-profile.json"),
                LogPath = Path.Combine(root, "host.jsonl"),
                Network = network
            };
            var guestConfig = hostConfig with
            {
                Role = SessionRole.Guest,
                PipeName =
                    $"CoopStory.SelfTest.MotionMode.Guest.{Guid.NewGuid():N}",
                MotionReplicationMode = MotionReplicationMode.AnimGraphReplica,
                ProfilePath = Path.Combine(root, "guest-profile.json"),
                LogPath = Path.Combine(root, "guest.jsonl")
            };

            hostLogger = new JsonLineLogger(hostConfig.LogPath);
            guestLogger = new JsonLineLogger(guestConfig.LogPath);
            hostRuntime = new SidecarRuntime(
                hostConfig,
                credentials,
                hostLogger,
                IPAddress.Loopback);
            guestRuntime = new SidecarRuntime(
                guestConfig,
                credentials,
                guestLogger);
            stop = new CancellationTokenSource(TimeSpan.FromSeconds(8));
            hostTask = hostRuntime.RunAsync(stop.Token);
            guestTask = guestRuntime.RunAsync(stop.Token);

            var hostFailureTask = ExpectMotionModeMismatchAsync(hostTask);
            var guestFailureTask = ExpectMotionModeMismatchAsync(guestTask);
            await Task.WhenAll(hostFailureTask, guestFailureTask)
                .WaitAsync(TimeSpan.FromSeconds(5))
                .ConfigureAwait(false);
            var hostFailure = await hostFailureTask.ConfigureAwait(false);
            var guestFailure = await guestFailureTask.ConfigureAwait(false);
            hostTask = null;
            guestTask = null;

            Check.Equal(
                MotionReplicationWireMode.TaskNavmesh,
                hostFailure.LocalMode);
            Check.True(
                hostFailure.PeerMode ==
                    MotionReplicationWireMode.AnimGraphReplica);
            Check.Equal(
                MotionReplicationWireMode.AnimGraphReplica,
                guestFailure.LocalMode);
            Check.True(
                guestFailure.PeerMode ==
                    MotionReplicationWireMode.TaskNavmesh);
            Check.True(LogContains(
                hostConfig.LogPath,
                "\"event\":\"network.motion-mode.negotiation-failed\""));
            Check.True(LogContains(
                guestConfig.LogPath,
                "\"event\":\"network.motion-mode.negotiation-failed\""));
        }
        finally
        {
            if (stop is not null)
            {
                await stop.CancelAsync().ConfigureAwait(false);
            }
            await IgnoreCancellationAsync(hostTask, guestTask)
                .ConfigureAwait(false);
            if (hostRuntime is not null)
            {
                await hostRuntime.DisposeAsync().ConfigureAwait(false);
            }
            if (guestRuntime is not null)
            {
                await guestRuntime.DisposeAsync().ConfigureAwait(false);
            }
            stop?.Dispose();
            if (hostLogger is not null)
            {
                await hostLogger.DisposeAsync().ConfigureAwait(false);
            }
            if (guestLogger is not null)
            {
                await guestLogger.DisposeAsync().ConfigureAwait(false);
            }
            DeleteOwnedSelfTestRoot(root);
        }
    }

    private static async Task<MotionReplicationModeMismatchException>
        ExpectMotionModeMismatchAsync(Task runtimeTask)
    {
        try
        {
            await runtimeTask.ConfigureAwait(false);
        }
        catch (MotionReplicationModeMismatchException exception)
        {
            return exception;
        }

        throw new SelfTestException(
            "Mismatched sidecar runtime completed without a motion-mode failure.");
    }

    private static async Task SendBridgeHelloAsync(Stream pipe)
    {
        await ProtocolCodec.WriteAsync(
            pipe,
            new ProtocolEnvelope(
                MessageType.Hello,
                1,
                unchecked((ulong)Environment.TickCount64),
                ReadOnlyMemory<byte>.Empty)).ConfigureAwait(false);
    }

    private static async Task ExpectBridgeRoleAsync(
        Stream pipe,
        SessionRole expected)
    {
        await ExpectBridgeRoleAcknowledgementAsync(pipe, expected)
            .ConfigureAwait(false);
        await ExpectBridgeMotionConfigAsync(pipe).ConfigureAwait(false);
    }

    private static async Task ExpectBridgeRoleAcknowledgementAsync(
        Stream pipe,
        SessionRole expected)
    {
        var acknowledgement = await ProtocolCodec.ReadAsync(pipe)
            .AsTask()
            .WaitAsync(TimeSpan.FromSeconds(2))
            .ConfigureAwait(false);
        Check.NotNull(acknowledgement);
        Check.Equal(MessageType.HelloAck, acknowledgement!.Type);
        Check.Equal(
            expected,
            BridgeRolePayloadCodec.Decode(acknowledgement.Payload.Span));
    }

    private static async Task ExpectBridgeMotionConfigAsync(Stream pipe)
    {
        var motionConfig = await ProtocolCodec.ReadAsync(pipe)
            .AsTask()
            .WaitAsync(TimeSpan.FromSeconds(2))
            .ConfigureAwait(false);
        Check.NotNull(motionConfig);
        Check.Equal(MessageType.MotionReplicationConfig, motionConfig!.Type);
        _ = AnimationReplicationPayloadCodec.DecodeMotionReplicationConfig(
            motionConfig.Payload.Span);
    }

    private static async Task SendBridgePlayerStateAsync(
        Stream pipe,
        NetEntityId entityId,
        SessionRole role)
    {
        var state = new PlayerStatePayload(
            entityId,
            (byte)role,
            PlayerLifecycle.Alive,
            Vector3.Zero,
            Vector3.Zero,
            0,
            1,
            PlayerStateFlags.None);
        await ProtocolCodec.WriteAsync(
            pipe,
            new ProtocolEnvelope(
                MessageType.PlayerState,
                2,
                unchecked((ulong)Environment.TickCount64),
                BinaryPayloadCodec.EncodePlayerState(state)))
            .ConfigureAwait(false);
    }

    private static async Task WaitForPlayerIdentityAsync(
        Stream pipe,
        PlayerIdentityPayload expected,
        CancellationToken cancellationToken)
    {
        while (true)
        {
            var envelope = await ProtocolCodec.ReadAsync(
                pipe,
                cancellationToken).ConfigureAwait(false);
            if (envelope is null)
            {
                throw new SelfTestException(
                    "Bridge pipe closed before player identity arrived.");
            }
            if (envelope.Type != MessageType.PlayerIdentity)
            {
                continue;
            }

            Check.Equal(
                expected,
                BinaryPayloadCodec.DecodePlayerIdentity(
                    envelope.Payload.Span));
            return;
        }
    }

    private static Task SessionMenuCodecAsync()
    {
        var credentials = SessionCredentials.Generate();
        var invite = new SessionInviteCode(
            "192.168.50.25",
            43120,
            43121,
            credentials.ExportToken());
        var code = SessionInviteCodeCodec.Encode(invite);
        Check.True(code.StartsWith(SessionInviteCodeCodec.Prefix, StringComparison.Ordinal));
        var decodedInvite = SessionInviteCodeCodec.Decode($"  {code}\r\n");
        Check.Equal(invite, decodedInvite);
        Check.Throws<FormatException>(
            () => SessionInviteCodeCodec.Decode("R2C1.invalid*"));

        var request = new SessionMenuRequestPayload(
            SessionMenuAction.JoinFromClipboard,
            code);
        Check.Equal(
            request,
            SessionMenuPayloadCodec.DecodeRequest(
                SessionMenuPayloadCodec.EncodeRequest(request)));
        var status = new SessionMenuStatusPayload(
            SessionMenuStatusKind.ReadyHost,
            "ready",
            code);
        Check.Equal(
            status,
            SessionMenuPayloadCodec.DecodeStatus(
                SessionMenuPayloadCodec.EncodeStatus(status)));
        Check.Throws<ProtocolException>(
            () => SessionMenuPayloadCodec.DecodeRequest(new byte[] { 255 }));
        foreach (var action in new[]
                 {
                     SessionMenuAction.ToggleSoloTest,
                     SessionMenuAction.ToggleGhostRecord,
                     SessionMenuAction.ToggleGhostReplay,
                     SessionMenuAction.StopSession
                 })
        {
            var localTestRequest = new SessionMenuRequestPayload(
                action,
                string.Empty);
            Check.Equal(
                localTestRequest,
                SessionMenuPayloadCodec.DecodeRequest(
                    SessionMenuPayloadCodec.EncodeRequest(localTestRequest)));
        }

        var bootstrap = new SidecarConfig
        {
            Role = SessionRole.Host,
            HostAddress = "127.0.0.1",
            SessionToken = SessionCredentials.Generate().ExportToken()
        };
        var activation = InGameSessionCoordinator.CreateGuest(bootstrap, code);
        Check.Equal(SessionRole.Guest, activation.Config.Role);
        Check.Equal(invite.HostAddress, activation.Config.HostAddress);
        Check.Equal(invite.SessionToken, activation.Config.SessionToken);
        var hamachiInvite = SessionInviteCodeCodec.Encode(
            invite with { HostAddress = "25.0.0.25" });
        var hamachiActivation = InGameSessionCoordinator.CreateGuest(
            bootstrap,
            hamachiInvite);
        Check.Equal("25.0.0.25", hamachiActivation.Config.HostAddress);
        var guestBootstrap = bootstrap with
        {
            Role = SessionRole.Guest,
            HostAddress = "25.0.0.25",
            SessionToken = invite.SessionToken
        };
        Check.Throws<ConfigurationException>(
            () => InGameSessionCoordinator.CreateGuest(
                guestBootstrap,
                " "));
        var ipv6Invite = SessionInviteCodeCodec.Encode(
            invite with { HostAddress = "fd00::25" });
        Check.Throws<ConfigurationException>(
            () => InGameSessionCoordinator.CreateGuest(
                bootstrap,
                ipv6Invite));
        Check.Equal(
            "25.0.0.25",
            InGameSessionCoordinator.SelectSuggestedLanAddress(
                "25.0.0.25",
                [
                    IPAddress.Parse("192.168.50.30"),
                    IPAddress.Parse("25.0.0.25")
                ])!);
        Check.Equal(
            "192.168.50.30",
            InGameSessionCoordinator.SelectSuggestedLanAddress(
                "25.0.0.99",
                [IPAddress.Parse("192.168.50.30")])!);
        var hosted = InGameSessionCoordinator.CreateHost(
            bootstrap,
            "192.168.50.30");
        Check.Equal(SessionRole.Host, hosted.Config.Role);
        Check.Equal("192.168.50.30", hosted.Config.HostAddress);
        Check.Equal(
            hosted.Config.SessionToken,
            SessionInviteCodeCodec.Decode(hosted.InviteCode).SessionToken);
        var publicInvite = SessionInviteCodeCodec.Encode(
            invite with { HostAddress = "203.0.113.10" });
        Check.Throws<ConfigurationException>(
            () => InGameSessionCoordinator.CreateGuest(
                bootstrap,
                publicInvite));
        Check.False(
            SecretRedactor.Redact($"invite={code}").Contains(
                code,
                StringComparison.Ordinal));
        return Task.CompletedTask;
    }

    private static async Task InGameJoinNegotiationAsync()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "CoopStory.SelfTest",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);

        JsonLineLogger? logger = null;
        SidecarRuntime? runtime = null;
        CancellationTokenSource? runtimeStop = null;
        Task? runtimeTask = null;
        try
        {
            var bootstrapCredentials = SessionCredentials.Generate();
            var hostCredentials = SessionCredentials.Generate();
            var inviteCode = SessionInviteCodeCodec.Encode(
                new SessionInviteCode(
                    "192.168.50.25",
                    checked((ushort)FindFreeTcpPort()),
                    checked((ushort)FindFreeUdpPort()),
                    hostCredentials.ExportToken()));
            var pipeBaseName =
                $"CoopStory.SelfTest.InGameMenu.{Guid.NewGuid():N}";
            var config = new SidecarConfig
            {
                Role = SessionRole.Host,
                HostAddress = "127.0.0.1",
                TcpPort = FindFreeTcpPort(),
                UdpPort = FindFreeUdpPort(),
                PipeName = pipeBaseName,
                SessionToken = bootstrapCredentials.ExportToken(),
                InGameMenuEnabled = true,
                ProfilePath = Path.Combine(root, "guest-profile.json"),
                LogPath = Path.Combine(root, "sidecar.jsonl"),
                Network = new NetworkConfig
                {
                    HeartbeatIntervalMs = 100,
                    HeartbeatTimeoutMs = 600,
                    ReconnectMinMs = 100,
                    ReconnectMaxMs = 400,
                    DiagnosticsIntervalMs = 250
                }
            };

            logger = new JsonLineLogger(config.LogPath);
            runtime = new SidecarRuntime(
                config,
                bootstrapCredentials,
                logger,
                IPAddress.Loopback);
            runtimeStop = new CancellationTokenSource();
            runtimeTask = runtime.RunAsync(runtimeStop.Token);

            await using var pipe = new NamedPipeClientStream(
                ".",
                PipeNameResolver.ResolveForCurrentUser(pipeBaseName),
                PipeDirection.InOut,
                PipeOptions.Asynchronous);
            await pipe.ConnectAsync(3000, CancellationToken.None)
                .ConfigureAwait(false);
            await ProtocolCodec.WriteAsync(
                pipe,
                new ProtocolEnvelope(
                    MessageType.Hello,
                    1,
                    unchecked((ulong)Environment.TickCount64),
                    ReadOnlyMemory<byte>.Empty)).ConfigureAwait(false);
            var waiting = await ProtocolCodec.ReadAsync(pipe)
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(2))
                .ConfigureAwait(false);
            Check.NotNull(waiting);
            Check.Equal(MessageType.SessionMenuStatus, waiting!.Type);
            Check.Equal(
                SessionMenuStatusKind.Waiting,
                SessionMenuPayloadCodec.DecodeStatus(waiting.Payload.Span).Kind);

            await ProtocolCodec.WriteAsync(
                pipe,
                new ProtocolEnvelope(
                    MessageType.SessionMenuRequest,
                    2,
                    unchecked((ulong)Environment.TickCount64),
                    SessionMenuPayloadCodec.EncodeRequest(
                        new SessionMenuRequestPayload(
                            SessionMenuAction.JoinFromClipboard,
                            inviteCode)))).ConfigureAwait(false);

            var sawStarting = false;
            var sawRole = false;
            var sawReady = false;
            var readyMessage = string.Empty;
            using var readStop = new CancellationTokenSource(
                TimeSpan.FromSeconds(3));
            while (!sawReady)
            {
                var envelope = await ProtocolCodec.ReadAsync(
                    pipe,
                    readStop.Token).ConfigureAwait(false);
                Check.NotNull(envelope);
                if (envelope!.Type == MessageType.HelloAck)
                {
                    Check.Equal(
                        SessionRole.Guest,
                        BridgeRolePayloadCodec.Decode(envelope.Payload.Span));
                    sawRole = true;
                }
                else if (envelope.Type == MessageType.SessionMenuStatus)
                {
                    var menuStatus =
                        SessionMenuPayloadCodec.DecodeStatus(envelope.Payload.Span);
                    sawStarting |=
                        menuStatus.Kind == SessionMenuStatusKind.StartingGuest;
                    sawReady |=
                        menuStatus.Kind == SessionMenuStatusKind.ReadyGuest;
                    if (menuStatus.Kind == SessionMenuStatusKind.ReadyGuest)
                    {
                        readyMessage = menuStatus.Message;
                    }
                }
            }

            Check.True(sawStarting);
            Check.True(sawRole);
            Check.True(
                readyMessage.Contains(
                    "REMOTE STREAMING",
                    StringComparison.Ordinal));

            var teleport = new CommandPayload(
                CommandOpcode.TeleportGuest,
                0,
                new NetEntityId(42),
                new Vector3(1, 2, 3),
                Heading: 90,
                Value: 0);
            await ProtocolCodec.WriteAsync(
                pipe,
                new ProtocolEnvelope(
                    MessageType.Command,
                    3,
                    unchecked((ulong)Environment.TickCount64),
                    BinaryPayloadCodec.EncodeCommand(teleport)))
                .ConfigureAwait(false);
            await WaitUntilAsync(
                () =>
                    LogContains(
                        config.LogPath,
                        "\"event\":\"bridge.authority-rejected\"") &&
                    LogContains(
                        config.LogPath,
                        "\"messageType\":\"Command\"") &&
                    LogContains(
                        config.LogPath,
                        "non-authoritative local role"),
                TimeSpan.FromSeconds(2),
                "Rejected guest-authored TeleportGuest was not recorded in diagnostics.")
                .ConfigureAwait(false);

            await runtime.OnNetworkAuthenticationRejectedAsync(
                "Host rejected handshake: session-id",
                CancellationToken.None).ConfigureAwait(false);
            var rejectionEnvelope = await ProtocolCodec.ReadAsync(pipe)
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(2))
                .ConfigureAwait(false);
            Check.NotNull(rejectionEnvelope);
            Check.Equal(
                MessageType.SessionMenuStatus,
                rejectionEnvelope!.Type);
            var rejectionStatus = SessionMenuPayloadCodec.DecodeStatus(
                rejectionEnvelope.Payload.Span);
            Check.Equal(
                SessionMenuStatusKind.Error,
                rejectionStatus.Kind);
            Check.True(
                rejectionStatus.Message.Contains(
                    "Check IPv4",
                    StringComparison.Ordinal));
            Check.True(
                rejectionStatus.Message.Contains(
                    "password set by the host",
                    StringComparison.Ordinal));

            await WriteSessionMenuRequestAsync(
                pipe,
                SessionMenuAction.StopSession,
                sequence: 4,
                CancellationToken.None).ConfigureAwait(false);
            SessionMenuStatusPayload stoppedStatus;
            using (var stopRead = new CancellationTokenSource(
                       TimeSpan.FromSeconds(4)))
            {
                while (true)
                {
                    var envelope = await ProtocolCodec.ReadAsync(
                        pipe,
                        stopRead.Token).ConfigureAwait(false);
                    Check.NotNull(envelope);
                    if (envelope!.Type != MessageType.SessionMenuStatus)
                    {
                        continue;
                    }
                    stoppedStatus = SessionMenuPayloadCodec.DecodeStatus(
                        envelope.Payload.Span);
                    if (stoppedStatus.Kind == SessionMenuStatusKind.Waiting)
                    {
                        break;
                    }
                }
            }
            Check.True(
                stoppedStatus.Message.Contains(
                    "HOST or JOIN",
                    StringComparison.Ordinal));
            Check.False(runtimeTask.IsCompleted);

            await ProtocolCodec.WriteAsync(
                pipe,
                new ProtocolEnvelope(
                    MessageType.SessionMenuRequest,
                    5,
                    unchecked((ulong)Environment.TickCount64),
                    SessionMenuPayloadCodec.EncodeRequest(
                        new SessionMenuRequestPayload(
                            SessionMenuAction.JoinFromClipboard,
                            inviteCode)))).ConfigureAwait(false);
            var sawRestartRole = false;
            var sawRestartReady = false;
            using (var restartRead = new CancellationTokenSource(
                       TimeSpan.FromSeconds(4)))
            {
                while (!sawRestartReady)
                {
                    var envelope = await ProtocolCodec.ReadAsync(
                        pipe,
                        restartRead.Token).ConfigureAwait(false);
                    Check.NotNull(envelope);
                    if (envelope!.Type == MessageType.HelloAck)
                    {
                        sawRestartRole =
                            BridgeRolePayloadCodec.Decode(
                                envelope.Payload.Span) == SessionRole.Guest;
                    }
                    else if (envelope.Type == MessageType.SessionMenuStatus)
                    {
                        sawRestartReady =
                            SessionMenuPayloadCodec.DecodeStatus(
                                envelope.Payload.Span).Kind ==
                            SessionMenuStatusKind.ReadyGuest;
                    }
                }
            }
            Check.True(sawRestartRole);
            Check.True(sawRestartReady);
            Check.False(
                LogContains(
                    config.LogPath,
                    hostCredentials.ExportToken()));
        }
        finally
        {
            if (runtimeStop is not null)
            {
                await runtimeStop.CancelAsync().ConfigureAwait(false);
            }
            await IgnoreCancellationAsync(runtimeTask).ConfigureAwait(false);
            if (runtime is not null)
            {
                await runtime.DisposeAsync().ConfigureAwait(false);
            }
            runtimeStop?.Dispose();
            if (logger is not null)
            {
                await logger.DisposeAsync().ConfigureAwait(false);
            }
            if (Directory.Exists(root))
            {
                Directory.Delete(root, recursive: true);
            }
        }
    }

    private static Task GhostRecordingStoreAsync()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "CoopStory.SelfTest.Ghost",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var path = Path.Combine(root, "ghost-last.json");
            var store = new GhostRecordingStore(path, snapshotRateHz: 20);
            var sourceEntity = NetEntityId.Create(7, 1);
            store.Begin();
            store.ObserveEquipment(new EquipmentStatePayload(
                sourceEntity,
                WeaponHash: 0xA6C9B6C9,
                Ammo: 42,
                EquipmentStateFlags.Equipped));
            for (var index = 0; index < 3; index++)
            {
                store.Capture(
                    new PlayerStatePayload(
                        sourceEntity,
                        Slot: (byte)SessionRole.Host,
                        PlayerLifecycle.Alive,
                        new Vector3(10 + index, 20 + index, 30),
                        new Vector3(1, 2, 0),
                        Heading: 90 + index,
                        HealthFraction: 0.75f,
                        PlayerStateFlags.Aiming |
                        PlayerStateFlags.AimTargetValid |
                        PlayerStateFlags.Firing |
                        PlayerStateFlags.OnlineModeDetected,
                        new Vector3(50, 60, 40),
                        FireSequence: (uint)(100 + index)),
                    observedAtMilliseconds: 1_000 + index * 50);
            }

            var saved = store.CompleteAndSave();
            Check.Equal(3, saved.Frames.Count);
            Check.True(File.Exists(path));
            Check.False(Directory.EnumerateFiles(root, "*.tmp").Any());

            var loaded = store.Load();
            Check.Equal(GhostRecordingStore.CurrentFormatVersion, loaded.FormatVersion);
            Check.Equal(20, loaded.SnapshotRateHz);
            Check.Equal(100, loaded.Frames[^1].OffsetMilliseconds);
            Check.Equal((uint)42, loaded.Frames[1].Ammo);
            var replayEntity = NetEntityId.Create(9, 2);
            var replay = loaded.Frames[1].ToPlayerState(replayEntity);
            Check.Equal(replayEntity, replay.EntityId);
            Check.Equal((byte)SessionRole.Guest, replay.Slot);
            Check.Equal(new Vector3(11, 21, 30), replay.Position);
            Check.True((replay.Flags & PlayerStateFlags.SyntheticTest) != 0);
            Check.False(
                (replay.Flags & PlayerStateFlags.OnlineModeDetected) != 0);
            Check.Equal((uint)101, replay.FireSequence);
        }
        finally
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, recursive: true);
            }
        }
        return Task.CompletedTask;
    }

    private static async Task LocalGameTestAsync()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "CoopStory.SelfTest",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);

        JsonLineLogger? logger = null;
        CancellationTokenSource? sessionStop = null;
        CancellationTokenSource? hostWriterStop = null;
        Task<LocalGameTestResult>? sessionTask = null;
        Task? writerTask = null;
        try
        {
            var credentials = SessionCredentials.Generate();
            var pipeBaseName = $"CoopStory.SelfTest.LocalGame.{Guid.NewGuid():N}";
            var config = new SidecarConfig
            {
                Role = SessionRole.Host,
                HostAddress = "127.0.0.1",
                TcpPort = FindFreeTcpPort(),
                UdpPort = FindFreeUdpPort(),
                PipeName = pipeBaseName,
                SessionToken = credentials.ExportToken(),
                ProfilePath = Path.Combine(root, "guest-profile.json"),
                LogPath = Path.Combine(root, "local-game-test.jsonl"),
                MotionReplicationMode =
                    MotionReplicationMode.AnimGraphReplica,
                Network = new NetworkConfig
                {
                    HeartbeatIntervalMs = 100,
                    HeartbeatTimeoutMs = 600,
                    ReconnectMinMs = 100,
                    ReconnectMaxMs = 400,
                    DiagnosticsIntervalMs = 250
                }
            };

            logger = new JsonLineLogger(config.LogPath);
            sessionStop = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            hostWriterStop = CancellationTokenSource.CreateLinkedTokenSource(
                sessionStop.Token);
            var sawStreamingStatus = 0;
            var sawGhostSavedStatus = 0;
            var ghostRecordingPath = Path.Combine(root, "ghost-last.json");
            sessionTask = LocalGameTestSession.RunAsync(
                config,
                credentials,
                logger,
                sessionStop.Token,
                status =>
                {
                    if (status.StartsWith(
                            "LOCAL_TEST_GUEST_STREAMING",
                            StringComparison.Ordinal))
                    {
                        Interlocked.Exchange(ref sawStreamingStatus, 1);
                    }
                    if (status.StartsWith(
                            "GHOST_RECORD_SAVED",
                            StringComparison.Ordinal))
                    {
                        Interlocked.Exchange(ref sawGhostSavedStatus, 1);
                    }
                },
                waitForInGameActivation: true,
                ghostRecordingPath: ghostRecordingPath);

            await using var pipe = new NamedPipeClientStream(
                ".",
                PipeNameResolver.ResolveForCurrentUser(pipeBaseName),
                PipeDirection.InOut,
                PipeOptions.Asynchronous);
            await pipe.ConnectAsync(3000, CancellationToken.None).ConfigureAwait(false);
            await ProtocolCodec.WriteAsync(
                pipe,
                new ProtocolEnvelope(
                    MessageType.Hello,
                    Sequence: 1,
                    Tick: unchecked((ulong)Environment.TickCount64),
                    ReadOnlyMemory<byte>.Empty)).ConfigureAwait(false);
            var acknowledgement = await ProtocolCodec.ReadAsync(pipe)
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(2))
                .ConfigureAwait(false);
            Check.NotNull(acknowledgement);
            Check.Equal(MessageType.HelloAck, acknowledgement!.Type);
            Check.Equal(
                SessionRole.Host,
                BridgeRolePayloadCodec.Decode(acknowledgement.Payload.Span));

            await ProtocolCodec.WriteAsync(
                pipe,
                new ProtocolEnvelope(
                    MessageType.SessionMenuRequest,
                    Sequence: 2,
                    Tick: unchecked((ulong)Environment.TickCount64),
                    SessionMenuPayloadCodec.EncodeRequest(
                        new SessionMenuRequestPayload(
                            SessionMenuAction.ToggleSoloTest,
                            string.Empty)))).ConfigureAwait(false);

            var hostPosition = new Vector3(100f, 200f, 30f);
            writerTask = WriteHostSnapshotsAsync(
                pipe,
                hostPosition,
                hostWriterStop.Token);

            PlayerStatePayload? syntheticGuest = null;
            PlayerIdentityPayload? syntheticIdentity = null;
            var syntheticAnimations =
                new List<PlayerAnimationStatePayload>();
            using var receiveStop = CancellationTokenSource.CreateLinkedTokenSource(
                sessionStop.Token);
            receiveStop.CancelAfter(TimeSpan.FromSeconds(5));
            while (syntheticGuest is null || syntheticIdentity is null ||
                   syntheticAnimations.Count < 2)
            {
                var envelope = await ProtocolCodec.ReadAsync(
                    pipe,
                    receiveStop.Token).ConfigureAwait(false);
                if (envelope is null)
                {
                    throw new SelfTestException(
                        "Bridge pipe closed before receiving a synthetic guest snapshot.");
                }

                if (envelope.Type == MessageType.PlayerIdentity)
                {
                    syntheticIdentity = BinaryPayloadCodec.DecodePlayerIdentity(
                        envelope.Payload.Span);
                    continue;
                }
                if (envelope.Type == MessageType.PlayerAnimationState)
                {
                    var animation = AnimationReplicationPayloadCodec
                        .DecodePlayerAnimationState(envelope.Payload.Span);
                    if (animation.Slot == (byte)SessionRole.Guest)
                    {
                        syntheticAnimations.Add(animation);
                    }
                    continue;
                }
                if (envelope.Type != MessageType.PlayerState)
                {
                    continue;
                }

                var state = BinaryPayloadCodec.DecodePlayerState(
                    envelope.Payload.Span);
                if (state.Slot == (byte)SessionRole.Guest)
                {
                    syntheticGuest = state;
                }
            }

            Check.False(syntheticGuest.Value.EntityId.IsNone);
            Check.Equal("SOLO BOT", syntheticIdentity!.Value.Nickname);
            Check.Equal(PlayerLifecycle.Alive, syntheticGuest.Value.Lifecycle);
            Check.True(
                (syntheticGuest.Value.Flags &
                 PlayerStateFlags.SyntheticTest) != 0,
                "The loopback guest must enable the solo-only target marker.");
            Check.Near(
                8f,
                Vector3.Distance(hostPosition, syntheticGuest.Value.Position),
                tolerance: 0.01f);
            Check.Near(hostPosition.Z, syntheticGuest.Value.Position.Z);
            var matchingAnimations = syntheticAnimations
                .Where(animation =>
                    animation.EntityId == syntheticGuest.Value.EntityId)
                .ToArray();
            Check.True(
                matchingAnimations.Length >= 2,
                "The loopback AnimGraph lane did not deliver two samples.");
            foreach (var animation in matchingAnimations)
            {
                Check.Equal(
                    PlayerAnimationSampleSource.LocomotionNative,
                    animation.Source);
                Check.Equal(
                    syntheticGuest.Value.LocomotionEpoch,
                    animation.LocomotionEpoch);
            }
            Check.True(
                matchingAnimations[1].SampleSequence >
                    matchingAnimations[0].SampleSequence,
                "Synthetic animation sampleSequence did not advance.");
            await WaitUntilAsync(
                () => LogContains(
                    config.LogPath,
                    "\"event\":\"diagnostics.streaming\""),
                TimeSpan.FromSeconds(2),
                "Periodic streaming diagnostics were not written.")
                .ConfigureAwait(false);

            await hostWriterStop.CancelAsync().ConfigureAwait(false);
            await IgnoreCancellationAsync(writerTask).ConfigureAwait(false);
            await AssertGuestStreamExpiresAsync(
                pipe,
                sessionStop.Token).ConfigureAwait(false);

            await WriteSessionMenuRequestAsync(
                pipe,
                SessionMenuAction.ToggleGhostRecord,
                sequence: 1_000,
                sessionStop.Token).ConfigureAwait(false);
            hostWriterStop.Dispose();
            hostWriterStop = CancellationTokenSource.CreateLinkedTokenSource(
                sessionStop.Token);
            hostWriterStop.CancelAfter(TimeSpan.FromMilliseconds(450));
            writerTask = WriteHostSnapshotsAsync(
                pipe,
                hostPosition,
                hostWriterStop.Token,
                initialSequence: 2_000);
            await IgnoreCancellationAsync(writerTask).ConfigureAwait(false);
            await WriteSessionMenuRequestAsync(
                pipe,
                SessionMenuAction.ToggleGhostRecord,
                sequence: 3_000,
                sessionStop.Token).ConfigureAwait(false);
            await WaitUntilAsync(
                () => File.Exists(ghostRecordingPath),
                TimeSpan.FromSeconds(2),
                "Ghost Record did not save its route atomically.")
                .ConfigureAwait(false);

            await WriteSessionMenuRequestAsync(
                pipe,
                SessionMenuAction.ToggleGhostReplay,
                sequence: 4_000,
                sessionStop.Token).ConfigureAwait(false);
            hostWriterStop.Dispose();
            hostWriterStop = CancellationTokenSource.CreateLinkedTokenSource(
                sessionStop.Token);
            writerTask = WriteHostSnapshotsAsync(
                pipe,
                hostPosition,
                hostWriterStop.Token,
                initialSequence: 5_000);

            PlayerStatePayload? ghostReplay = null;
            PlayerIdentityPayload? ghostIdentity = null;
            PlayerAnimationStatePayload? ghostAnimation = null;
            var ghostAnimationCandidates =
                new List<PlayerAnimationStatePayload>();
            using (var ghostReceiveStop =
                   CancellationTokenSource.CreateLinkedTokenSource(
                       sessionStop.Token))
            {
                ghostReceiveStop.CancelAfter(TimeSpan.FromSeconds(4));
                while (ghostReplay is null || ghostIdentity is null ||
                       ghostAnimation is null)
                {
                    var envelope = await ProtocolCodec.ReadAsync(
                        pipe,
                        ghostReceiveStop.Token).ConfigureAwait(false);
                    if (envelope is null)
                    {
                        throw new SelfTestException(
                            "Bridge pipe closed before Ghost Replay reached it.");
                    }
                    if (envelope.Type == MessageType.PlayerIdentity)
                    {
                        var identity = BinaryPayloadCodec.DecodePlayerIdentity(
                            envelope.Payload.Span);
                        if (identity.Nickname == "GHOST REPLAY")
                        {
                            ghostIdentity = identity;
                        }
                        continue;
                    }
                    if (envelope.Type == MessageType.PlayerAnimationState)
                    {
                        var animation = AnimationReplicationPayloadCodec
                            .DecodePlayerAnimationState(envelope.Payload.Span);
                        if (animation.Slot == (byte)SessionRole.Guest)
                        {
                            ghostAnimationCandidates.Add(animation);
                        }
                    }
                    if (envelope.Type != MessageType.PlayerState)
                    {
                        if (ghostReplay is { } knownReplay)
                        {
                            ghostAnimation = ghostAnimationCandidates
                                .FirstOrDefault(animation =>
                                    animation.EntityId ==
                                        knownReplay.EntityId);
                            if (ghostAnimation.Value.EntityId.IsNone)
                            {
                                ghostAnimation = null;
                            }
                        }
                        continue;
                    }
                    var state = BinaryPayloadCodec.DecodePlayerState(
                        envelope.Payload.Span);
                    if (state.Slot == (byte)SessionRole.Guest &&
                        Vector3.Distance(state.Position, hostPosition) < 0.01f)
                    {
                        ghostReplay = state;
                        ghostAnimation = ghostAnimationCandidates
                            .FirstOrDefault(animation =>
                                animation.EntityId == state.EntityId);
                        if (ghostAnimation.Value.EntityId.IsNone)
                        {
                            ghostAnimation = null;
                        }
                    }
                }
            }
            Check.Equal(
                ghostReplay.Value.EntityId,
                ghostIdentity.Value.EntityId);
            Check.True(
                (ghostReplay.Value.Flags & PlayerStateFlags.SyntheticTest) != 0);
            Check.Equal(
                ghostReplay.Value.EntityId,
                ghostAnimation.Value.EntityId);
            Check.Equal(
                ghostReplay.Value.LocomotionEpoch,
                ghostAnimation.Value.LocomotionEpoch);
            Check.Equal(
                PlayerAnimationSampleSource.LocomotionNative,
                ghostAnimation.Value.Source);
            Check.True(ghostAnimation.Value.SampleSequence > 0);
            Check.Equal(1, Volatile.Read(ref sawGhostSavedStatus));

            await sessionStop.CancelAsync().ConfigureAwait(false);
            var result = await sessionTask.ConfigureAwait(false);
            Check.True(result.HostSnapshotsObserved > 0);
            Check.True(result.GuestSnapshotsSent > 0);
            Check.True(result.GuestSnapshotsDelivered > 0);
            Check.Equal(1, Volatile.Read(ref sawStreamingStatus));
        }
        finally
        {
            if (sessionStop is not null)
            {
                await sessionStop.CancelAsync().ConfigureAwait(false);
            }
            if (hostWriterStop is not null)
            {
                await hostWriterStop.CancelAsync().ConfigureAwait(false);
            }

            await IgnoreCancellationAsync(writerTask, sessionTask)
                .ConfigureAwait(false);
            hostWriterStop?.Dispose();
            sessionStop?.Dispose();
            if (logger is not null)
            {
                await logger.DisposeAsync().ConfigureAwait(false);
            }

            var resolvedRoot = Path.GetFullPath(root);
            var expectedParent = Path.GetFullPath(
                Path.Combine(Path.GetTempPath(), "CoopStory.SelfTest"));
            if (resolvedRoot.StartsWith(expectedParent, StringComparison.OrdinalIgnoreCase) &&
                Directory.Exists(resolvedRoot))
            {
                Directory.Delete(resolvedRoot, recursive: true);
            }
        }
    }

    private static Task SyntheticPuppetCourseAsync()
    {
        var idle = SyntheticPlayerMotionCourse.Sample(
            LocalGameTestMotionProfile.PuppetCourse,
            TimeSpan.Zero,
            hostHeadingDegrees: 180f);
        Check.Equal("idle", idle.Phase);
        Check.Near(8f, idle.Offset.Length(), tolerance: 0.001f);
        Check.Near(0f, idle.RelativeVelocity.Length(), tolerance: 0.001f);

        var walk = SyntheticPlayerMotionCourse.Sample(
            LocalGameTestMotionProfile.PuppetCourse,
            TimeSpan.FromSeconds(6),
            hostHeadingDegrees: 0f);
        Check.Equal("walk", walk.Phase);
        Check.Near(1f, walk.RelativeVelocity.Length(), tolerance: 0.001f);

        var run = SyntheticPlayerMotionCourse.Sample(
            LocalGameTestMotionProfile.PuppetCourse,
            TimeSpan.FromSeconds(12),
            hostHeadingDegrees: 0f);
        Check.Equal("run", run.Phase);
        Check.Near(3f, run.RelativeVelocity.Length(), tolerance: 0.001f);

        var sprint = SyntheticPlayerMotionCourse.Sample(
            LocalGameTestMotionProfile.PuppetCourse,
            TimeSpan.FromSeconds(18),
            hostHeadingDegrees: 0f);
        Check.Equal("sprint", sprint.Phase);
        Check.Near(5.5f, sprint.RelativeVelocity.Length(), tolerance: 0.001f);

        foreach (var boundarySeconds in new[] { 3d, 9d, 15d, 21d, 24d, 30d })
        {
            var before = SyntheticPlayerMotionCourse.Sample(
                LocalGameTestMotionProfile.PuppetCourse,
                TimeSpan.FromSeconds(boundarySeconds - 0.001d),
                hostHeadingDegrees: 0f);
            var after = SyntheticPlayerMotionCourse.Sample(
                LocalGameTestMotionProfile.PuppetCourse,
                TimeSpan.FromSeconds(boundarySeconds + 0.001d),
                hostHeadingDegrees: 0f);
            Check.True(
                Vector3.Distance(before.Offset, after.Offset) < 0.02f,
                $"The puppet course jumped at phase boundary {boundarySeconds}s.");
        }

        var beforeBoundary = SyntheticPlayerMotionCourse.Sample(
            LocalGameTestMotionProfile.PuppetCourse,
            TimeSpan.FromSeconds(35.999),
            hostHeadingDegrees: 0f);
        var afterBoundary = SyntheticPlayerMotionCourse.Sample(
            LocalGameTestMotionProfile.PuppetCourse,
            TimeSpan.FromSeconds(36.001),
            hostHeadingDegrees: 0f);
        Check.True(
            Vector3.Distance(beforeBoundary.Offset, afterBoundary.Offset) < 0.02f,
            "The repeated puppet course must not teleport at its cycle boundary.");

        var follow = SyntheticPlayerMotionCourse.Sample(
            LocalGameTestMotionProfile.FollowHost,
            TimeSpan.FromMinutes(5),
            hostHeadingDegrees: 90f);
        Check.Equal("follow", follow.Phase);
        Check.Near(2f, follow.Offset.Length(), tolerance: 0.001f);
        return Task.CompletedTask;
    }

    private static Task SyntheticActionCourseAsync()
    {
        var origin = new Vector3(10f, 20f, 30f);
        var idle = SyntheticPlayerActionCourse.Sample(
            TimeSpan.Zero,
            origin,
            playerHeadingDegrees: 0f);
        Check.Equal("movement-only", idle.Phase);
        Check.Equal(
            SyntheticPlayerActionCourse.CattlemanRevolverHash,
            idle.WeaponHash);
        Check.Equal(PlayerStateFlags.None, idle.Flags);
        Check.False(idle.Reloading);

        var forwardShot = SyntheticPlayerActionCourse.Sample(
            TimeSpan.FromSeconds(11.6),
            origin,
            playerHeadingDegrees: 0f);
        Check.Equal("fire-forward", forwardShot.Phase);
        Check.True(
            (forwardShot.Flags & PlayerStateFlags.Aiming) != 0,
            "Forward shot must preserve aiming.");
        Check.True(
            (forwardShot.Flags & PlayerStateFlags.Firing) != 0,
            "Forward shot must carry a firing pulse.");
        Check.True(
            (forwardShot.Flags & PlayerStateFlags.AimTargetValid) != 0,
            "Forward shot requires an explicit aim target.");
        Check.Equal((uint)1, forwardShot.FireSequence);
        Check.Near(35f, forwardShot.AimTarget.X, 0.001f);
        Check.Near(20f, forwardShot.AimTarget.Y, 0.001f);

        var upShot = SyntheticPlayerActionCourse.Sample(
            TimeSpan.FromSeconds(14.6),
            origin,
            playerHeadingDegrees: 0f);
        Check.Equal("fire-up", upShot.Phase);
        Check.Equal((uint)2, upShot.FireSequence);
        Check.True(
            upShot.AimTarget.Z > forwardShot.AimTarget.Z,
            "Up shot must raise the aim target.");

        var downShot = SyntheticPlayerActionCourse.Sample(
            TimeSpan.FromSeconds(17.6),
            origin,
            playerHeadingDegrees: 0f);
        Check.Equal("fire-down", downShot.Phase);
        Check.Equal((uint)3, downShot.FireSequence);
        Check.True(
            downShot.AimTarget.Z < origin.Z,
            "Down shot must lower the aim target.");

        var leftShot = SyntheticPlayerActionCourse.Sample(
            TimeSpan.FromSeconds(20.6),
            origin,
            playerHeadingDegrees: 0f);
        Check.Equal("fire-left", leftShot.Phase);
        Check.Equal((uint)4, leftShot.FireSequence);
        Check.True(leftShot.AimTarget.Y > origin.Y);

        var rightShot = SyntheticPlayerActionCourse.Sample(
            TimeSpan.FromSeconds(23.6),
            origin,
            playerHeadingDegrees: 0f);
        Check.Equal("fire-right", rightShot.Phase);
        Check.Equal((uint)5, rightShot.FireSequence);
        Check.True(rightShot.AimTarget.Y < origin.Y);

        var behindShot = SyntheticPlayerActionCourse.Sample(
            TimeSpan.FromSeconds(26.6),
            origin,
            playerHeadingDegrees: 0f);
        Check.Equal("fire-behind-180", behindShot.Phase);
        Check.Equal((uint)6, behindShot.FireSequence);
        Check.True(behindShot.AimTarget.X < origin.X);

        var reload = SyntheticPlayerActionCourse.Sample(
            TimeSpan.FromSeconds(31),
            origin,
            playerHeadingDegrees: 0f);
        Check.Equal("reload", reload.Phase);
        Check.True(reload.Reloading);
        Check.Equal(PlayerStateFlags.None, reload.Flags);

        var jump = SyntheticPlayerActionCourse.Sample(
            TimeSpan.FromSeconds(36.2),
            origin,
            playerHeadingDegrees: 0f);
        Check.Equal("jump", jump.Phase);
        Check.True(
            (jump.Flags & PlayerStateFlags.Jumping) != 0,
            "Solo action course must exercise replicated jump state.");

        var beforeReload = SyntheticPlayerActionCourse.CoupleMovement(
            TimeSpan.FromSeconds(29.999));
        var duringReload = SyntheticPlayerActionCourse.CoupleMovement(
            TimeSpan.FromSeconds(32));
        var reloadEnd = SyntheticPlayerActionCourse.CoupleMovement(
            TimeSpan.FromSeconds(34));
        var afterReload = SyntheticPlayerActionCourse.CoupleMovement(
            TimeSpan.FromSeconds(34.001));
        Check.False(beforeReload.MovementBlocked);
        Check.True(duringReload.MovementBlocked);
        Check.True(
            string.Equals(
                "reload",
                duringReload.BlockingPhase,
                StringComparison.Ordinal),
            "Reload must identify the movement-blocking action.");
        Check.Near(
            30f,
            (float)duringReload.MotionElapsed.TotalSeconds,
            tolerance: 0.001f);
        Check.False(reloadEnd.MovementBlocked);
        Check.Near(
            30f,
            (float)reloadEnd.MotionElapsed.TotalSeconds,
            tolerance: 0.001f);
        Check.Near(
            0.001f,
            (float)(afterReload.MotionElapsed - reloadEnd.MotionElapsed)
                .TotalSeconds,
            tolerance: 0.001f);

        var nextCycleStart = SyntheticPlayerActionCourse.CoupleMovement(
            TimeSpan.FromSeconds(60));
        Check.False(nextCycleStart.MovementBlocked);
        Check.Near(
            56f,
            (float)nextCycleStart.MotionElapsed.TotalSeconds,
            tolerance: 0.001f);

        var nextCycleShot = SyntheticPlayerActionCourse.Sample(
            TimeSpan.FromSeconds(71.6),
            origin,
            playerHeadingDegrees: 0f);
        Check.Equal((uint)7, nextCycleShot.FireSequence);
        return Task.CompletedTask;
    }

    private static async Task BridgeGoodbyeStopsRuntimeAsync()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "CoopStory.SelfTest",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);

        JsonLineLogger? logger = null;
        SidecarRuntime? runtime = null;
        CancellationTokenSource? runtimeStop = null;
        Task? runtimeTask = null;
        try
        {
            var credentials = SessionCredentials.Generate();
            var pipeBaseName = $"CoopStory.SelfTest.Goodbye.{Guid.NewGuid():N}";
            var logPath = Path.Combine(root, "goodbye.jsonl");
            var config = new SidecarConfig
            {
                Role = SessionRole.Host,
                HostAddress = "127.0.0.1",
                TcpPort = FindFreeTcpPort(),
                UdpPort = FindFreeUdpPort(),
                PipeName = pipeBaseName,
                SessionToken = credentials.ExportToken(),
                ProfilePath = Path.Combine(root, "guest-profile.json"),
                LogPath = logPath
            };

            logger = new JsonLineLogger(logPath);
            runtime = new SidecarRuntime(
                config,
                credentials,
                logger,
                IPAddress.Loopback);
            runtimeStop = new CancellationTokenSource();
            runtimeTask = runtime.RunAsync(runtimeStop.Token);

            await using var pipe = new NamedPipeClientStream(
                ".",
                PipeNameResolver.ResolveForCurrentUser(pipeBaseName),
                PipeDirection.InOut,
                PipeOptions.Asynchronous);
            await pipe.ConnectAsync(3000, CancellationToken.None).ConfigureAwait(false);
            await ProtocolCodec.WriteAsync(
                pipe,
                new ProtocolEnvelope(
                    MessageType.Hello,
                    Sequence: 1,
                    Tick: unchecked((ulong)Environment.TickCount64),
                    ReadOnlyMemory<byte>.Empty)).ConfigureAwait(false);
            _ = await ProtocolCodec.ReadAsync(pipe)
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(2))
                .ConfigureAwait(false);

            const string goodbyeReason = "offline Story Mode guard failed";
            await ProtocolCodec.WriteAsync(
                pipe,
                new ProtocolEnvelope(
                    MessageType.Goodbye,
                    Sequence: 2,
                    Tick: unchecked((ulong)Environment.TickCount64),
                    System.Text.Encoding.UTF8.GetBytes(goodbyeReason)))
                .ConfigureAwait(false);

            BridgeShutdownException? shutdown = null;
            try
            {
                await runtimeTask.WaitAsync(TimeSpan.FromSeconds(3))
                    .ConfigureAwait(false);
            }
            catch (BridgeShutdownException exception)
            {
                shutdown = exception;
            }

            Check.NotNull(shutdown);
            Check.Equal(goodbyeReason, shutdown!.Reason);
            await WaitUntilAsync(
                () => LogContains(logPath, goodbyeReason),
                TimeSpan.FromSeconds(2),
                "Sidecar did not log the bridge Goodbye reason.")
                .ConfigureAwait(false);
        }
        finally
        {
            if (runtimeStop is not null)
            {
                await runtimeStop.CancelAsync().ConfigureAwait(false);
            }

            if (runtimeTask is { IsCompleted: false })
            {
                await IgnoreCancellationAsync(runtimeTask).ConfigureAwait(false);
            }

            if (runtime is not null)
            {
                await runtime.DisposeAsync().ConfigureAwait(false);
            }

            runtimeStop?.Dispose();
            if (logger is not null)
            {
                await logger.DisposeAsync().ConfigureAwait(false);
            }

            var resolvedRoot = Path.GetFullPath(root);
            var expectedParent = Path.GetFullPath(
                Path.Combine(Path.GetTempPath(), "CoopStory.SelfTest"));
            if (resolvedRoot.StartsWith(expectedParent, StringComparison.OrdinalIgnoreCase) &&
                Directory.Exists(resolvedRoot))
            {
                Directory.Delete(resolvedRoot, recursive: true);
            }
        }
    }

    private static async Task WriteHostSnapshotsAsync(
        Stream pipe,
        Vector3 position,
        CancellationToken cancellationToken,
        uint initialSequence = 10)
    {
        var entityId = NetEntityId.Create(0xA1B2C3D4, 1);
        var sequence = initialSequence;
        using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(50));
        while (await timer.WaitForNextTickAsync(cancellationToken)
                   .ConfigureAwait(false))
        {
            var state = new PlayerStatePayload(
                entityId,
                Slot: (byte)SessionRole.Host,
                PlayerLifecycle.Alive,
                position,
                Vector3.Zero,
                Heading: 90f,
                HealthFraction: 1f,
                PlayerStateFlags.None);
            await ProtocolCodec.WriteAsync(
                pipe,
                new ProtocolEnvelope(
                    MessageType.PlayerState,
                    sequence++,
                    unchecked((ulong)Environment.TickCount64),
                    BinaryPayloadCodec.EncodePlayerState(state)),
                cancellationToken).ConfigureAwait(false);
        }
    }

    private static ValueTask WriteSessionMenuRequestAsync(
        Stream pipe,
        SessionMenuAction action,
        uint sequence,
        CancellationToken cancellationToken) =>
        ProtocolCodec.WriteAsync(
            pipe,
            new ProtocolEnvelope(
                MessageType.SessionMenuRequest,
                sequence,
                unchecked((ulong)Environment.TickCount64),
                SessionMenuPayloadCodec.EncodeRequest(
                    new SessionMenuRequestPayload(action, string.Empty))),
            cancellationToken);

    private static async Task AssertGuestStreamExpiresAsync(
        Stream pipe,
        CancellationToken cancellationToken)
    {
        using var observation = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken);
        observation.CancelAfter(TimeSpan.FromMilliseconds(1_800));
        var elapsed = Stopwatch.StartNew();
        long lastGuestAtMs = -1;
        try
        {
            while (true)
            {
                var envelope = await ProtocolCodec.ReadAsync(
                    pipe,
                    observation.Token).ConfigureAwait(false);
                if (envelope is null)
                {
                    throw new SelfTestException(
                        "Bridge pipe closed while checking stale-host expiry.");
                }
                if (envelope.Type != MessageType.PlayerState)
                {
                    continue;
                }

                var state = BinaryPayloadCodec.DecodePlayerState(
                    envelope.Payload.Span);
                if (state.Slot == (byte)SessionRole.Guest)
                {
                    lastGuestAtMs = elapsed.ElapsedMilliseconds;
                }
            }
        }
        catch (OperationCanceledException) when (observation.IsCancellationRequested)
        {
        }

        Check.True(
            lastGuestAtMs < 1_400,
            $"Synthetic guest kept streaming stale host data at {lastGuestAtMs} ms.");
    }

    private static async Task ExerciseRoleNegotiationAsync(SessionRole role)
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "CoopStory.SelfTest",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);

        JsonLineLogger? logger = null;
        SidecarRuntime? runtime = null;
        CancellationTokenSource? runtimeStop = null;
        Task? runtimeTask = null;
        try
        {
            var credentials = SessionCredentials.Generate();
            var pipeBaseName = $"CoopStory.SelfTest.Role.{Guid.NewGuid():N}";
            var config = new SidecarConfig
            {
                Role = role,
                HostAddress = "127.0.0.1",
                TcpPort = FindFreeTcpPort(),
                UdpPort = FindFreeUdpPort(),
                PipeName = pipeBaseName,
                SessionToken = credentials.ExportToken(),
                ProfilePath = Path.Combine(root, "guest-profile.json"),
                LogPath = Path.Combine(root, "sidecar.jsonl"),
                Network = new NetworkConfig
                {
                    HeartbeatIntervalMs = 100,
                    HeartbeatTimeoutMs = 600,
                    ReconnectMinMs = 100,
                    ReconnectMaxMs = 400
                }
            };

            logger = new JsonLineLogger(config.LogPath);
            runtime = new SidecarRuntime(
                config,
                credentials,
                logger,
                IPAddress.Loopback);
            runtimeStop = new CancellationTokenSource();
            runtimeTask = runtime.RunAsync(runtimeStop.Token);

            await using var pipe = new NamedPipeClientStream(
                ".",
                PipeNameResolver.ResolveForCurrentUser(pipeBaseName),
                PipeDirection.InOut,
                PipeOptions.Asynchronous);
            await pipe.ConnectAsync(3000, CancellationToken.None).ConfigureAwait(false);

            var hello = new ProtocolEnvelope(
                MessageType.Hello,
                Sequence: 1,
                Tick: unchecked((ulong)Environment.TickCount64),
                ReadOnlyMemory<byte>.Empty);
            await ProtocolCodec.WriteAsync(pipe, hello).ConfigureAwait(false);
            var acknowledgement = await ProtocolCodec.ReadAsync(pipe)
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(2))
                .ConfigureAwait(false);
            Check.NotNull(acknowledgement);
            Check.Equal(MessageType.HelloAck, acknowledgement!.Type);
            Check.Equal(1, acknowledgement.Payload.Length);
            Check.Equal(
                role,
                BridgeRolePayloadCodec.Decode(acknowledgement.Payload.Span));
        }
        finally
        {
            if (runtimeStop is not null)
            {
                await runtimeStop.CancelAsync().ConfigureAwait(false);
            }

            await IgnoreCancellationAsync(runtimeTask).ConfigureAwait(false);
            if (runtime is not null)
            {
                await runtime.DisposeAsync().ConfigureAwait(false);
            }

            runtimeStop?.Dispose();
            if (logger is not null)
            {
                await logger.DisposeAsync().ConfigureAwait(false);
            }

            var resolvedRoot = Path.GetFullPath(root);
            var expectedParent = Path.GetFullPath(
                Path.Combine(Path.GetTempPath(), "CoopStory.SelfTest"));
            if (resolvedRoot.StartsWith(expectedParent, StringComparison.OrdinalIgnoreCase) &&
                Directory.Exists(resolvedRoot))
            {
                Directory.Delete(resolvedRoot, recursive: true);
            }
        }
    }

    private static async Task SessionConfigPairAsync()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "CoopStory.SelfTest",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var output = Path.Combine(root, "paired-session");
            var result = await SessionConfigPairGenerator.CreateAsync(
                output,
                "192.168.50.10",
                FindFreeTcpPort(),
                FindFreeUdpPort()).ConfigureAwait(false);
            Check.Equal(Path.GetFullPath(output), result.DirectoryPath);
            Check.True(File.Exists(result.HostConfigPath));
            Check.True(File.Exists(result.GuestConfigPath));
            Check.Equal(2, Directory.GetFiles(output).Length);

            var host = await SidecarConfigStore.LoadAsync(result.HostConfigPath)
                .ConfigureAwait(false);
            var guest = await SidecarConfigStore.LoadAsync(result.GuestConfigPath)
                .ConfigureAwait(false);
            Check.Equal(SessionRole.Host, host.Role);
            Check.Equal(SessionRole.Guest, guest.Role);
            Check.Equal("192.168.50.10", host.HostAddress);
            Check.Equal(host.HostAddress, guest.HostAddress);
            Check.Equal(host.TcpPort, guest.TcpPort);
            Check.Equal(host.UdpPort, guest.UdpPort);
            Check.Equal(host.SessionToken, guest.SessionToken);
            Check.False(string.IsNullOrWhiteSpace(host.SessionToken));
            Check.False(string.Equals(
                host.LogPath,
                guest.LogPath,
                StringComparison.OrdinalIgnoreCase));

            await Check.ThrowsAsync<ConfigurationException>(
                () => SessionConfigPairGenerator.CreateAsync(
                    output,
                    "192.168.50.10")).ConfigureAwait(false);
            await Check.ThrowsAsync<ConfigurationException>(
                () => SessionConfigPairGenerator.CreateAsync(
                    Path.Combine(root, "loopback-session"),
                    "127.0.0.1")).ConfigureAwait(false);
            Check.False(Directory.Exists(
                Path.Combine(root, "loopback-session")));
        }
        finally
        {
            DeleteOwnedSelfTestRoot(root);
        }
    }

    private static async Task ListenerIsolationAsync()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "CoopStory.SelfTest",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var credentials = SessionCredentials.Generate();
            var config = new SidecarConfig
            {
                Role = SessionRole.Host,
                HostAddress = "192.168.50.10",
                TcpPort = FindFreeTcpPort(),
                UdpPort = FindFreeUdpPort(),
                SessionToken = credentials.ExportToken(),
                ProfilePath = Path.Combine(root, "guest-profile.json"),
                LogPath = Path.Combine(root, "listener.jsonl")
            };
            await using var logger = new JsonLineLogger(config.LogPath);
            await using var normalHost = new LanSessionHost(
                config,
                credentials,
                logger);
            await using var isolatedHost = new LanSessionHost(
                config,
                credentials,
                logger,
                IPAddress.Loopback);
            Check.Equal(IPAddress.Any, normalHost.ListenAddress);
            Check.Equal(IPAddress.Loopback, isolatedHost.ListenAddress);
        }
        finally
        {
            DeleteOwnedSelfTestRoot(root);
        }
    }

    private static Task MessageFlowDiagnosticsAsync()
    {
        var diagnostics = new MessageFlowDiagnostics();
        diagnostics.Observe(
            MessageFlowDirection.NetworkToBridge,
            MessageType.PlayerState,
            1_000);
        diagnostics.Observe(
            MessageFlowDirection.NetworkToBridge,
            MessageType.PlayerState,
            1_050);
        diagnostics.Observe(
            MessageFlowDirection.NetworkToBridge,
            MessageType.PlayerState,
            1_100);
        diagnostics.Observe(
            MessageFlowDirection.NetworkToBridge,
            MessageType.PlayerState,
            1_300);
        diagnostics.MarkDelivered(
            MessageFlowDirection.NetworkToBridge,
            MessageType.PlayerState);
        diagnostics.MarkDelivered(
            MessageFlowDirection.NetworkToBridge,
            MessageType.PlayerState);
        diagnostics.MarkDropped(
            MessageFlowDirection.NetworkToBridge,
            MessageType.PlayerState);
        diagnostics.MarkCoalesced(
            MessageFlowDirection.NetworkToBridge,
            MessageType.PlayerState);

        var snapshot = diagnostics.Capture(
            MessageFlowDirection.NetworkToBridge,
            MessageType.PlayerState,
            1_500);
        Check.True(snapshot.HasValue);
        var stream = snapshot ?? throw new SelfTestException(
            "Expected a message-flow snapshot.");
        Check.Equal(4L, stream.Observed);
        Check.Equal(2L, stream.Delivered);
        Check.Equal(1L, stream.Dropped);
        Check.Equal(1L, stream.Coalesced);
        Check.Equal(200L, stream.LastObservedAgeMs);
        Check.Equal(100.0, stream.AverageGapMs);
        Check.Equal(200L, stream.P95GapMs);
        Check.Equal(200L, stream.MaximumGapMs);

        diagnostics.Reset();
        Check.Equal(0, diagnostics.Capture(1_600).Count);
        return Task.CompletedTask;
    }

    private static Task DiagnosticLogRotationAsync()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            $"coopstory-log-rotation-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        try
        {
            var path = Path.Combine(root, "sidecar.jsonl");
            File.WriteAllText(path, "current-segment-is-over-budget");
            File.WriteAllText($"{path}.1", "previous-one");
            File.WriteAllText($"{path}.2", "previous-two");
            File.WriteAllText($"{path}.3", "expired-three");

            JsonLineLogger.RotateAtSessionStart(
                path,
                maximumSegmentBytes: 8,
                archiveSegments: 3);

            Check.False(File.Exists(path));
            Check.Equal(
                "current-segment-is-over-budget",
                File.ReadAllText($"{path}.1"));
            Check.Equal("previous-one", File.ReadAllText($"{path}.2"));
            Check.Equal("previous-two", File.ReadAllText($"{path}.3"));
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
        return Task.CompletedTask;
    }

    private static async Task DiagnosticsRedactionAsync()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "CoopStory.SelfTest",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var credentials = SessionCredentials.Generate();
            var token = credentials.ExportToken();
            var configPath = Path.Combine(root, "host.config.json");
            var logPath = Path.Combine(root, "host-sidecar.jsonl");
            var config = new SidecarConfig
            {
                Role = SessionRole.Host,
                HostAddress = "192.168.50.10",
                TcpPort = FindFreeTcpPort(),
                UdpPort = FindFreeUdpPort(),
                SessionToken = token,
                ProfilePath = Path.Combine(root, "guest-profile.json"),
                LogPath = logPath
            };
            await SidecarConfigStore.SaveAsync(configPath, config)
                .ConfigureAwait(false);

            await using (var logger = new JsonLineLogger(logPath))
            {
                await logger.WarningAsync(
                    "selftest.secret",
                    $"A credential accidentally reached a message: {token}.")
                    .ConfigureAwait(false);
            }
            Check.False(File.ReadAllText(logPath).Contains(
                token,
                StringComparison.Ordinal));
            Check.True(File.ReadAllText(logPath).Contains(
                SecretRedactor.Replacement,
                StringComparison.Ordinal));

            var archivePath = Path.Combine(root, "diagnostics.zip");
            var result = await DiagnosticsExporter.ExportAsync(
                configPath,
                archivePath).ConfigureAwait(false);
            Check.True(result.IncludedLog);
            Check.True(File.Exists(result.OutputPath));

            using var archive = ZipFile.OpenRead(archivePath);
            var names = archive.Entries
                .Select(static entry => entry.FullName)
                .OrderBy(static name => name, StringComparer.Ordinal)
                .ToArray();
            Check.SequenceEqual(
                System.Text.Encoding.UTF8.GetBytes(
                    "config.redacted.json|sidecar.jsonl|summary.json"),
                System.Text.Encoding.UTF8.GetBytes(string.Join('|', names)));
            foreach (var entry in archive.Entries)
            {
                using var reader = new StreamReader(entry.Open());
                var content = reader.ReadToEnd();
                Check.False(
                    content.Contains(token, StringComparison.Ordinal),
                    $"{entry.FullName} exposed the session token.");
            }

            var redactedEntry = archive.GetEntry("config.redacted.json");
            Check.NotNull(redactedEntry);
            using var redactedReader = new StreamReader(redactedEntry!.Open());
            Check.True(redactedReader.ReadToEnd().Contains(
                SecretRedactor.Replacement,
                StringComparison.Ordinal));
        }
        finally
        {
            DeleteOwnedSelfTestRoot(root);
        }
    }

    private static Task ConfigurationAsync()
    {
        var config = new SidecarConfig().Validate();
        Check.Equal(43120, config.TcpPort);
        Check.Equal(43121, config.UdpPort);
        Check.Near(200f, config.Bubble.WarningMeters);
        Check.Near(250f, config.Bubble.TeleportMeters);
        Check.Equal(20, config.Replication.SnapshotRateHz);
        Check.Equal(100, config.Replication.InterpolationDelayMs);
        Check.Equal(
            MotionReplicationMode.AnimGraphReplica,
            config.MotionReplicationMode);
        Check.Equal(5000, config.Network.DiagnosticsIntervalMs);
        Check.Equal("Player", config.Nickname);
        Check.True(config.Safety.StoryModeOnly);
        Check.True(config.Safety.RefuseOnlineMode);
        Check.Throws<ConfigurationException>(
            () => (config with
            {
                Bubble = new BubbleConfig
                {
                    WarningMeters = 250,
                    TeleportMeters = 200
                }
            }).Validate());
        Check.Throws<ConfigurationException>(
            () => (config with { HostAddress = "http://192.168.1.2:43120" })
                .Validate());
        Check.Throws<ConfigurationException>(
            () => (config with { HostAddress = "0.0.0.0" }).Validate());
        Check.Throws<ConfigurationException>(
            () => (config with { HostAddress = "192.168.1.999" }).Validate());
        Check.Throws<ConfigurationException>(
            () => (config with { Nickname = string.Empty }).Validate());
        Check.Throws<ConfigurationException>(
            () => (config with { Nickname = new string('x', 25) }).Validate());
        Check.Throws<ConfigurationException>(
            () => (config with
            {
                MotionReplicationMode = (MotionReplicationMode)99
            }).Validate());
        Check.Throws<ConfigurationException>(
            () => HostAddressValidator.Validate(
                "127.0.0.1",
                requireRemote: true));
        Check.Throws<ConfigurationException>(
            () => HostAddressValidator.Validate(
                "localhost",
                requireRemote: true));
        Check.Equal(
            "coop-host.lan",
            HostAddressValidator.Validate(
                "coop-host.lan",
                requireRemote: true));
        return Task.CompletedTask;
    }

    private static void DeleteOwnedSelfTestRoot(string root)
    {
        var resolvedRoot = Path.GetFullPath(root);
        var expectedParent = Path.GetFullPath(
            Path.Combine(Path.GetTempPath(), "CoopStory.SelfTest"));
        if (resolvedRoot.StartsWith(
                expectedParent + Path.DirectorySeparatorChar,
                StringComparison.OrdinalIgnoreCase) &&
            Directory.Exists(resolvedRoot))
        {
            Directory.Delete(resolvedRoot, recursive: true);
        }
    }

    private static int FindFreeTcpPort()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        try
        {
            listener.Start();
            return ((IPEndPoint)listener.LocalEndpoint).Port;
        }
        finally
        {
            listener.Stop();
        }
    }

    private static int FindFreeUdpPort()
    {
        using var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
        return ((IPEndPoint)socket.Client.LocalEndPoint!).Port;
    }

    private static bool LogContains(string path, string value)
    {
        if (!File.Exists(path))
        {
            return false;
        }

        try
        {
            using var stream = new FileStream(
                path,
                FileMode.Open,
                FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete);
            using var reader = new StreamReader(stream);
            return reader.ReadToEnd().Contains(value, StringComparison.Ordinal);
        }
        catch (IOException)
        {
            return false;
        }
    }

    private static async Task WaitUntilAsync(
        Func<bool> condition,
        TimeSpan timeout,
        string failureMessage)
    {
        var deadline = Stopwatch.StartNew();
        while (!condition())
        {
            if (deadline.Elapsed >= timeout)
            {
                throw new SelfTestException(failureMessage);
            }

            await Task.Delay(20).ConfigureAwait(false);
        }
    }

    private static async Task IgnoreCancellationAsync(params Task?[] tasks)
    {
        var activeTasks = tasks.Where(static task => task is not null).Cast<Task>().ToArray();
        if (activeTasks.Length == 0)
        {
            return;
        }

        try
        {
            await Task.WhenAll(activeTasks).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
        }
    }
}

internal sealed class GenerationBoundLanSession : ILanSession
{
    private readonly object _sync = new();
    private readonly Guid _sessionInstanceId = Guid.NewGuid();
    private readonly Dictionary<ulong, List<ProtocolEnvelope>>
        _controlsByGeneration = [];
    private ulong _generation = 1;
    private uint _sequence;
    private TaskCompletionSource<bool>? _blockedSendEntered;
    private TaskCompletionSource<bool>? _releaseBlockedSend;
    private bool _blockNextBoundSend;

    public bool IsConnected { get; private set; } = true;

    public System.Net.IPAddress? RemoteAddress =>
        IsConnected ? System.Net.IPAddress.Loopback : null;

    public event EnvelopeReceivedHandler? EnvelopeReceived
    {
        add { }
        remove { }
    }

    public event AuthenticationRejectedHandler? AuthenticationRejected
    {
        add { }
        remove { }
    }

    public event PeerConnectionChangedHandler? ConnectionChanged
    {
        add { }
        remove { }
    }

    public Task RunAsync(CancellationToken cancellationToken = default) =>
        Task.CompletedTask;

    public bool TryCaptureControlPeer(out ControlPeerToken peer)
    {
        lock (_sync)
        {
            if (!IsConnected)
            {
                peer = default;
                return false;
            }

            peer = new ControlPeerToken(
                _sessionInstanceId,
                _generation);
            return true;
        }
    }

    public bool IsControlPeerCurrent(ControlPeerToken peer)
    {
        lock (_sync)
        {
            return IsConnected &&
                peer.SessionInstanceId == _sessionInstanceId &&
                peer.Generation == _generation;
        }
    }

    public bool TryRunForControlPeer(
        ControlPeerToken peer,
        Action operation)
    {
        ArgumentNullException.ThrowIfNull(operation);
        lock (_sync)
        {
            if (!IsConnected ||
                peer.SessionInstanceId != _sessionInstanceId ||
                peer.Generation != _generation)
            {
                return false;
            }
            operation();
            return true;
        }
    }

    public ValueTask<bool> SendControlAsync(
        MessageType type,
        ReadOnlyMemory<byte> payload,
        ulong tick,
        CancellationToken cancellationToken = default)
    {
        if (!TryCaptureControlPeer(out var peer))
        {
            return ValueTask.FromResult(false);
        }

        return SendControlAsync(
            peer,
            type,
            payload,
            tick,
            cancellationToken);
    }

    public async ValueTask<bool> SendControlAsync(
        ControlPeerToken peer,
        MessageType type,
        ReadOnlyMemory<byte> payload,
        ulong tick,
        CancellationToken cancellationToken = default)
    {
        Task? waitForRelease = null;
        lock (_sync)
        {
            if (!IsConnected ||
                peer.SessionInstanceId != _sessionInstanceId ||
                peer.Generation != _generation)
            {
                return false;
            }

            if (_blockNextBoundSend)
            {
                _blockNextBoundSend = false;
                _blockedSendEntered!.TrySetResult(true);
                waitForRelease = _releaseBlockedSend!.Task;
            }
        }

        if (waitForRelease is not null)
        {
            await waitForRelease.WaitAsync(cancellationToken)
                .ConfigureAwait(false);
        }

        lock (_sync)
        {
            if (!_controlsByGeneration.TryGetValue(
                    peer.Generation,
                    out var controls))
            {
                controls = [];
                _controlsByGeneration.Add(peer.Generation, controls);
            }
            controls.Add(new ProtocolEnvelope(
                type,
                unchecked(++_sequence),
                tick,
                payload.ToArray()));
        }
        return true;
    }

    public ValueTask<bool> SendSnapshotAsync(
        MessageType type,
        ReadOnlyMemory<byte> payload,
        ulong tick,
        CancellationToken cancellationToken = default) =>
        ValueTask.FromResult(false);

    public void BlockNextBoundSend()
    {
        lock (_sync)
        {
            _blockNextBoundSend = true;
            _blockedSendEntered = new TaskCompletionSource<bool>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            _releaseBlockedSend = new TaskCompletionSource<bool>(
                TaskCreationOptions.RunContinuationsAsynchronously);
        }
    }

    public Task WaitForBlockedSendAsync()
    {
        lock (_sync)
        {
            return _blockedSendEntered?.Task ??
                throw new InvalidOperationException(
                    "No blocked send was configured.");
        }
    }

    public void ReleaseBlockedSend()
    {
        lock (_sync)
        {
            _releaseBlockedSend?.TrySetResult(true);
        }
    }

    public ControlPeerToken ReplacePeer()
    {
        lock (_sync)
        {
            _generation = _generation == ulong.MaxValue
                ? 1
                : _generation + 1;
            return new ControlPeerToken(
                _sessionInstanceId,
                _generation);
        }
    }

    public string DescribeControls(ulong generation)
    {
        lock (_sync)
        {
            return _controlsByGeneration.TryGetValue(
                    generation,
                    out var controls)
                ? string.Join(',', controls.Select(
                    static envelope =>
                        $"{envelope.Type}:{envelope.Tick}"))
                : string.Empty;
        }
    }

    public ValueTask DisposeAsync() => ValueTask.CompletedTask;
}

internal sealed class RecordingLanSession : ILanSession
{
    private uint _sequence;
    private readonly Guid _sessionInstanceId = Guid.NewGuid();
    private ulong _peerGeneration = 1;

    public bool IsConnected { get; set; }

    public System.Net.IPAddress? RemoteAddress =>
        IsConnected ? System.Net.IPAddress.Loopback : null;

    public List<ProtocolEnvelope> Controls { get; } = [];

    public event EnvelopeReceivedHandler? EnvelopeReceived
    {
        add { }
        remove { }
    }

    public event AuthenticationRejectedHandler? AuthenticationRejected
    {
        add { }
        remove { }
    }

    public event PeerConnectionChangedHandler? ConnectionChanged
    {
        add { }
        remove { }
    }

    public Task RunAsync(CancellationToken cancellationToken = default) =>
        Task.CompletedTask;

    public bool TryCaptureControlPeer(out ControlPeerToken peer)
    {
        if (!IsConnected)
        {
            peer = default;
            return false;
        }

        peer = new ControlPeerToken(
            _sessionInstanceId,
            _peerGeneration);
        return true;
    }

    public bool IsControlPeerCurrent(ControlPeerToken peer) =>
        IsConnected &&
        peer.SessionInstanceId == _sessionInstanceId &&
        peer.Generation == _peerGeneration;

    public bool TryRunForControlPeer(
        ControlPeerToken peer,
        Action operation)
    {
        ArgumentNullException.ThrowIfNull(operation);
        if (!IsControlPeerCurrent(peer))
        {
            return false;
        }
        operation();
        return true;
    }

    public ValueTask<bool> SendControlAsync(
        MessageType type,
        ReadOnlyMemory<byte> payload,
        ulong tick,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        Controls.Add(
            new ProtocolEnvelope(
                type,
                unchecked(++_sequence),
                tick,
                payload.ToArray()));
        return ValueTask.FromResult(true);
    }

    public ValueTask<bool> SendControlAsync(
        ControlPeerToken peer,
        MessageType type,
        ReadOnlyMemory<byte> payload,
        ulong tick,
        CancellationToken cancellationToken = default)
    {
        if (!IsControlPeerCurrent(peer))
        {
            return ValueTask.FromResult(false);
        }

        return SendControlAsync(
            type,
            payload,
            tick,
            cancellationToken);
    }

    public ValueTask<bool> SendSnapshotAsync(
        MessageType type,
        ReadOnlyMemory<byte> payload,
        ulong tick,
        CancellationToken cancellationToken = default) =>
        ValueTask.FromResult(false);

    public ValueTask DisposeAsync() => ValueTask.CompletedTask;
}

internal static class Check
{
    public static void True(bool condition, string? message = null)
    {
        if (!condition)
        {
            throw new SelfTestException(message ?? "Expected true, received false.");
        }
    }

    public static void False(bool condition, string? message = null) =>
        True(!condition, message ?? "Expected false, received true.");

    public static void Null(object? value)
    {
        if (value is not null)
        {
            throw new SelfTestException("Expected null.");
        }
    }

    public static void NotNull(object? value)
    {
        if (value is null)
        {
            throw new SelfTestException("Expected non-null value.");
        }
    }

    public static void Equal<T>(T expected, T actual)
        where T : notnull
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
        {
            throw new SelfTestException($"Expected '{expected}', received '{actual}'.");
        }
    }

    public static void Near(float expected, float actual, float tolerance = 0.0001f)
    {
        if (MathF.Abs(expected - actual) > tolerance)
        {
            throw new SelfTestException($"Expected {expected}, received {actual}.");
        }
    }

    public static void SequenceEqual(ReadOnlySpan<byte> expected, ReadOnlySpan<byte> actual)
    {
        if (!expected.SequenceEqual(actual))
        {
            throw new SelfTestException("Byte sequences differ.");
        }
    }

    public static void Throws<TException>(Action action)
        where TException : Exception
    {
        try
        {
            action();
        }
        catch (TException)
        {
            return;
        }

        throw new SelfTestException($"Expected {typeof(TException).Name}.");
    }

    public static async Task ThrowsAsync<TException>(Func<Task> action)
        where TException : Exception
    {
        try
        {
            await action().ConfigureAwait(false);
        }
        catch (TException)
        {
            return;
        }

        throw new SelfTestException($"Expected {typeof(TException).Name}.");
    }
}

internal sealed class SelfTestException : Exception
{
    public SelfTestException(string message)
        : base(message)
    {
    }
}
