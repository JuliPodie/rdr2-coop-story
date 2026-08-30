using System.Buffers.Binary;
using System.Numerics;
using System.Security.Cryptography;
using System.Text;

namespace CoopStory.Protocol;

public static class BinaryPayloadCodec
{
    public const int PlayerStateSize = 104;
    public const int PlayerTraversalSize = 88;
    public const int PlayerActionSize = 88;
    public const int PlayerIdentityHeaderSize = 10;
    public const int PlayerIdentityMaximumSize =
        PlayerIdentityHeaderSize +
        PlayerIdentityRules.MaximumNicknameUtf8Bytes;
    public const int PlayerAppearanceStateHeaderSize = 32;
    public const int PlayerAppearanceMaximumComponents = 64;
    public const int PlayerMountStateSize = 60;
    public const int WorldEntityStateSize = 76;
    public const int EntityDespawnSize = 8;
    public const int DamageIntentSize = 32;
    public const int WorldStateSize = 24;
    public const int MissionStateSize = 48;
    public const int MissionCinematicStateSize = 48;
    public const int MissionCinematicActionSize = 32;
    public const int AnimSceneReplicaStateSize = 72;
    public const int AnimSceneDefinitionHeaderSize = 60;
    public const int AnimSceneRoleBindingHeaderSize = 20;
    public const int AnimSceneDefinitionMaximumSize = 8192;
    public const int AnimSceneDefinitionMaximumRoles = 48;
    public const int AnimSceneResourceMaximumBytes = 256;
    public const int AnimScenePlaybackListMaximumBytes = 128;
    public const int AnimSceneRoleNameMaximumBytes = 64;
    public const int AnimSceneControlSize = 64;
    public const int MissionCameraStateSize = 56;
    public const int EquipmentStateSize = 24;
    public const int PauseVoteSize = 12;
    public const int CommandSize = 32;
    public const int DownedStateSize = 16;
    public const int ReviveRequestSize = 16;
    public const int ReviveCompleteSize = 20;
    public const int InteractionIntentSize = 40;
    public const int InteractionResultSize = 44;
    public const int RestraintStateSize = 28;
    public const int CampaignCapabilitySize = 24;
    public const int CampaignCapabilityAckSize = 16;
    public const int PickupCollectedSize = 24;
    public const int MissionProgressionSize = 24;
    public const int MissionObjectiveHeaderSize = 28;
    public const int MaximumMissionObjectiveUtf8Bytes = 192;
    public const int MissionDialogueCueSize = 40;
    public const int MissionDialogueReadySize = 32;
    public const int AmbientEncounterProposalSize = 48;
    public const int AmbientEncounterStateSize = 56;

    public static byte[] EncodeMissionObjective(MissionObjectivePayload payload)
    {
        ValidateMissionObjective(payload);
        var text = Encoding.UTF8.GetBytes(payload.Text);
        var bytes = new byte[MissionObjectiveHeaderSize + text.Length];
        BinaryPrimitives.WriteUInt64LittleEndian(bytes, payload.HostEntityId.Value);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(8), payload.MissionEpoch);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(12), payload.Revision);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(16), payload.Fingerprint);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(24), checked((ushort)text.Length));
        // bytes 26..27 reserved and left zero.
        text.CopyTo(bytes.AsSpan(MissionObjectiveHeaderSize));
        return bytes;
    }

    public static MissionObjectivePayload DecodeMissionObjective(ReadOnlySpan<byte> payload)
    {
        if (payload.Length < MissionObjectiveHeaderSize)
            throw new ProtocolException("Mission objective payload is truncated.");
        var textLength = BinaryPrimitives.ReadUInt16LittleEndian(payload[24..]);
        if (payload[26] != 0 || payload[27] != 0 ||
            textLength == 0 || textLength > MaximumMissionObjectiveUtf8Bytes ||
            payload.Length != MissionObjectiveHeaderSize + textLength)
            throw new ProtocolException("Mission objective payload length is invalid.");
        var result = new MissionObjectivePayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[8..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[12..]),
            BinaryPrimitives.ReadUInt64LittleEndian(payload[16..]),
            Encoding.UTF8.GetString(payload[MissionObjectiveHeaderSize..]));
        ValidateMissionObjective(result);
        return result;
    }

    public static byte[] EncodeMissionDialogueCue(MissionDialogueCuePayload payload)
    {
        ValidateMissionDialogueCue(payload);
        var bytes = new byte[MissionDialogueCueSize];
        BinaryPrimitives.WriteUInt64LittleEndian(bytes, payload.HostEntityId.Value);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(8), payload.MissionEpoch);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(12), payload.CheckpointGeneration);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(16), payload.DialogueSequence);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(20), payload.ProfileId);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(24), payload.RootId);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(28), payload.LineIndex);
        // bytes 30..31 are reserved and left zero.
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(32), payload.HostStartTick);
        return bytes;
    }

    public static MissionDialogueCuePayload DecodeMissionDialogueCue(ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, MissionDialogueCueSize, nameof(MissionDialogueCuePayload));
        if (BinaryPrimitives.ReadUInt16LittleEndian(payload[30..]) != 0)
            throw new ProtocolException("Mission dialogue cue reserved bytes must be zero.");
        var result = new MissionDialogueCuePayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[8..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[12..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[16..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[20..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[24..]),
            BinaryPrimitives.ReadUInt16LittleEndian(payload[28..]),
            BinaryPrimitives.ReadUInt64LittleEndian(payload[32..]));
        ValidateMissionDialogueCue(result);
        return result;
    }

    public static byte[] EncodeMissionDialogueReady(MissionDialogueReadyPayload payload)
    {
        ValidateMissionDialogueReady(payload);
        var bytes = new byte[MissionDialogueReadySize];
        BinaryPrimitives.WriteUInt64LittleEndian(bytes, payload.HostEntityId.Value);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(8), payload.MissionEpoch);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(12), payload.CheckpointGeneration);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(16), payload.DialogueSequence);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(20), payload.ProfileId);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(24), payload.RootId);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(28), payload.LineIndex);
        bytes[30] = (byte)payload.State;
        // byte 31 is reserved and left zero.
        return bytes;
    }

    public static MissionDialogueReadyPayload DecodeMissionDialogueReady(ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, MissionDialogueReadySize, nameof(MissionDialogueReadyPayload));
        if (payload[31] != 0)
            throw new ProtocolException("Mission dialogue readiness reserved byte must be zero.");
        var result = new MissionDialogueReadyPayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[8..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[12..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[16..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[20..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[24..]),
            BinaryPrimitives.ReadUInt16LittleEndian(payload[28..]),
            (MissionDialogueReadyState)payload[30]);
        ValidateMissionDialogueReady(result);
        return result;
    }

    public static byte[] EncodeAmbientEncounterProposal(AmbientEncounterProposalPayload payload)
    {
        ValidateAmbientEncounterProposal(payload);
        var bytes = new byte[AmbientEncounterProposalSize];
        BinaryPrimitives.WriteUInt64LittleEndian(bytes, payload.GuestEntityId.Value);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(8), payload.ProposalId);
        bytes[16] = (byte)payload.Profile;
        WriteVector3(bytes.AsSpan(20), payload.Anchor);
        BinaryPrimitives.WriteSingleLittleEndian(bytes.AsSpan(32), payload.RadiusMeters);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(36), payload.LocalEvidenceHash);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(40), payload.SuggestedRosterSeed);
        return bytes;
    }

    public static AmbientEncounterProposalPayload DecodeAmbientEncounterProposal(ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, AmbientEncounterProposalSize, nameof(AmbientEncounterProposalPayload));
        if (payload[17] != 0 || BinaryPrimitives.ReadUInt16LittleEndian(payload[18..]) != 0 ||
            BinaryPrimitives.ReadUInt32LittleEndian(payload[44..]) != 0)
            throw new ProtocolException("Ambient encounter proposal reserved bytes must be zero.");
        var result = new AmbientEncounterProposalPayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            BinaryPrimitives.ReadUInt64LittleEndian(payload[8..]), (AmbientEncounterProfile)payload[16],
            ReadVector3(payload[20..]), BinaryPrimitives.ReadSingleLittleEndian(payload[32..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[36..]), BinaryPrimitives.ReadUInt32LittleEndian(payload[40..]));
        ValidateAmbientEncounterProposal(result);
        return result;
    }

    public static byte[] EncodeAmbientEncounterState(AmbientEncounterStatePayload payload)
    {
        ValidateAmbientEncounterState(payload);
        var bytes = new byte[AmbientEncounterStateSize];
        BinaryPrimitives.WriteUInt64LittleEndian(bytes, payload.HostEntityId.Value);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(8), payload.InstanceId);
        bytes[16] = (byte)payload.Profile; bytes[17] = (byte)payload.Phase; bytes[18] = (byte)payload.Rejection;
        bytes[19] = (byte)payload.GuestDisposition;
        WriteVector3(bytes.AsSpan(20), payload.Anchor);
        BinaryPrimitives.WriteSingleLittleEndian(bytes.AsSpan(32), payload.RadiusMeters);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(36), payload.RosterSeed);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(40), payload.RosterCount);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(44), payload.HostStartTick);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(52), payload.ExactEventId);
        return bytes;
    }

    public static AmbientEncounterStatePayload DecodeAmbientEncounterState(ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, AmbientEncounterStateSize, nameof(AmbientEncounterStatePayload));
        if (BinaryPrimitives.ReadUInt16LittleEndian(payload[42..]) != 0 ||
            payload[19] > (byte)AmbientEncounterPeerDisposition.Companion)
            throw new ProtocolException("Ambient encounter state reserved bytes must be zero.");
        var result = new AmbientEncounterStatePayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)), BinaryPrimitives.ReadUInt64LittleEndian(payload[8..]),
            (AmbientEncounterProfile)payload[16], (AmbientEncounterPhase)payload[17], (AmbientEncounterRejection)payload[18],
            ReadVector3(payload[20..]), BinaryPrimitives.ReadSingleLittleEndian(payload[32..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[36..]), BinaryPrimitives.ReadUInt16LittleEndian(payload[40..]),
            BinaryPrimitives.ReadUInt64LittleEndian(payload[44..]), BinaryPrimitives.ReadUInt32LittleEndian(payload[52..]),
            (AmbientEncounterPeerDisposition)payload[19]);
        ValidateAmbientEncounterState(result);
        return result;
    }

    public static byte[] EncodeMissionProgression(MissionProgressionPayload payload)
    {
        ValidateMissionProgression(payload);
        var bytes = new byte[MissionProgressionSize];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, payload.MissionId);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(4), payload.MissionEpoch);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(8), payload.EventId);
        bytes[16] = (byte)payload.Phase;
        bytes[17] = (byte)payload.Flags;
        bytes[18] = payload.CompletionRating;
        BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(20), payload.CompletionCashAward);
        return bytes;
    }

    public static MissionProgressionPayload DecodeMissionProgression(ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, MissionProgressionSize, nameof(MissionProgressionPayload));
        if (payload[19] != 0)
            throw new ProtocolException("Mission progression reserved bytes must be zero.");
        var result = new MissionProgressionPayload(
            BinaryPrimitives.ReadUInt32LittleEndian(payload),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[4..]),
            BinaryPrimitives.ReadUInt64LittleEndian(payload[8..]),
            (MissionProgressionPhase)payload[16],
            (MissionProgressionFlags)payload[17],
            payload[18],
            BinaryPrimitives.ReadInt32LittleEndian(payload[20..]));
        ValidateMissionProgression(result);
        return result;
    }

    public static byte[] EncodePickupCollected(PickupCollectedPayload payload)
    {
        ValidatePickupCollected(payload);
        var bytes = new byte[PickupCollectedSize];
        BinaryPrimitives.WriteUInt64LittleEndian(bytes, payload.ActorEntityId.Value);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(8), payload.CollectionId);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(16), payload.PickupHash);
        return bytes;
    }

    public static PickupCollectedPayload DecodePickupCollected(ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, PickupCollectedSize, nameof(PickupCollectedPayload));
        if (BinaryPrimitives.ReadUInt32LittleEndian(payload[20..]) != 0)
            throw new ProtocolException("Pickup collection reserved bytes must be zero.");
        var result = new PickupCollectedPayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            BinaryPrimitives.ReadUInt64LittleEndian(payload[8..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[16..]));
        ValidatePickupCollected(result);
        return result;
    }

    public static byte[] EncodeCampaignCapabilityAck(CampaignCapabilityAckPayload payload)
    {
        ValidateCampaignCapabilityAck(payload);
        var bytes = new byte[CampaignCapabilityAckSize];
        bytes[0] = (byte)payload.Kind;
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(4), payload.RecordHash);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(8), payload.HostEventId);
        return bytes;
    }

    public static CampaignCapabilityAckPayload DecodeCampaignCapabilityAck(ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, CampaignCapabilityAckSize, nameof(CampaignCapabilityAckPayload));
        var result = new CampaignCapabilityAckPayload((CampaignCapabilityKind)payload[0], BinaryPrimitives.ReadUInt32LittleEndian(payload[4..]), BinaryPrimitives.ReadUInt64LittleEndian(payload[8..]));
        ValidateCampaignCapabilityAck(result);
        return result;
    }

    public static byte[] EncodeCampaignCapability(CampaignCapabilityPayload payload)
    {
        ValidateCampaignCapability(payload);
        var bytes = new byte[CampaignCapabilitySize];
        bytes[0] = (byte)payload.Kind;
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(4), payload.RecordHash);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(8), payload.HostEventId);
        BinaryPrimitives.WriteInt64LittleEndian(bytes.AsSpan(16), payload.GrantedAtUnixMilliseconds);
        return bytes;
    }

    public static CampaignCapabilityPayload DecodeCampaignCapability(ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, CampaignCapabilitySize, nameof(CampaignCapabilityPayload));
        var result = new CampaignCapabilityPayload((CampaignCapabilityKind)payload[0], BinaryPrimitives.ReadUInt32LittleEndian(payload[4..]), BinaryPrimitives.ReadUInt64LittleEndian(payload[8..]), BinaryPrimitives.ReadInt64LittleEndian(payload[16..]));
        ValidateCampaignCapability(result);
        return result;
    }

    public static byte[] EncodePlayerState(PlayerStatePayload payload)
    {
        ValidatePlayerState(payload);
        var bytes = new byte[PlayerStateSize];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(span, payload.EntityId.Value);
        span[8] = payload.Slot;
        span[9] = (byte)payload.Lifecycle;
        WriteVector3(span[12..], payload.Position);
        WriteVector3(span[24..], payload.Velocity);
        BinaryPrimitives.WriteSingleLittleEndian(span[36..], payload.Heading);
        BinaryPrimitives.WriteSingleLittleEndian(span[40..], payload.HealthFraction);
        BinaryPrimitives.WriteUInt32LittleEndian(span[44..], (uint)payload.Flags);
        WriteVector3(span[48..], payload.AimTarget);
        BinaryPrimitives.WriteUInt32LittleEndian(span[60..], payload.FireSequence);
        BinaryPrimitives.WriteSingleLittleEndian(span[64..], payload.MovementHeading);
        BinaryPrimitives.WriteSingleLittleEndian(span[68..], payload.LocalForwardSpeed);
        BinaryPrimitives.WriteSingleLittleEndian(span[72..], payload.LocalRightSpeed);
        BinaryPrimitives.WriteSingleLittleEndian(span[76..], payload.DesiredMoveBlend);
        BinaryPrimitives.WriteUInt16LittleEndian(span[80..], payload.LocomotionEpoch);
        BinaryPrimitives.WriteUInt16LittleEndian(span[82..], payload.TraversalActionId);
        span[84] = (byte)payload.TraversalKind;
        span[85] = (byte)payload.LocomotionMode;
        WriteVector3(span[88..], payload.TraversalAnchor);
        BinaryPrimitives.WriteSingleLittleEndian(span[100..], payload.TraversalHeading);
        return bytes;
    }

    public static PlayerStatePayload DecodePlayerState(ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, PlayerStateSize, nameof(PlayerStatePayload));
        var result = new PlayerStatePayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            payload[8],
            (PlayerLifecycle)payload[9],
            ReadVector3(payload[12..]),
            ReadVector3(payload[24..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[36..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[40..]),
            (PlayerStateFlags)BinaryPrimitives.ReadUInt32LittleEndian(payload[44..]),
            ReadVector3(payload[48..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[60..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[64..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[68..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[72..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[76..]),
            BinaryPrimitives.ReadUInt16LittleEndian(payload[80..]),
            BinaryPrimitives.ReadUInt16LittleEndian(payload[82..]),
            (PlayerTraversalKind)payload[84],
            (PlayerLocomotionMode)payload[85],
            ReadVector3(payload[88..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[100..]));
        ValidatePlayerState(result);
        if (BinaryPrimitives.ReadUInt16LittleEndian(payload[86..]) != 0)
        {
            throw new ProtocolException(
                "Player state semantic reserved bytes must be zero.");
        }
        return result;
    }

    public static byte[] EncodePlayerTraversal(PlayerTraversalPayload payload)
    {
        ValidatePlayerTraversal(payload);
        var bytes = new byte[PlayerTraversalSize];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(span, payload.EntityId.Value);
        span[8] = payload.Slot;
        span[9] = (byte)payload.Kind;
        BinaryPrimitives.WriteUInt16LittleEndian(span[10..], payload.ActionId);
        BinaryPrimitives.WriteUInt16LittleEndian(span[12..], payload.Revision);
        BinaryPrimitives.WriteUInt16LittleEndian(span[14..], payload.LocomotionEpoch);
        BinaryPrimitives.WriteUInt32LittleEndian(span[16..], (uint)payload.Flags);
        BinaryPrimitives.WriteSingleLittleEndian(span[20..], payload.TakeoffHeading);
        WriteVector3(span[24..], payload.TakeoffPosition);
        WriteVector3(span[36..], payload.ApproachVelocity);
        WriteVector3(span[48..], payload.ObstaclePoint);
        WriteVector3(span[60..], payload.ObstacleNormal);
        BinaryPrimitives.WriteSingleLittleEndian(span[72..], payload.ObstacleTopZ);
        WriteVector3(span[76..], payload.ExpectedLanding);
        return bytes;
    }

    public static PlayerTraversalPayload DecodePlayerTraversal(
        ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, PlayerTraversalSize, nameof(PlayerTraversalPayload));
        var result = new PlayerTraversalPayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            payload[8],
            (PlayerTraversalKind)payload[9],
            BinaryPrimitives.ReadUInt16LittleEndian(payload[10..]),
            BinaryPrimitives.ReadUInt16LittleEndian(payload[12..]),
            BinaryPrimitives.ReadUInt16LittleEndian(payload[14..]),
            (PlayerTraversalFlags)BinaryPrimitives.ReadUInt32LittleEndian(payload[16..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[20..]),
            ReadVector3(payload[24..]),
            ReadVector3(payload[36..]),
            ReadVector3(payload[48..]),
            ReadVector3(payload[60..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[72..]),
            ReadVector3(payload[76..]));
        ValidatePlayerTraversal(result);
        return result;
    }

    public static byte[] EncodePlayerAction(PlayerActionPayload payload)
    {
        ValidatePlayerAction(payload);
        var bytes = new byte[PlayerActionSize];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(
            span,
            payload.ActorEntityId.Value);
        BinaryPrimitives.WriteUInt64LittleEndian(
            span[8..],
            payload.TargetEntityId.Value);
        BinaryPrimitives.WriteUInt32LittleEndian(span[16..], payload.Sequence);
        BinaryPrimitives.WriteUInt32LittleEndian(span[20..], payload.ActionId);
        BinaryPrimitives.WriteUInt16LittleEndian(span[24..], payload.Revision);
        span[26] = payload.ActorSlot;
        span[27] = payload.AuthoritySlot;
        span[28] = (byte)payload.Kind;
        span[29] = (byte)payload.Phase;
        BinaryPrimitives.WriteUInt16LittleEndian(span[30..], 0);
        BinaryPrimitives.WriteUInt32LittleEndian(span[32..], (uint)payload.Flags);
        BinaryPrimitives.WriteUInt32LittleEndian(
            span[36..],
            payload.DurationMilliseconds);
        BinaryPrimitives.WriteUInt32LittleEndian(
            span[40..],
            payload.PhaseElapsedMilliseconds);
        BinaryPrimitives.WriteUInt32LittleEndian(span[44..], payload.WeaponHash);
        BinaryPrimitives.WriteUInt32LittleEndian(span[48..], payload.VariantHash);
        BinaryPrimitives.WriteUInt32LittleEndian(
            span[52..],
            payload.AnimationSampleSequence);
        WriteVector3(span[56..], payload.ActorAnchor);
        WriteVector3(span[68..], payload.TargetPoint);
        BinaryPrimitives.WriteSingleLittleEndian(span[80..], payload.FacingHeading);
        BinaryPrimitives.WriteSingleLittleEndian(span[84..], payload.NormalizedPhase);
        return bytes;
    }

    public static PlayerActionPayload DecodePlayerAction(
        ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, PlayerActionSize, nameof(PlayerActionPayload));
        if (BinaryPrimitives.ReadUInt16LittleEndian(payload[30..]) != 0)
        {
            throw new ProtocolException(
                "Player action reserved field must be zero.");
        }

        var result = new PlayerActionPayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload[8..])),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[16..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[20..]),
            BinaryPrimitives.ReadUInt16LittleEndian(payload[24..]),
            payload[26],
            payload[27],
            (PlayerActionKind)payload[28],
            (PlayerActionPhase)payload[29],
            (PlayerActionFlags)BinaryPrimitives.ReadUInt32LittleEndian(payload[32..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[36..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[40..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[44..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[48..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[52..]),
            ReadVector3(payload[56..]),
            ReadVector3(payload[68..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[80..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[84..]));
        ValidatePlayerAction(result);
        return result;
    }

    public static byte[] EncodeInteractionIntent(
        InteractionIntentPayload payload)
    {
        ValidateInteractionIntent(payload);
        var bytes = new byte[InteractionIntentSize];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(span, payload.ActorEntityId.Value);
        BinaryPrimitives.WriteUInt64LittleEndian(span[8..], payload.TargetEntityId.Value);
        BinaryPrimitives.WriteUInt64LittleEndian(span[16..], payload.SecondaryEntityId.Value);
        BinaryPrimitives.WriteUInt32LittleEndian(span[24..], payload.InteractionId);
        BinaryPrimitives.WriteUInt16LittleEndian(span[28..], payload.Revision);
        span[30] = payload.ActorSlot;
        span[31] = (byte)payload.Kind;
        span[32] = (byte)payload.Phase;
        span[33] = (byte)payload.Flags;
        BinaryPrimitives.WriteUInt16LittleEndian(span[34..], 0);
        BinaryPrimitives.WriteUInt32LittleEndian(
            span[36..],
            payload.RequestedDurationMilliseconds);
        return bytes;
    }

    public static InteractionIntentPayload DecodeInteractionIntent(
        ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, InteractionIntentSize, nameof(InteractionIntentPayload));
        if (BinaryPrimitives.ReadUInt16LittleEndian(payload[34..]) != 0)
        {
            throw new ProtocolException(
                "Interaction intent reserved bytes must be zero.");
        }
        var result = new InteractionIntentPayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload[8..])),
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload[16..])),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[24..]),
            BinaryPrimitives.ReadUInt16LittleEndian(payload[28..]),
            payload[30],
            (InteractionKind)payload[31],
            (InteractionIntentPhase)payload[32],
            (InteractionIntentFlags)payload[33],
            BinaryPrimitives.ReadUInt32LittleEndian(payload[36..]));
        ValidateInteractionIntent(result);
        return result;
    }

    public static byte[] EncodeInteractionResult(
        InteractionResultPayload payload)
    {
        ValidateInteractionResult(payload);
        var bytes = new byte[InteractionResultSize];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(span, payload.ActorEntityId.Value);
        BinaryPrimitives.WriteUInt64LittleEndian(span[8..], payload.TargetEntityId.Value);
        BinaryPrimitives.WriteUInt64LittleEndian(span[16..], payload.SecondaryEntityId.Value);
        BinaryPrimitives.WriteUInt32LittleEndian(span[24..], payload.InteractionId);
        BinaryPrimitives.WriteUInt16LittleEndian(span[28..], payload.Revision);
        span[30] = (byte)payload.Kind;
        span[31] = (byte)payload.Status;
        span[32] = (byte)payload.RejectReason;
        span[33] = 0;
        BinaryPrimitives.WriteUInt16LittleEndian(span[34..], (ushort)payload.Flags);
        BinaryPrimitives.WriteUInt32LittleEndian(span[36..], payload.ProgressMilliseconds);
        BinaryPrimitives.WriteUInt32LittleEndian(span[40..], payload.RequiredDurationMilliseconds);
        return bytes;
    }

    public static InteractionResultPayload DecodeInteractionResult(
        ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, InteractionResultSize, nameof(InteractionResultPayload));
        if (payload[33] != 0)
        {
            throw new ProtocolException(
                "Interaction result reserved byte must be zero.");
        }
        var result = new InteractionResultPayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload[8..])),
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload[16..])),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[24..]),
            BinaryPrimitives.ReadUInt16LittleEndian(payload[28..]),
            (InteractionKind)payload[30],
            (InteractionResultStatus)payload[31],
            (InteractionRejectReason)payload[32],
            (InteractionResultFlags)BinaryPrimitives.ReadUInt16LittleEndian(payload[34..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[36..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[40..]));
        ValidateInteractionResult(result);
        return result;
    }

    public static byte[] EncodeRestraintState(RestraintStatePayload payload)
    {
        ValidateRestraintState(payload);
        var bytes = new byte[RestraintStateSize];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(span, payload.SubjectEntityId.Value);
        BinaryPrimitives.WriteUInt64LittleEndian(span[8..], payload.OwnerEntityId.Value);
        BinaryPrimitives.WriteUInt32LittleEndian(span[16..], payload.SourceInteractionId);
        BinaryPrimitives.WriteUInt32LittleEndian(span[20..], payload.Revision);
        span[24] = (byte)payload.State;
        span[25] = (byte)payload.Flags;
        BinaryPrimitives.WriteUInt16LittleEndian(span[26..], 0);
        return bytes;
    }

    public static RestraintStatePayload DecodeRestraintState(
        ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, RestraintStateSize, nameof(RestraintStatePayload));
        if (BinaryPrimitives.ReadUInt16LittleEndian(payload[26..]) != 0)
        {
            throw new ProtocolException(
                "Restraint state reserved bytes must be zero.");
        }
        var result = new RestraintStatePayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload[8..])),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[16..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[20..]),
            (PlayerRestraintState)payload[24],
            (RestraintStateFlags)payload[25]);
        ValidateRestraintState(result);
        return result;
    }

    public static byte[] EncodePlayerIdentity(PlayerIdentityPayload payload)
    {
        ValidatePlayerIdentity(payload);
        var nickname = PlayerIdentityRules.EncodeUtf8(payload.Nickname);
        var bytes = new byte[PlayerIdentityHeaderSize + nickname.Length];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(span, payload.EntityId.Value);
        span[8] = payload.Slot;
        span[9] = checked((byte)nickname.Length);
        nickname.CopyTo(span[PlayerIdentityHeaderSize..]);
        return bytes;
    }

    public static PlayerIdentityPayload DecodePlayerIdentity(
        ReadOnlySpan<byte> payload)
    {
        if (payload.Length < PlayerIdentityHeaderSize)
        {
            throw new ProtocolException(
                $"{nameof(PlayerIdentityPayload)} payload must contain at least " +
                $"{PlayerIdentityHeaderSize} bytes; received {payload.Length}.");
        }

        var nicknameLength = payload[9];
        var expectedLength = PlayerIdentityHeaderSize + nicknameLength;
        if (payload.Length != expectedLength)
        {
            throw new ProtocolException(
                $"{nameof(PlayerIdentityPayload)} nickname length is " +
                $"{nicknameLength} bytes, but the payload contains " +
                $"{payload.Length - PlayerIdentityHeaderSize} bytes.");
        }

        var result = new PlayerIdentityPayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            payload[8],
            PlayerIdentityRules.DecodeUtf8(
                payload[PlayerIdentityHeaderSize..]));
        ValidatePlayerIdentity(result);
        return result;
    }

    public static byte[] EncodePlayerAppearanceState(
        PlayerAppearanceStatePayload payload)
    {
        ValidatePlayerAppearanceState(payload);
        var bytes = new byte[
            PlayerAppearanceStateHeaderSize +
            payload.ComponentHashes.Length * sizeof(uint)];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(span, payload.EntityId.Value);
        span[8] = payload.Slot;
        span[9] = payload.SchemaVersion;
        BinaryPrimitives.WriteUInt16LittleEndian(span[10..], (ushort)payload.Flags);
        BinaryPrimitives.WriteUInt32LittleEndian(span[12..], payload.Revision);
        BinaryPrimitives.WriteUInt32LittleEndian(span[16..], payload.ModelHash);
        BinaryPrimitives.WriteUInt16LittleEndian(
            span[20..], checked((ushort)payload.ComponentHashes.Length));
        BinaryPrimitives.WriteUInt16LittleEndian(span[22..], 0);
        BinaryPrimitives.WriteUInt64LittleEndian(span[24..], payload.Fingerprint);
        for (var index = 0; index < payload.ComponentHashes.Length; index++)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(
                span[(PlayerAppearanceStateHeaderSize + index * sizeof(uint))..],
                payload.ComponentHashes[index]);
        }
        return bytes;
    }

    public static PlayerAppearanceStatePayload DecodePlayerAppearanceState(
        ReadOnlySpan<byte> payload)
    {
        if (payload.Length < PlayerAppearanceStateHeaderSize ||
            payload.Length > PlayerAppearanceStateHeaderSize +
                PlayerAppearanceMaximumComponents * sizeof(uint))
        {
            throw new ProtocolException(
                "Player appearance payload length is outside the supported range.");
        }
        var componentCount = BinaryPrimitives.ReadUInt16LittleEndian(payload[20..]);
        if (BinaryPrimitives.ReadUInt16LittleEndian(payload[22..]) != 0 ||
            componentCount == 0 ||
            componentCount > PlayerAppearanceMaximumComponents ||
            payload.Length != PlayerAppearanceStateHeaderSize +
                componentCount * sizeof(uint))
        {
            throw new ProtocolException(
                "Player appearance component count or reserved field is invalid.");
        }
        var components = new uint[componentCount];
        for (var index = 0; index < components.Length; index++)
        {
            components[index] = BinaryPrimitives.ReadUInt32LittleEndian(
                payload[(PlayerAppearanceStateHeaderSize + index * sizeof(uint))..]);
        }
        var result = new PlayerAppearanceStatePayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            payload[8],
            payload[9],
            (PlayerAppearanceStateFlags)BinaryPrimitives.ReadUInt16LittleEndian(
                payload[10..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[12..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[16..]),
            BinaryPrimitives.ReadUInt64LittleEndian(payload[24..]),
            components);
        ValidatePlayerAppearanceState(result);
        return result;
    }

    public static byte[] EncodePlayerMountState(
        PlayerMountStatePayload payload)
    {
        ValidatePlayerMountState(payload);
        var bytes = new byte[PlayerMountStateSize];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(
            span,
            payload.PlayerEntityId.Value);
        BinaryPrimitives.WriteUInt64LittleEndian(
            span[8..],
            payload.MountEntityId.Value);
        span[16] = payload.Slot;
        span[17] = (byte)payload.Flags;
        BinaryPrimitives.WriteUInt16LittleEndian(span[18..], 0);
        BinaryPrimitives.WriteUInt32LittleEndian(span[20..], payload.ModelHash);
        WriteVector3(span[24..], payload.Position);
        WriteVector3(span[36..], payload.Velocity);
        BinaryPrimitives.WriteSingleLittleEndian(span[48..], payload.Heading);
        BinaryPrimitives.WriteSingleLittleEndian(
            span[52..],
            payload.HealthFraction);
        BinaryPrimitives.WriteUInt32LittleEndian(span[56..], payload.Generation);
        return bytes;
    }

    public static PlayerMountStatePayload DecodePlayerMountState(
        ReadOnlySpan<byte> payload)
    {
        RequireLength(
            payload,
            PlayerMountStateSize,
            nameof(PlayerMountStatePayload));
        if (BinaryPrimitives.ReadUInt16LittleEndian(payload[18..]) != 0)
        {
            throw new ProtocolException(
                "Player mount state reserved field must be zero.");
        }

        var result = new PlayerMountStatePayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload[8..])),
            payload[16],
            (PlayerMountStateFlags)payload[17],
            BinaryPrimitives.ReadUInt32LittleEndian(payload[20..]),
            ReadVector3(payload[24..]),
            ReadVector3(payload[36..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[48..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[52..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[56..]));
        ValidatePlayerMountState(result);
        return result;
    }

    public static byte[] EncodeWorldEntityState(
        WorldEntityStatePayload payload)
    {
        ValidateWorldEntityState(payload);
        var bytes = new byte[WorldEntityStateSize];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(span, payload.EntityId.Value);
        BinaryPrimitives.WriteUInt32LittleEndian(span[8..], payload.ModelHash);
        span[12] = (byte)payload.Kind;
        span[13] = (byte)payload.Flags;
        span[14] = (byte)payload.CombatTargetSlot;
        span[15] = (byte)payload.TaskKind;
        WriteVector3(span[16..], payload.Position);
        WriteVector3(span[28..], payload.Velocity);
        BinaryPrimitives.WriteSingleLittleEndian(span[40..], payload.Heading);
        BinaryPrimitives.WriteSingleLittleEndian(
            span[44..],
            payload.HealthFraction);
        BinaryPrimitives.WriteUInt32LittleEndian(span[48..], payload.WeaponHash);
        BinaryPrimitives.WriteUInt64LittleEndian(
            span[52..],
            payload.ParentEntityId.Value);
        WriteVector3(span[60..], payload.TaskTarget);
        BinaryPrimitives.WriteUInt32LittleEndian(span[72..], 0);
        return bytes;
    }

    public static WorldEntityStatePayload DecodeWorldEntityState(
        ReadOnlySpan<byte> payload)
    {
        RequireLength(
            payload,
            WorldEntityStateSize,
            nameof(WorldEntityStatePayload));
        if (BinaryPrimitives.ReadUInt32LittleEndian(payload[72..]) != 0)
        {
            throw new ProtocolException(
                "World entity state reserved field must be zero.");
        }

        var result = new WorldEntityStatePayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[8..]),
            (WorldEntityKind)payload[12],
            (WorldEntityStateFlags)payload[13],
            (WorldCombatTargetSlot)payload[14],
            ReadVector3(payload[16..]),
            ReadVector3(payload[28..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[40..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[44..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[48..]),
            (WorldTaskKind)payload[15],
            new NetEntityId(
                BinaryPrimitives.ReadUInt64LittleEndian(payload[52..])),
            ReadVector3(payload[60..]));
        ValidateWorldEntityState(result);
        return result;
    }

    public static byte[] EncodeEntityDespawn(EntityDespawnPayload payload)
    {
        ValidateEntityDespawn(payload);
        var bytes = new byte[EntityDespawnSize];
        BinaryPrimitives.WriteUInt64LittleEndian(bytes, payload.EntityId.Value);
        return bytes;
    }

    public static EntityDespawnPayload DecodeEntityDespawn(
        ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, EntityDespawnSize, nameof(EntityDespawnPayload));
        var result = new EntityDespawnPayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)));
        ValidateEntityDespawn(result);
        return result;
    }

    public static byte[] EncodeDamageIntent(DamageIntentPayload payload)
    {
        ValidateDamageIntent(payload);
        var bytes = new byte[DamageIntentSize];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(span, payload.AttackerId.Value);
        BinaryPrimitives.WriteUInt64LittleEndian(span[8..], payload.TargetId.Value);
        BinaryPrimitives.WriteUInt32LittleEndian(span[16..], payload.WeaponHash);
        BinaryPrimitives.WriteSingleLittleEndian(span[20..], payload.Damage);
        BinaryPrimitives.WriteUInt32LittleEndian(span[24..], payload.ShotSequence);
        BinaryPrimitives.WriteUInt32LittleEndian(span[28..], 0);
        return bytes;
    }

    public static DamageIntentPayload DecodeDamageIntent(
        ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, DamageIntentSize, nameof(DamageIntentPayload));
        if (BinaryPrimitives.ReadUInt32LittleEndian(payload[28..]) != 0)
        {
            throw new ProtocolException(
                "Damage intent reserved field must be zero.");
        }

        var result = new DamageIntentPayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload[8..])),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[16..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[20..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[24..]));
        ValidateDamageIntent(result);
        return result;
    }

    public static byte[] EncodeWorldState(WorldStatePayload payload)
    {
        ValidateWorldState(payload);
        var bytes = new byte[WorldStateSize];
        var span = bytes.AsSpan();
        span[0] = payload.Hour;
        span[1] = payload.Minute;
        span[2] = payload.Second;
        span[3] = (byte)payload.Flags;
        BinaryPrimitives.WriteUInt32LittleEndian(span[4..], payload.WeatherFrom);
        BinaryPrimitives.WriteUInt32LittleEndian(span[8..], payload.WeatherTo);
        BinaryPrimitives.WriteSingleLittleEndian(span[12..], payload.Blend);
        span[16] = payload.Day;
        span[17] = payload.Month;
        BinaryPrimitives.WriteUInt16LittleEndian(span[18..], payload.Year);
        BinaryPrimitives.WriteUInt32LittleEndian(span[20..], 0);
        return bytes;
    }

    public static WorldStatePayload DecodeWorldState(ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, WorldStateSize, nameof(WorldStatePayload));
        if (BinaryPrimitives.ReadUInt32LittleEndian(payload[20..]) != 0)
        {
            throw new ProtocolException(
                "World state reserved field must be zero.");
        }

        var result = new WorldStatePayload(
            payload[0],
            payload[1],
            payload[2],
            (WorldStateFlags)payload[3],
            BinaryPrimitives.ReadUInt32LittleEndian(payload[4..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[8..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[12..]),
            payload[16],
            payload[17],
            BinaryPrimitives.ReadUInt16LittleEndian(payload[18..]));
        ValidateWorldState(result);
        return result;
    }

    public static byte[] EncodeMissionState(MissionStatePayload payload)
    {
        ValidateMissionState(payload);
        var bytes = new byte[MissionStateSize];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(span, payload.HostEntityId.Value);
        BinaryPrimitives.WriteUInt32LittleEndian(span[8..], payload.MissionEpoch);
        BinaryPrimitives.WriteUInt32LittleEndian(span[12..], payload.Revision);
        BinaryPrimitives.WriteUInt32LittleEndian(
            span[16..],
            payload.CheckpointGeneration);
        span[20] = (byte)payload.Phase;
        span[21] = (byte)payload.Flags;
        BinaryPrimitives.WriteUInt16LittleEndian(span[22..], 0);
        WriteVector3(span[24..], payload.HostAnchor);
        BinaryPrimitives.WriteSingleLittleEndian(span[36..], payload.HostHeading);
        BinaryPrimitives.WriteUInt64LittleEndian(span[40..], 0);
        return bytes;
    }

    public static MissionStatePayload DecodeMissionState(
        ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, MissionStateSize, nameof(MissionStatePayload));
        if (BinaryPrimitives.ReadUInt16LittleEndian(payload[22..]) != 0 ||
            BinaryPrimitives.ReadUInt64LittleEndian(payload[40..]) != 0)
        {
            throw new ProtocolException(
                "Mission state reserved field must be zero.");
        }

        var result = new MissionStatePayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[8..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[12..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[16..]),
            (MissionPhase)payload[20],
            (MissionStateFlags)payload[21],
            ReadVector3(payload[24..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[36..]));
        ValidateMissionState(result);
        return result;
    }

    public static byte[] EncodeMissionCameraState(
        MissionCameraStatePayload payload)
    {
        ValidateMissionCameraState(payload);
        var bytes = new byte[MissionCameraStateSize];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(span, payload.HostEntityId.Value);
        BinaryPrimitives.WriteUInt32LittleEndian(span[8..], payload.MissionEpoch);
        BinaryPrimitives.WriteUInt32LittleEndian(
            span[12..], payload.CinematicGeneration);
        BinaryPrimitives.WriteUInt32LittleEndian(span[16..], payload.Revision);
        BinaryPrimitives.WriteUInt32LittleEndian(span[20..], (uint)payload.Flags);
        WriteVector3(span[24..], payload.Position);
        WriteVector3(span[36..], payload.Rotation);
        BinaryPrimitives.WriteSingleLittleEndian(span[48..], payload.FieldOfView);
        BinaryPrimitives.WriteUInt32LittleEndian(span[52..], 0);
        return bytes;
    }

    public static MissionCameraStatePayload DecodeMissionCameraState(
        ReadOnlySpan<byte> payload)
    {
        RequireLength(
            payload,
            MissionCameraStateSize,
            nameof(MissionCameraStatePayload));
        if (BinaryPrimitives.ReadUInt32LittleEndian(payload[52..]) != 0)
        {
            throw new ProtocolException(
                "Mission camera state reserved field must be zero.");
        }

        var result = new MissionCameraStatePayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[8..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[12..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[16..]),
            (MissionCameraStateFlags)BinaryPrimitives.ReadUInt32LittleEndian(
                payload[20..]),
            ReadVector3(payload[24..]),
            ReadVector3(payload[36..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[48..]));
        ValidateMissionCameraState(result);
        return result;
    }

    public static byte[] EncodeMissionCinematicState(
        MissionCinematicStatePayload payload)
    {
        ValidateMissionCinematicState(payload);
        var bytes = new byte[MissionCinematicStateSize];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(span, payload.HostEntityId.Value);
        BinaryPrimitives.WriteUInt32LittleEndian(span[8..], payload.MissionEpoch);
        BinaryPrimitives.WriteUInt32LittleEndian(
            span[12..], payload.CinematicGeneration);
        BinaryPrimitives.WriteUInt32LittleEndian(span[16..], payload.Revision);
        BinaryPrimitives.WriteUInt32LittleEndian(
            span[20..], payload.CheckpointGeneration);
        span[24] = (byte)payload.Phase;
        span[25] = 0;
        BinaryPrimitives.WriteUInt16LittleEndian(span[26..], (ushort)payload.Flags);
        WriteVector3(span[28..], payload.ResumeAnchor);
        BinaryPrimitives.WriteSingleLittleEndian(span[40..], payload.ResumeHeading);
        BinaryPrimitives.WriteUInt32LittleEndian(span[44..], 0);
        return bytes;
    }

    public static MissionCinematicStatePayload DecodeMissionCinematicState(
        ReadOnlySpan<byte> payload)
    {
        RequireLength(
            payload,
            MissionCinematicStateSize,
            nameof(MissionCinematicStatePayload));
        if (payload[25] != 0 ||
            BinaryPrimitives.ReadUInt32LittleEndian(payload[44..]) != 0)
        {
            throw new ProtocolException(
                "Mission cinematic state reserved fields must be zero.");
        }

        var result = new MissionCinematicStatePayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[8..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[12..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[16..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[20..]),
            (MissionCinematicPhase)payload[24],
            (MissionCinematicStateFlags)BinaryPrimitives.ReadUInt16LittleEndian(
                payload[26..]),
            ReadVector3(payload[28..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[40..]));
        ValidateMissionCinematicState(result);
        return result;
    }

    public static byte[] EncodeMissionCinematicAction(
        MissionCinematicActionPayload payload)
    {
        ValidateMissionCinematicAction(payload);
        var bytes = new byte[MissionCinematicActionSize];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(span, payload.HostEntityId.Value);
        BinaryPrimitives.WriteUInt32LittleEndian(span[8..], payload.MissionEpoch);
        BinaryPrimitives.WriteUInt32LittleEndian(
            span[12..], payload.CinematicGeneration);
        BinaryPrimitives.WriteUInt32LittleEndian(span[16..], payload.ActionId);
        span[20] = (byte)payload.Kind;
        span[21] = payload.SenderSlot;
        BinaryPrimitives.WriteUInt16LittleEndian(span[22..], (ushort)payload.Flags);
        BinaryPrimitives.WriteUInt64LittleEndian(span[24..], 0);
        return bytes;
    }

    public static MissionCinematicActionPayload DecodeMissionCinematicAction(
        ReadOnlySpan<byte> payload)
    {
        RequireLength(
            payload,
            MissionCinematicActionSize,
            nameof(MissionCinematicActionPayload));
        if (BinaryPrimitives.ReadUInt64LittleEndian(payload[24..]) != 0)
        {
            throw new ProtocolException(
                "Mission cinematic action reserved field must be zero.");
        }

        var result = new MissionCinematicActionPayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[8..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[12..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[16..]),
            (MissionCinematicActionKind)payload[20],
            payload[21],
            (MissionCinematicActionFlags)BinaryPrimitives.ReadUInt16LittleEndian(
                payload[22..]));
        ValidateMissionCinematicAction(result);
        return result;
    }

    public static byte[] EncodeAnimSceneReplicaState(
        AnimSceneReplicaStatePayload payload)
    {
        ValidateAnimSceneReplicaState(payload);
        var bytes = new byte[AnimSceneReplicaStateSize];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(span, payload.HostEntityId.Value);
        BinaryPrimitives.WriteUInt32LittleEndian(span[8..], payload.MissionEpoch);
        BinaryPrimitives.WriteUInt32LittleEndian(
            span[12..], payload.CinematicGeneration);
        BinaryPrimitives.WriteUInt32LittleEndian(
            span[16..], payload.DefinitionRevision);
        BinaryPrimitives.WriteUInt32LittleEndian(span[20..], payload.Revision);
        BinaryPrimitives.WriteUInt32LittleEndian(span[24..], payload.DictionaryHash);
        BinaryPrimitives.WriteUInt32LittleEndian(span[28..], (uint)payload.Flags);
        BinaryPrimitives.WriteSingleLittleEndian(span[32..], payload.Phase);
        BinaryPrimitives.WriteSingleLittleEndian(span[36..], payload.DurationSeconds);
        BinaryPrimitives.WriteSingleLittleEndian(span[40..], payload.Rate);
        WriteVector3(span[44..], payload.OriginPosition);
        WriteVector3(span[56..], payload.OriginRotation);
        BinaryPrimitives.WriteUInt16LittleEndian(span[68..], payload.ActiveCameraCount);
        BinaryPrimitives.WriteUInt16LittleEndian(span[70..], 0);
        return bytes;
    }

    public static AnimSceneReplicaStatePayload DecodeAnimSceneReplicaState(
        ReadOnlySpan<byte> payload)
    {
        RequireLength(
            payload,
            AnimSceneReplicaStateSize,
            nameof(AnimSceneReplicaStatePayload));
        if (BinaryPrimitives.ReadUInt16LittleEndian(payload[70..]) != 0)
        {
            throw new ProtocolException(
                "AnimScene replica reserved field must be zero.");
        }
        var result = new AnimSceneReplicaStatePayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[8..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[12..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[16..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[20..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[24..]),
            (AnimSceneReplicaStateFlags)BinaryPrimitives.ReadUInt32LittleEndian(
                payload[28..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[32..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[36..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[40..]),
            ReadVector3(payload[44..]),
            ReadVector3(payload[56..]),
            BinaryPrimitives.ReadUInt16LittleEndian(payload[68..]));
        ValidateAnimSceneReplicaState(result);
        return result;
    }

    public static byte[] EncodeAnimSceneDefinition(
        AnimSceneDefinitionPayload payload)
    {
        ValidateAnimSceneDefinition(payload);
        return SerializeAnimSceneDefinition(payload, clearFingerprint: false);
    }

    public static AnimSceneDefinitionFingerprint
        ComputeAnimSceneDefinitionFingerprint(
            AnimSceneDefinitionPayload payload)
    {
        ValidateAnimSceneDefinitionShape(payload);
        var canonical = SerializeAnimSceneDefinition(
            payload,
            clearFingerprint: true);
        var digest = SHA256.HashData(canonical);
        return new AnimSceneDefinitionFingerprint(
            BinaryPrimitives.ReadUInt64LittleEndian(digest),
            BinaryPrimitives.ReadUInt64LittleEndian(digest.AsSpan(8)));
    }

    private static byte[] SerializeAnimSceneDefinition(
        AnimSceneDefinitionPayload payload,
        bool clearFingerprint)
    {
        var resource = EncodePrintableAscii(
            payload.ResourceName,
            allowEmpty: false,
            AnimSceneResourceMaximumBytes,
            "AnimScene resource name");
        var playback = EncodePrintableAscii(
            payload.PlaybackList,
            allowEmpty: true,
            AnimScenePlaybackListMaximumBytes,
            "AnimScene playback list");
        var roleNames = payload.Roles
            .Select(static role => EncodePrintableAscii(
                role.RoleName,
                allowEmpty: false,
                AnimSceneRoleNameMaximumBytes,
                "AnimScene role name"))
            .ToArray();
        var size = checked(
            AnimSceneDefinitionHeaderSize +
            resource.Length +
            playback.Length +
            payload.Roles.Length * AnimSceneRoleBindingHeaderSize +
            roleNames.Sum(static name => name.Length));
        if (size > AnimSceneDefinitionMaximumSize)
        {
            throw new ProtocolException(
                $"AnimScene definition exceeds {AnimSceneDefinitionMaximumSize} bytes.");
        }

        var bytes = new byte[size];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(span, payload.HostEntityId.Value);
        BinaryPrimitives.WriteUInt32LittleEndian(span[8..], payload.MissionEpoch);
        BinaryPrimitives.WriteUInt32LittleEndian(
            span[12..], payload.CinematicGeneration);
        BinaryPrimitives.WriteUInt32LittleEndian(
            span[16..], payload.DefinitionRevision);
        BinaryPrimitives.WriteUInt32LittleEndian(span[20..], payload.DictionaryHash);
        BinaryPrimitives.WriteUInt64LittleEndian(
            span[24..], clearFingerprint ? 0 : payload.FingerprintLow);
        BinaryPrimitives.WriteUInt64LittleEndian(
            span[32..], clearFingerprint ? 0 : payload.FingerprintHigh);
        BinaryPrimitives.WriteSingleLittleEndian(span[40..], payload.DurationSeconds);
        BinaryPrimitives.WriteUInt16LittleEndian(span[44..], (ushort)resource.Length);
        BinaryPrimitives.WriteUInt16LittleEndian(span[46..], (ushort)playback.Length);
        BinaryPrimitives.WriteUInt16LittleEndian(
            span[48..], (ushort)payload.Roles.Length);
        BinaryPrimitives.WriteUInt16LittleEndian(span[50..], 0);
        BinaryPrimitives.WriteUInt32LittleEndian(span[52..], payload.SceneFlags);
        span[56] = payload.CreateOptionFlags;

        var offset = AnimSceneDefinitionHeaderSize;
        resource.CopyTo(span[offset..]);
        offset += resource.Length;
        playback.CopyTo(span[offset..]);
        offset += playback.Length;
        for (var index = 0; index < payload.Roles.Length; index++)
        {
            var role = payload.Roles[index];
            var roleName = roleNames[index];
            BinaryPrimitives.WriteUInt64LittleEndian(
                span[offset..], role.EntityId.Value);
            BinaryPrimitives.WriteUInt32LittleEndian(
                span[(offset + 8)..], role.ModelHash);
            BinaryPrimitives.WriteUInt32LittleEndian(
                span[(offset + 12)..], role.BindingFlags);
            BinaryPrimitives.WriteUInt16LittleEndian(
                span[(offset + 16)..], (ushort)role.Flags);
            span[offset + 18] = (byte)role.Kind;
            span[offset + 19] = (byte)roleName.Length;
            offset += AnimSceneRoleBindingHeaderSize;
            roleName.CopyTo(span[offset..]);
            offset += roleName.Length;
        }
        return bytes;
    }

    public static AnimSceneDefinitionPayload DecodeAnimSceneDefinition(
        ReadOnlySpan<byte> payload)
    {
        if (payload.Length < AnimSceneDefinitionHeaderSize ||
            payload.Length > AnimSceneDefinitionMaximumSize)
        {
            throw new ProtocolException(
                $"AnimScene definition must contain {AnimSceneDefinitionHeaderSize} to " +
                $"{AnimSceneDefinitionMaximumSize} bytes; received {payload.Length}.");
        }
        if (BinaryPrimitives.ReadUInt16LittleEndian(payload[50..]) != 0 ||
            payload[57] != 0 || payload[58] != 0 || payload[59] != 0)
        {
            throw new ProtocolException(
                "AnimScene definition reserved field must be zero.");
        }

        var resourceLength = BinaryPrimitives.ReadUInt16LittleEndian(payload[44..]);
        var playbackLength = BinaryPrimitives.ReadUInt16LittleEndian(payload[46..]);
        var roleCount = BinaryPrimitives.ReadUInt16LittleEndian(payload[48..]);
        if (resourceLength is 0 or > AnimSceneResourceMaximumBytes ||
            playbackLength > AnimScenePlaybackListMaximumBytes ||
            roleCount > AnimSceneDefinitionMaximumRoles)
        {
            throw new ProtocolException(
                "AnimScene definition contains an out-of-range string or role count.");
        }

        var offset = AnimSceneDefinitionHeaderSize;
        if (resourceLength + playbackLength > payload.Length - offset)
        {
            throw new ProtocolException(
                "AnimScene definition string lengths exceed the payload.");
        }
        var resourceName = DecodePrintableAscii(
            payload.Slice(offset, resourceLength),
            allowEmpty: false,
            AnimSceneResourceMaximumBytes,
            "AnimScene resource name");
        offset += resourceLength;
        var playbackList = DecodePrintableAscii(
            payload.Slice(offset, playbackLength),
            allowEmpty: true,
            AnimScenePlaybackListMaximumBytes,
            "AnimScene playback list");
        offset += playbackLength;

        var roles = new AnimSceneRoleBindingPayload[roleCount];
        for (var index = 0; index < roles.Length; index++)
        {
            if (payload.Length - offset < AnimSceneRoleBindingHeaderSize)
            {
                throw new ProtocolException(
                    "AnimScene definition ends inside a role binding header.");
            }
            var roleNameLength = payload[offset + 19];
            if (roleNameLength is 0 or > AnimSceneRoleNameMaximumBytes ||
                payload.Length - offset - AnimSceneRoleBindingHeaderSize <
                    roleNameLength)
            {
                throw new ProtocolException(
                    "AnimScene role name length exceeds the payload or its limit.");
            }
            var role = new AnimSceneRoleBindingPayload(
                DecodePrintableAscii(
                    payload.Slice(
                        offset + AnimSceneRoleBindingHeaderSize,
                        roleNameLength),
                    allowEmpty: false,
                    AnimSceneRoleNameMaximumBytes,
                    "AnimScene role name"),
                new NetEntityId(
                    BinaryPrimitives.ReadUInt64LittleEndian(payload[offset..])),
                BinaryPrimitives.ReadUInt32LittleEndian(payload[(offset + 8)..]),
                (AnimSceneRoleKind)payload[offset + 18],
                (AnimSceneRoleFlags)BinaryPrimitives.ReadUInt16LittleEndian(
                    payload[(offset + 16)..]),
                BinaryPrimitives.ReadUInt32LittleEndian(payload[(offset + 12)..]));
            roles[index] = role;
            offset += AnimSceneRoleBindingHeaderSize + roleNameLength;
        }
        if (offset != payload.Length)
        {
            throw new ProtocolException(
                "AnimScene definition contains trailing or unaccounted bytes.");
        }

        var result = new AnimSceneDefinitionPayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[8..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[12..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[16..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[20..]),
            BinaryPrimitives.ReadUInt64LittleEndian(payload[24..]),
            BinaryPrimitives.ReadUInt64LittleEndian(payload[32..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[40..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[52..]),
            payload[56],
            resourceName,
            playbackList,
            roles);
        ValidateAnimSceneDefinition(result);
        return result;
    }

    public static byte[] EncodeAnimSceneControl(AnimSceneControlPayload payload)
    {
        ValidateAnimSceneControl(payload);
        var bytes = new byte[AnimSceneControlSize];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(span, payload.HostEntityId.Value);
        BinaryPrimitives.WriteUInt32LittleEndian(span[8..], payload.MissionEpoch);
        BinaryPrimitives.WriteUInt32LittleEndian(
            span[12..], payload.CinematicGeneration);
        BinaryPrimitives.WriteUInt32LittleEndian(
            span[16..], payload.DefinitionRevision);
        BinaryPrimitives.WriteUInt32LittleEndian(span[20..], payload.ActionId);
        BinaryPrimitives.WriteUInt64LittleEndian(span[24..], payload.FingerprintLow);
        BinaryPrimitives.WriteUInt64LittleEndian(span[32..], payload.FingerprintHigh);
        BinaryPrimitives.WriteUInt64LittleEndian(span[40..], payload.PlayAtHostTick);
        BinaryPrimitives.WriteSingleLittleEndian(span[48..], payload.StartPhase);
        BinaryPrimitives.WriteSingleLittleEndian(span[52..], payload.Rate);
        span[56] = (byte)payload.Kind;
        span[57] = payload.SenderSlot;
        span[58] = (byte)payload.Reason;
        span[59] = 0;
        BinaryPrimitives.WriteUInt32LittleEndian(span[60..], (uint)payload.Flags);
        return bytes;
    }

    public static AnimSceneControlPayload DecodeAnimSceneControl(
        ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, AnimSceneControlSize, nameof(AnimSceneControlPayload));
        if (payload[59] != 0)
        {
            throw new ProtocolException(
                "AnimScene control reserved byte must be zero.");
        }
        var result = new AnimSceneControlPayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[8..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[12..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[16..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[20..]),
            BinaryPrimitives.ReadUInt64LittleEndian(payload[24..]),
            BinaryPrimitives.ReadUInt64LittleEndian(payload[32..]),
            BinaryPrimitives.ReadUInt64LittleEndian(payload[40..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[48..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[52..]),
            (AnimSceneControlKind)payload[56],
            payload[57],
            (AnimSceneControlReason)payload[58],
            (AnimSceneControlFlags)BinaryPrimitives.ReadUInt32LittleEndian(
                payload[60..]));
        ValidateAnimSceneControl(result);
        return result;
    }

    public static byte[] EncodeEquipmentState(EquipmentStatePayload payload)
    {
        ValidateEquipmentState(payload);
        var bytes = new byte[EquipmentStateSize];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(span, payload.EntityId.Value);
        BinaryPrimitives.WriteUInt32LittleEndian(span[8..], payload.WeaponHash);
        BinaryPrimitives.WriteUInt32LittleEndian(span[12..], payload.Ammo);
        BinaryPrimitives.WriteUInt32LittleEndian(span[16..], (uint)payload.Flags);
        BinaryPrimitives.WriteUInt32LittleEndian(span[20..], 0);
        return bytes;
    }

    public static EquipmentStatePayload DecodeEquipmentState(
        ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, EquipmentStateSize, nameof(EquipmentStatePayload));
        if (BinaryPrimitives.ReadUInt32LittleEndian(payload[20..]) != 0)
        {
            throw new ProtocolException(
                "Equipment state reserved field must be zero.");
        }

        var result = new EquipmentStatePayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[8..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[12..]),
            (EquipmentStateFlags)BinaryPrimitives.ReadUInt32LittleEndian(
                payload[16..]));
        ValidateEquipmentState(result);
        return result;
    }

    public static byte[] EncodePauseVote(PauseVotePayload payload)
    {
        ValidatePauseVote(payload);
        var bytes = new byte[PauseVoteSize];
        var span = bytes.AsSpan();
        span[0] = (byte)payload.Kind;
        span[1] = payload.VoterSlot;
        span[2] = (byte)payload.Flags;
        span[3] = 0;
        BinaryPrimitives.WriteUInt32LittleEndian(span[4..], payload.Generation);
        BinaryPrimitives.WriteUInt32LittleEndian(span[8..], 0);
        return bytes;
    }

    public static PauseVotePayload DecodePauseVote(ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, PauseVoteSize, nameof(PauseVotePayload));
        if (payload[3] != 0 ||
            BinaryPrimitives.ReadUInt32LittleEndian(payload[8..]) != 0)
        {
            throw new ProtocolException(
                "Pause-vote reserved fields must be zero.");
        }

        var result = new PauseVotePayload(
            (PauseVoteKind)payload[0],
            payload[1],
            (PauseVoteFlags)payload[2],
            BinaryPrimitives.ReadUInt32LittleEndian(payload[4..]));
        ValidatePauseVote(result);
        return result;
    }

    public static byte[] EncodeCommand(CommandPayload payload)
    {
        if (!Enum.IsDefined(payload.Opcode))
        {
            throw new ProtocolException($"Unknown command opcode {(ushort)payload.Opcode}.");
        }

        if (!IsFinite(payload.Position) ||
            !float.IsFinite(payload.Heading) ||
            !float.IsFinite(payload.Value))
        {
            throw new ProtocolException("Command numeric values must be finite.");
        }

        var bytes = new byte[CommandSize];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt16LittleEndian(span, (ushort)payload.Opcode);
        BinaryPrimitives.WriteUInt16LittleEndian(span[2..], (ushort)payload.Flags);
        BinaryPrimitives.WriteUInt64LittleEndian(span[4..], payload.TargetEntityId.Value);
        WriteVector3(span[12..], payload.Position);
        BinaryPrimitives.WriteSingleLittleEndian(span[24..], payload.Heading);
        BinaryPrimitives.WriteSingleLittleEndian(span[28..], payload.Value);
        return bytes;
    }

    public static CommandPayload DecodeCommand(ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, CommandSize, nameof(CommandPayload));
        var result = new CommandPayload(
            (CommandOpcode)BinaryPrimitives.ReadUInt16LittleEndian(payload),
            (CommandFlags)BinaryPrimitives.ReadUInt16LittleEndian(payload[2..]),
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload[4..])),
            ReadVector3(payload[12..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[24..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[28..]));
        _ = EncodeCommand(result);
        return result;
    }

    public static byte[] EncodeDownedState(DownedStatePayload payload)
    {
        ValidateDownedState(payload);
        var bytes = new byte[DownedStateSize];
        BinaryPrimitives.WriteUInt64LittleEndian(bytes, payload.EntityId.Value);
        bytes[8] = (byte)payload.Lifecycle;
        BinaryPrimitives.WriteSingleLittleEndian(bytes.AsSpan(12), payload.HealthFraction);
        return bytes;
    }

    public static DownedStatePayload DecodeDownedState(ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, DownedStateSize, nameof(DownedStatePayload));
        if (payload[9] != 0 ||
            BinaryPrimitives.ReadUInt16LittleEndian(payload[10..]) != 0)
        {
            throw new ProtocolException(
                "Downed state reserved bytes must be zero.");
        }

        var result = new DownedStatePayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            (PlayerLifecycle)payload[8],
            BinaryPrimitives.ReadSingleLittleEndian(payload[12..]));
        ValidateDownedState(result);
        return result;
    }

    public static byte[] EncodeReviveRequest(ReviveRequestPayload payload)
    {
        ValidateReviveRequest(payload);
        var bytes = new byte[ReviveRequestSize];
        BinaryPrimitives.WriteUInt64LittleEndian(bytes, payload.ReviverId.Value);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(8), payload.TargetId.Value);
        return bytes;
    }

    public static ReviveRequestPayload DecodeReviveRequest(ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, ReviveRequestSize, nameof(ReviveRequestPayload));
        var result = new ReviveRequestPayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload[8..])));
        ValidateReviveRequest(result);
        return result;
    }

    public static byte[] EncodeReviveComplete(ReviveCompletePayload payload)
    {
        ValidateReviveComplete(payload);
        var bytes = new byte[ReviveCompleteSize];
        BinaryPrimitives.WriteUInt64LittleEndian(bytes, payload.ReviverId.Value);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(8), payload.TargetId.Value);
        BinaryPrimitives.WriteSingleLittleEndian(bytes.AsSpan(16), payload.HealthFraction);
        return bytes;
    }

    public static ReviveCompletePayload DecodeReviveComplete(ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, ReviveCompleteSize, nameof(ReviveCompletePayload));
        var result = new ReviveCompletePayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload[8..])),
            BinaryPrimitives.ReadSingleLittleEndian(payload[16..]));
        ValidateReviveComplete(result);
        return result;
    }

    private static void ValidateDownedState(DownedStatePayload payload)
    {
        if (!payload.EntityId.IsValid)
        {
            throw new ProtocolException(
                "Downed state requires a valid entity ID.");
        }

        if (!Enum.IsDefined(payload.Lifecycle))
        {
            throw new ProtocolException(
                $"Downed state has unknown lifecycle {(byte)payload.Lifecycle}.");
        }

        if (!float.IsFinite(payload.HealthFraction) ||
            payload.HealthFraction is < 0f or > 1f)
        {
            throw new ProtocolException(
                "Downed state health must be finite and between zero and one.");
        }
    }

    private static void ValidateReviveRequest(ReviveRequestPayload payload)
    {
        if (!payload.ReviverId.IsValid ||
            !payload.TargetId.IsValid ||
            payload.ReviverId == payload.TargetId)
        {
            throw new ProtocolException(
                "Revive request requires two distinct valid entity IDs.");
        }
    }

    private static void ValidateReviveComplete(ReviveCompletePayload payload)
    {
        ValidateReviveRequest(
            new ReviveRequestPayload(
                payload.ReviverId,
                payload.TargetId));
        if (!float.IsFinite(payload.HealthFraction) ||
            payload.HealthFraction is < 0f or > 1f)
        {
            throw new ProtocolException(
                "Revive completion health must be finite and between zero and one.");
        }
    }

    private static void ValidatePlayerState(PlayerStatePayload payload)
    {
        if (!payload.EntityId.IsValid)
        {
            throw new ProtocolException(
                "Player state requires non-zero NetEntityId epoch and counter.");
        }

        if (payload.Slot > (byte)SessionRole.Guest)
        {
            throw new ProtocolException(
                $"Player state has unknown slot {payload.Slot}.");
        }

        if (!Enum.IsDefined(payload.Lifecycle))
        {
            throw new ProtocolException($"Unknown lifecycle {(byte)payload.Lifecycle}.");
        }

        if (!IsFinite(payload.Position) ||
            !IsFinite(payload.Velocity) ||
            !float.IsFinite(payload.Heading) ||
            !float.IsFinite(payload.HealthFraction) ||
            payload.HealthFraction is < 0f or > 1f ||
            !IsFinite(payload.AimTarget) ||
            !float.IsFinite(payload.MovementHeading) ||
            !float.IsFinite(payload.LocalForwardSpeed) ||
            !float.IsFinite(payload.LocalRightSpeed) ||
            MathF.Abs(payload.LocalForwardSpeed) > 50f ||
            MathF.Abs(payload.LocalRightSpeed) > 50f ||
            !float.IsFinite(payload.DesiredMoveBlend) ||
            payload.DesiredMoveBlend is < 0f or > 3f ||
            !Enum.IsDefined(payload.TraversalKind) ||
            !Enum.IsDefined(payload.LocomotionMode) ||
            !IsFinite(payload.TraversalAnchor) ||
            !float.IsFinite(payload.TraversalHeading))
        {
            throw new ProtocolException("Player state contains an invalid numeric value.");
        }

        var aimTargetValid =
            (payload.Flags & PlayerStateFlags.AimTargetValid) != 0;
        if (!aimTargetValid && payload.AimTarget != Vector3.Zero)
        {
            throw new ProtocolException(
                "Player state without AimTargetValid requires a zero aim target.");
        }

        if (payload.TraversalKind != PlayerTraversalKind.None &&
            payload.TraversalActionId == 0)
        {
            throw new ProtocolException(
                "A traversal intent requires a non-zero action ID.");
        }
    }

    private static void ValidatePlayerTraversal(PlayerTraversalPayload payload)
    {
        const PlayerTraversalFlags allowedFlags =
            PlayerTraversalFlags.InputEdgeDetected |
            PlayerTraversalFlags.ObstacleValid |
            PlayerTraversalFlags.ExpectedLandingValid;
        var obstacleValid =
            (payload.Flags & PlayerTraversalFlags.ObstacleValid) != 0;
        var landingValid =
            (payload.Flags & PlayerTraversalFlags.ExpectedLandingValid) != 0;
        if (!payload.EntityId.IsValid ||
            payload.Slot > (byte)SessionRole.Guest ||
            payload.Kind is PlayerTraversalKind.None ||
            !Enum.IsDefined(payload.Kind) ||
            payload.ActionId == 0 ||
            payload.Revision == 0 ||
            payload.LocomotionEpoch == 0 ||
            (payload.Flags & ~allowedFlags) != 0 ||
            !float.IsFinite(payload.TakeoffHeading) ||
            !IsFinite(payload.TakeoffPosition) ||
            !IsFinite(payload.ApproachVelocity) ||
            !IsFinite(payload.ObstaclePoint) ||
            !IsFinite(payload.ObstacleNormal) ||
            !float.IsFinite(payload.ObstacleTopZ) ||
            !IsFinite(payload.ExpectedLanding))
        {
            throw new ProtocolException(
                "Player traversal contains an invalid identity, enum, flag or numeric value.");
        }

        if (!obstacleValid &&
            (payload.ObstaclePoint != Vector3.Zero ||
             payload.ObstacleNormal != Vector3.Zero ||
             payload.ObstacleTopZ != 0f))
        {
            throw new ProtocolException(
                "Traversal without ObstacleValid requires zero obstacle fields.");
        }
        if (!landingValid && payload.ExpectedLanding != Vector3.Zero)
        {
            throw new ProtocolException(
                "Traversal without ExpectedLandingValid requires a zero landing point.");
        }
    }

    private static void ValidatePlayerAction(PlayerActionPayload payload)
    {
        const PlayerActionFlags allowedFlags =
            PlayerActionFlags.Intent |
            PlayerActionFlags.Authoritative |
            PlayerActionFlags.TargetEntityValid |
            PlayerActionFlags.TargetPointValid |
            PlayerActionFlags.ActorAnchorValid |
            PlayerActionFlags.Persistent |
            PlayerActionFlags.PhysicalTargetEffect |
            PlayerActionFlags.ResyncSnapshot |
            PlayerActionFlags.VariantValid |
            PlayerActionFlags.AnimationSampleValid |
            PlayerActionFlags.NormalizedPhaseValid;
        const uint maximumActionTimeMilliseconds = 3_600_000;

        var intent = (payload.Flags & PlayerActionFlags.Intent) != 0;
        var authoritative =
            (payload.Flags & PlayerActionFlags.Authoritative) != 0;
        var targetEntityValid =
            (payload.Flags & PlayerActionFlags.TargetEntityValid) != 0;
        var targetPointValid =
            (payload.Flags & PlayerActionFlags.TargetPointValid) != 0;
        var actorAnchorValid =
            (payload.Flags & PlayerActionFlags.ActorAnchorValid) != 0;
        var variantValid =
            (payload.Flags & PlayerActionFlags.VariantValid) != 0;
        var animationSampleValid =
            (payload.Flags & PlayerActionFlags.AnimationSampleValid) != 0;
        var normalizedPhaseValid =
            (payload.Flags & PlayerActionFlags.NormalizedPhaseValid) != 0;

        if (!payload.ActorEntityId.IsValid ||
            payload.Sequence == 0 ||
            payload.ActionId == 0 ||
            payload.Revision == 0 ||
            payload.ActorSlot > (byte)SessionRole.Guest ||
            payload.AuthoritySlot > (byte)SessionRole.Guest ||
            payload.Kind is PlayerActionKind.None ||
            !Enum.IsDefined(payload.Kind) ||
            payload.Phase is PlayerActionPhase.None ||
            !Enum.IsDefined(payload.Phase) ||
            (payload.Flags & ~allowedFlags) != 0 ||
            intent == authoritative)
        {
            throw new ProtocolException(
                "Player action contains an invalid identity, enum, authority or flag.");
        }

        if ((intent && payload.ActorSlot != payload.AuthoritySlot) ||
            (authoritative &&
             payload.AuthoritySlot != (byte)SessionRole.Host))
        {
            throw new ProtocolException(
                "Player action authority does not match its actor or host resolver.");
        }

        if (targetEntityValid
                ? !payload.TargetEntityId.IsValid ||
                  payload.TargetEntityId == payload.ActorEntityId
                : payload.TargetEntityId != NetEntityId.None)
        {
            throw new ProtocolException(
                "Player action target entity validity is inconsistent.");
        }

        if (!targetPointValid && payload.TargetPoint != Vector3.Zero ||
            !actorAnchorValid && payload.ActorAnchor != Vector3.Zero ||
            !variantValid && payload.VariantHash != 0 ||
            variantValid && payload.VariantHash == 0 ||
            !animationSampleValid && payload.AnimationSampleSequence != 0 ||
            animationSampleValid && payload.AnimationSampleSequence == 0 ||
            !normalizedPhaseValid && payload.NormalizedPhase != 0)
        {
            throw new ProtocolException(
                "Player action optional fields do not match their validity flags.");
        }

        if (!IsFinite(payload.ActorAnchor) ||
            !IsFinite(payload.TargetPoint) ||
            !float.IsFinite(payload.FacingHeading) ||
            payload.FacingHeading is < 0f or >= 360f ||
            !float.IsFinite(payload.NormalizedPhase) ||
            normalizedPhaseValid && payload.NormalizedPhase is < 0f or > 1f ||
            payload.DurationMilliseconds > maximumActionTimeMilliseconds ||
            payload.PhaseElapsedMilliseconds > maximumActionTimeMilliseconds ||
            payload.DurationMilliseconds != 0 &&
            payload.PhaseElapsedMilliseconds > payload.DurationMilliseconds)
        {
            throw new ProtocolException(
                "Player action contains an invalid numeric value.");
        }

        if ((payload.Flags & PlayerActionFlags.PhysicalTargetEffect) != 0 &&
            !targetEntityValid)
        {
            throw new ProtocolException(
                "A physical player action effect requires a target entity.");
        }

        if ((payload.Flags & PlayerActionFlags.ResyncSnapshot) != 0 &&
            (!authoritative ||
             (payload.Flags & PlayerActionFlags.Persistent) == 0))
        {
            throw new ProtocolException(
                "A player action resync snapshot must be authoritative and persistent.");
        }
    }

    private static void ValidateInteractionIntent(
        InteractionIntentPayload payload)
    {
        const InteractionIntentFlags allowedFlags =
            InteractionIntentFlags.TargetPlayer |
            InteractionIntentFlags.TargetMount |
            InteractionIntentFlags.HoldRequired;
        const uint maximumDurationMilliseconds = 60_000;
        var targetsPlayer =
            payload.Flags.HasFlag(InteractionIntentFlags.TargetPlayer);
        var targetsMount =
            payload.Flags.HasFlag(InteractionIntentFlags.TargetMount);

        if (!payload.ActorEntityId.IsValid ||
            !payload.TargetEntityId.IsValid ||
            (payload.ActorEntityId == payload.TargetEntityId &&
             payload.Kind != InteractionKind.EmergencyRecover) ||
            payload.InteractionId == 0 ||
            payload.Revision == 0 ||
            payload.ActorSlot > (byte)SessionRole.Guest ||
            payload.Kind is InteractionKind.None ||
            !Enum.IsDefined(payload.Kind) ||
            payload.Phase is InteractionIntentPhase.None ||
            !Enum.IsDefined(payload.Phase) ||
            (payload.Flags & ~allowedFlags) != 0 ||
            targetsPlayer == targetsMount ||
            payload.RequestedDurationMilliseconds > maximumDurationMilliseconds)
        {
            throw new ProtocolException(
                "Interaction intent contains an invalid identity, enum, flag or duration.");
        }

        var secondaryRequired = payload.Kind is
            InteractionKind.MountDriver or
            InteractionKind.MountPassenger;
        if (secondaryRequired != payload.SecondaryEntityId.IsValid)
        {
            throw new ProtocolException(
                "Interaction intent secondary entity does not match its kind.");
        }
        if (secondaryRequired &&
            (payload.SecondaryEntityId == payload.ActorEntityId ||
             payload.SecondaryEntityId == payload.TargetEntityId))
        {
            throw new ProtocolException(
                "Interaction intent requires distinct actor, target and secondary identities.");
        }

        var holdRequired =
            payload.Flags.HasFlag(InteractionIntentFlags.HoldRequired);
        if (payload.Phase == InteractionIntentPhase.Cancel)
        {
            if (payload.RequestedDurationMilliseconds != 0 || holdRequired)
            {
                throw new ProtocolException(
                    "Cancelled interaction intent requires zero duration and no hold flag.");
            }
        }
        else if (payload.Kind == InteractionKind.Revive)
        {
            if (!targetsPlayer || !holdRequired ||
                payload.RequestedDurationMilliseconds !=
                    DownedStateMachine.ReviveDurationMs)
            {
                throw new ProtocolException(
                    "Revive interaction requires a four-second held player target.");
            }
        }
        else if (holdRequired !=
                 (payload.RequestedDurationMilliseconds > 0))
        {
            throw new ProtocolException(
                "Interaction hold flag and requested duration are inconsistent.");
        }

    }

    private static void ValidateInteractionResult(
        InteractionResultPayload payload)
    {
        const InteractionResultFlags allowedFlags =
            InteractionResultFlags.Authoritative |
            InteractionResultFlags.StateChanged |
            InteractionResultFlags.HoldRequired;
        const uint maximumDurationMilliseconds = 60_000;
        if (!payload.ActorEntityId.IsValid ||
            !payload.TargetEntityId.IsValid ||
            (payload.ActorEntityId == payload.TargetEntityId &&
             payload.Kind != InteractionKind.EmergencyRecover) ||
            payload.InteractionId == 0 ||
            payload.Revision == 0 ||
            payload.Kind is InteractionKind.None ||
            !Enum.IsDefined(payload.Kind) ||
            payload.Status is InteractionResultStatus.None ||
            !Enum.IsDefined(payload.Status) ||
            !Enum.IsDefined(payload.RejectReason) ||
            (payload.Flags & ~allowedFlags) != 0 ||
            !payload.Flags.HasFlag(InteractionResultFlags.Authoritative) ||
            payload.ProgressMilliseconds > maximumDurationMilliseconds ||
            payload.RequiredDurationMilliseconds > maximumDurationMilliseconds ||
            (payload.RequiredDurationMilliseconds != 0 &&
             payload.ProgressMilliseconds > payload.RequiredDurationMilliseconds))
        {
            throw new ProtocolException(
                "Interaction result contains an invalid identity, enum, flag or progress.");
        }

        var rejected = payload.Status == InteractionResultStatus.Rejected;
        if (rejected !=
            (payload.RejectReason != InteractionRejectReason.None))
        {
            throw new ProtocolException(
                "Interaction reject reason is inconsistent with result status.");
        }

        var secondaryRequired = payload.Kind is
            InteractionKind.MountDriver or
            InteractionKind.MountPassenger;
        if (secondaryRequired != payload.SecondaryEntityId.IsValid)
        {
            throw new ProtocolException(
                "Interaction result secondary entity does not match its kind.");
        }
    }

    private static void ValidateRestraintState(
        RestraintStatePayload payload)
    {
        const RestraintStateFlags allowedFlags =
            RestraintStateFlags.Authoritative |
            RestraintStateFlags.EngineOwned |
            RestraintStateFlags.Snapshot;
        if (!payload.SubjectEntityId.IsValid ||
            payload.Revision == 0 ||
            !Enum.IsDefined(payload.State) ||
            (payload.Flags & ~allowedFlags) != 0 ||
            !payload.Flags.HasFlag(RestraintStateFlags.Authoritative))
        {
            throw new ProtocolException(
                "Restraint state contains an invalid subject, revision, state or flag.");
        }

        if (payload.State == PlayerRestraintState.Free)
        {
            if (payload.OwnerEntityId.IsValid ||
                payload.Flags.HasFlag(RestraintStateFlags.EngineOwned))
            {
                throw new ProtocolException(
                    "Free restraint state cannot retain an owner or engine constraint.");
            }
        }
        else if (!payload.OwnerEntityId.IsValid ||
                 payload.OwnerEntityId == payload.SubjectEntityId)
        {
            throw new ProtocolException(
                "An active restraint requires a distinct valid owner.");
        }
    }

    private static void ValidatePlayerIdentity(PlayerIdentityPayload payload)
    {
        if (!payload.EntityId.IsValid)
        {
            throw new ProtocolException(
                "Player identity requires non-zero NetEntityId epoch and counter.");
        }

        if (payload.Slot > (byte)SessionRole.Guest)
        {
            throw new ProtocolException(
                $"Player identity has unknown slot {payload.Slot}.");
        }

        try
        {
            _ = PlayerIdentityRules.ValidateNickname(payload.Nickname);
        }
        catch (ArgumentException exception)
        {
            throw new ProtocolException(
                "Player identity nickname is invalid.",
                exception);
        }
    }

    private static void ValidatePlayerAppearanceState(
        PlayerAppearanceStatePayload payload)
    {
        const PlayerAppearanceStateFlags allowedFlags =
            PlayerAppearanceStateFlags.CompleteComponentSet |
            PlayerAppearanceStateFlags.StoryMetaPed;
        if (!payload.EntityId.IsValid ||
            payload.Slot > (byte)SessionRole.Guest ||
            payload.SchemaVersion != 1 ||
            payload.Revision == 0 ||
            payload.ModelHash == 0 ||
            payload.Fingerprint == 0 ||
            (payload.Flags & ~allowedFlags) != 0 ||
            payload.ComponentHashes is null ||
            payload.ComponentHashes.Length is 0 or > PlayerAppearanceMaximumComponents)
        {
            throw new ProtocolException(
                "Player appearance contains an invalid identity, schema, model, revision, fingerprint, flags or component count.");
        }
        var unique = new HashSet<uint>();
        foreach (var component in payload.ComponentHashes)
        {
            if (component == 0 || !unique.Add(component))
            {
                throw new ProtocolException(
                    "Player appearance components must be non-zero and unique.");
            }
        }
    }

    private static void ValidatePlayerMountState(
        PlayerMountStatePayload payload)
    {
        const PlayerMountStateFlags allowedFlags =
            PlayerMountStateFlags.Present |
            PlayerMountStateFlags.Mounted |
            PlayerMountStateFlags.Dead |
            PlayerMountStateFlags.BorrowedPeerMount |
            PlayerMountStateFlags.Vehicle |
            PlayerMountStateFlags.VehicleDriver |
            PlayerMountStateFlags.VehiclePassenger;
        if (!payload.PlayerEntityId.IsValid ||
            !payload.MountEntityId.IsValid ||
            payload.PlayerEntityId == payload.MountEntityId ||
            payload.Slot > (byte)SessionRole.Guest ||
            (payload.Flags & ~allowedFlags) != 0)
        {
            throw new ProtocolException(
                "Player mount state contains an invalid identity, slot or flag.");
        }

        var present = (payload.Flags & PlayerMountStateFlags.Present) != 0;
        var mounted = (payload.Flags & PlayerMountStateFlags.Mounted) != 0;
        var borrowed =
            (payload.Flags & PlayerMountStateFlags.BorrowedPeerMount) != 0;
        var vehicle =
            (payload.Flags & PlayerMountStateFlags.Vehicle) != 0;
        var driver =
            (payload.Flags & PlayerMountStateFlags.VehicleDriver) != 0;
        var passenger =
            (payload.Flags & PlayerMountStateFlags.VehiclePassenger) != 0;
        if (borrowed && (!present || !mounted))
        {
            throw new ProtocolException(
                "A borrowed peer mount must be present and mounted.");
        }
        if (vehicle && (!present || !mounted || driver == passenger))
        {
            throw new ProtocolException(
                "A shared vehicle must be mounted with exactly one seat role.");
        }
        if (!vehicle && (driver || passenger))
        {
            throw new ProtocolException(
                "Vehicle seat flags require a shared vehicle.");
        }
        if (!present &&
            (payload.Flags != PlayerMountStateFlags.None ||
             payload.ModelHash != 0 ||
             payload.Position != Vector3.Zero ||
             payload.Velocity != Vector3.Zero ||
             payload.Heading != 0f ||
             payload.HealthFraction != 0f))
        {
            throw new ProtocolException(
                "An absent player mount requires zero state fields.");
        }

        if (present && payload.ModelHash == 0)
        {
            throw new ProtocolException(
                "A present player mount requires a model hash.");
        }

        if (!IsFinite(payload.Position) ||
            !IsFinite(payload.Velocity) ||
            !float.IsFinite(payload.Heading) ||
            payload.Heading is < 0f or >= 360f ||
            !float.IsFinite(payload.HealthFraction) ||
            payload.HealthFraction is < 0f or > 1f ||
            payload.Generation == 0)
        {
            throw new ProtocolException(
                "Player mount state contains an invalid numeric value.");
        }
    }

    private static void ValidateWorldEntityState(
        WorldEntityStatePayload payload)
    {
        const WorldEntityStateFlags allowedFlags =
            WorldEntityStateFlags.Human |
            WorldEntityStateFlags.Horse |
            WorldEntityStateFlags.Dead |
            WorldEntityStateFlags.InCombat |
            WorldEntityStateFlags.Firing |
            WorldEntityStateFlags.Aiming |
            WorldEntityStateFlags.Mounted |
            WorldEntityStateFlags.ScriptOwned;
        if (!payload.EntityId.IsValid)
        {
            throw new ProtocolException(
                "World entity state requires non-zero NetEntityId epoch and counter.");
        }

        if (payload.ModelHash == 0)
        {
            throw new ProtocolException(
                "World entity state requires a non-zero model hash.");
        }

        if (!Enum.IsDefined(payload.Kind))
        {
            throw new ProtocolException(
                $"Unknown world entity kind {(byte)payload.Kind}.");
        }

        if ((payload.Flags & ~allowedFlags) != 0)
        {
            throw new ProtocolException(
                $"World entity state contains unknown flags 0x{(byte)payload.Flags:X2}.");
        }

        if (!Enum.IsDefined(payload.CombatTargetSlot))
        {
            throw new ProtocolException(
                $"Unknown world combat target slot {(byte)payload.CombatTargetSlot}.");
        }

        if (!Enum.IsDefined(payload.TaskKind))
        {
            throw new ProtocolException(
                $"Unknown world task kind {(byte)payload.TaskKind}.");
        }

        var inCombat =
            (payload.Flags & WorldEntityStateFlags.InCombat) != 0;
        if (!inCombat &&
            payload.CombatTargetSlot != WorldCombatTargetSlot.None)
        {
            throw new ProtocolException(
                "World entity outside combat requires target slot None.");
        }

        var human =
            (payload.Flags & WorldEntityStateFlags.Human) != 0;
        var horse =
            (payload.Flags & WorldEntityStateFlags.Horse) != 0;
        if (human && horse)
        {
            throw new ProtocolException(
                "World entity state cannot be both Human and Horse.");
        }

        var mounted =
            (payload.Flags & WorldEntityStateFlags.Mounted) != 0;
        if (payload.Kind == WorldEntityKind.Object &&
            (human || horse || inCombat || mounted ||
             payload.CombatTargetSlot != WorldCombatTargetSlot.None ||
             payload.ParentEntityId.IsValid || payload.WeaponHash != 0 ||
             (payload.TaskKind != WorldTaskKind.Idle &&
              payload.TaskKind != WorldTaskKind.Cinematic)))
        {
            throw new ProtocolException(
                "World object state contains ped-only semantics.");
        }
        if (mounted != payload.ParentEntityId.IsValid ||
            (mounted && (!human ||
                         payload.TaskKind != WorldTaskKind.Mounted ||
                         payload.ParentEntityId == payload.EntityId)))
        {
            throw new ProtocolException(
                "Mounted world riders require a distinct parent mount entity.");
        }

        if (payload.TaskKind == WorldTaskKind.Dead &&
            (payload.Flags & WorldEntityStateFlags.Dead) == 0)
        {
            throw new ProtocolException(
                "Dead world task requires the Dead flag.");
        }

        if (!human && payload.WeaponHash != 0)
        {
            throw new ProtocolException(
                "Non-human world entity state requires a zero weapon hash.");
        }

        var usesWeapon =
            (payload.Flags &
             (WorldEntityStateFlags.Firing |
              WorldEntityStateFlags.Aiming)) != 0;
        if (usesWeapon &&
            (!human || payload.WeaponHash == 0))
        {
            throw new ProtocolException(
                "Aiming or firing world entities must be armed humans.");
        }

        if (!IsFinite(payload.Position) ||
            !IsFinite(payload.Velocity) ||
            !float.IsFinite(payload.Heading) ||
            payload.Heading is < 0f or >= 360f ||
            !float.IsFinite(payload.HealthFraction) ||
            payload.HealthFraction is < 0f or > 1f ||
            !IsFinite(payload.TaskTarget))
        {
            throw new ProtocolException(
                "World entity state contains an invalid numeric value.");
        }
    }

    private static void ValidateEntityDespawn(EntityDespawnPayload payload)
    {
        if (!payload.EntityId.IsValid)
        {
            throw new ProtocolException(
                "Entity despawn requires non-zero NetEntityId epoch and counter.");
        }
    }

    private static void ValidateDamageIntent(DamageIntentPayload payload)
    {
        if (!payload.AttackerId.IsValid ||
            !payload.TargetId.IsValid)
        {
            throw new ProtocolException(
                "Damage intent requires valid attacker and target entity IDs.");
        }

        if (payload.WeaponHash == 0)
        {
            throw new ProtocolException(
                "Damage intent requires a non-zero weapon hash.");
        }

        if (!float.IsFinite(payload.Damage) ||
            payload.Damage is <= 0f or > 100f)
        {
            throw new ProtocolException(
                "Damage intent must be finite and in the range (0, 100].");
        }

        if (payload.ShotSequence == 0)
        {
            throw new ProtocolException(
                "Damage intent requires a non-zero shot sequence.");
        }
    }

    private static void ValidateWorldState(WorldStatePayload payload)
    {
        const WorldStateFlags allowedFlags = WorldStateFlags.WeatherValid;
        if (payload.Hour > 23 ||
            payload.Minute > 59 ||
            payload.Second > 59 ||
            payload.Day is < 1 or > 31 ||
            payload.Month > 11 ||
            payload.Year is < 1800 or > 2200)
        {
            throw new ProtocolException(
                "World state clock/date is outside the supported range.");
        }

        if ((payload.Flags & ~allowedFlags) != 0)
        {
            throw new ProtocolException(
                $"World state contains unknown flags 0x{(byte)payload.Flags:X2}.");
        }

        if (!float.IsFinite(payload.Blend) ||
            payload.Blend is < 0f or > 1f)
        {
            throw new ProtocolException(
                "World state weather blend must be finite and between 0 and 1.");
        }

        var weatherValid =
            (payload.Flags & WorldStateFlags.WeatherValid) != 0;
        if (weatherValid &&
            (payload.WeatherFrom == 0 || payload.WeatherTo == 0))
        {
            throw new ProtocolException(
                "World state with WeatherValid requires both weather hashes.");
        }

        if (!weatherValid &&
            (payload.WeatherFrom != 0 ||
             payload.WeatherTo != 0 ||
             payload.Blend != 0f))
        {
            throw new ProtocolException(
                "World state without WeatherValid requires zero weather fields.");
        }
    }

    private static void ValidateMissionState(MissionStatePayload payload)
    {
        const MissionStateFlags allowedFlags =
            MissionStateFlags.AnchorValid |
            MissionStateFlags.MissionActive |
            MissionStateFlags.CheckpointRecovery |
            MissionStateFlags.ScriptedControlLock |
            MissionStateFlags.ScreenTransition |
            MissionStateFlags.ScenarioActivity |
            MissionStateFlags.ScriptedVehicleTransition |
            MissionStateFlags.MinigameActivity;
        if (!payload.HostEntityId.IsValid ||
            payload.MissionEpoch == 0 ||
            payload.Revision == 0)
        {
            throw new ProtocolException(
                "Mission state requires a valid host entity and non-zero epoch/revision.");
        }

        if (!Enum.IsDefined(payload.Phase))
        {
            throw new ProtocolException(
                $"Mission state contains unknown phase {(byte)payload.Phase}.");
        }

        if ((payload.Flags & ~allowedFlags) != 0)
        {
            throw new ProtocolException(
                $"Mission state contains unknown flags 0x{(byte)payload.Flags:X2}.");
        }

        if (!IsFinite(payload.HostAnchor) ||
            !float.IsFinite(payload.HostHeading))
        {
            throw new ProtocolException(
                "Mission state anchor and heading must be finite.");
        }

        var anchorValid = payload.Flags.HasFlag(MissionStateFlags.AnchorValid);
        if (!anchorValid &&
            (payload.HostAnchor != Vector3.Zero || payload.HostHeading != 0f))
        {
            throw new ProtocolException(
                "Mission state without AnchorValid requires a zero anchor and heading.");
        }

        var missionActive = payload.Flags.HasFlag(MissionStateFlags.MissionActive);
        if ((payload.Phase == MissionPhase.Idle && missionActive) ||
            (payload.Phase == MissionPhase.Active && !missionActive))
        {
            throw new ProtocolException(
                "Mission state phase and MissionActive flag are inconsistent.");
        }

        var checkpointRecovery =
            payload.Flags.HasFlag(MissionStateFlags.CheckpointRecovery);
        if (checkpointRecovery != (payload.Phase == MissionPhase.Recovery))
        {
            throw new ProtocolException(
                "Mission state recovery phase and CheckpointRecovery flag are inconsistent.");
        }
    }

    private static void ValidateMissionCameraState(
        MissionCameraStatePayload payload)
    {
        const MissionCameraStateFlags allowedFlags =
            MissionCameraStateFlags.Active |
            MissionCameraStateFlags.ScreenFadedOut |
            MissionCameraStateFlags.ScreenFadingOut |
            MissionCameraStateFlags.ScreenFadingIn |
            MissionCameraStateFlags.SourceRenderingScriptCamera |
            MissionCameraStateFlags.SourceCinematicGameplayCamera |
            MissionCameraStateFlags.SourceGameplayCameraFallback;
        if (!payload.HostEntityId.IsValid ||
            payload.MissionEpoch == 0 ||
            payload.CinematicGeneration == 0 ||
            payload.Revision == 0)
        {
            throw new ProtocolException(
                "Mission camera state requires a valid host entity and non-zero epoch/generation/revision.");
        }

        var fadeFlags = payload.Flags &
            (MissionCameraStateFlags.ScreenFadedOut |
             MissionCameraStateFlags.ScreenFadingOut |
             MissionCameraStateFlags.ScreenFadingIn);
        var fadeBits = (uint)fadeFlags;
        if (fadeBits != 0 && (fadeBits & (fadeBits - 1)) != 0)
        {
            throw new ProtocolException(
                "Mission camera state may contain only one fade flag.");
        }

        var sourceFlags = payload.Flags &
            (MissionCameraStateFlags.SourceRenderingScriptCamera |
             MissionCameraStateFlags.SourceCinematicGameplayCamera |
             MissionCameraStateFlags.SourceGameplayCameraFallback);
        var sourceBits = (uint)sourceFlags;
        var active = payload.Flags.HasFlag(MissionCameraStateFlags.Active);
        if (active &&
            (sourceBits == 0 || (sourceBits & (sourceBits - 1)) != 0))
        {
            throw new ProtocolException(
                "An active mission camera requires exactly one camera source flag.");
        }
        if (!active && sourceBits != 0)
        {
            throw new ProtocolException(
                "An inactive mission camera cannot contain a camera source flag.");
        }

        if ((payload.Flags & ~allowedFlags) != 0)
        {
            throw new ProtocolException(
                $"Mission camera state contains unknown flags 0x{(uint)payload.Flags:X8}.");
        }

        if (!IsFinite(payload.Position) ||
            !IsFinite(payload.Rotation) ||
            !float.IsFinite(payload.FieldOfView))
        {
            throw new ProtocolException(
                "Mission camera transform and field of view must be finite.");
        }

        if (active && payload.FieldOfView is < 1f or > 179f)
        {
            throw new ProtocolException(
                "An active mission camera requires a field of view in [1, 179].");
        }

        if (!active &&
            (payload.Position != Vector3.Zero ||
             payload.Rotation != Vector3.Zero ||
             payload.FieldOfView != 0f))
        {
            throw new ProtocolException(
                "An inactive mission camera requires a zero transform and field of view.");
        }
    }

    private static void ValidateMissionCinematicState(
        MissionCinematicStatePayload payload)
    {
        const MissionCinematicStateFlags allowedFlags =
            MissionCinematicStateFlags.CameraExpected |
            MissionCinematicStateFlags.AnchorValid |
            MissionCinematicStateFlags.SkipPending |
            MissionCinematicStateFlags.ResumeTimedOut;
        if (!payload.HostEntityId.IsValid ||
            payload.MissionEpoch == 0 ||
            payload.CinematicGeneration == 0 ||
            payload.Revision == 0 ||
            payload.CheckpointGeneration == 0 ||
            !Enum.IsDefined(payload.Phase))
        {
            throw new ProtocolException(
                "Mission cinematic state contains an invalid identity, generation, revision, checkpoint, or phase.");
        }

        if ((payload.Flags & ~allowedFlags) != 0)
        {
            throw new ProtocolException(
                $"Mission cinematic state contains unknown flags 0x{(ushort)payload.Flags:X4}.");
        }

        if (!IsFinite(payload.ResumeAnchor) ||
            !float.IsFinite(payload.ResumeHeading))
        {
            throw new ProtocolException(
                "Mission cinematic resume anchor and heading must be finite.");
        }

        var anchorValid = payload.Flags.HasFlag(
            MissionCinematicStateFlags.AnchorValid);
        if (!anchorValid &&
            (payload.ResumeAnchor != Vector3.Zero || payload.ResumeHeading != 0f))
        {
            throw new ProtocolException(
                "Mission cinematic state without AnchorValid requires a zero anchor and heading.");
        }

        if (payload.Phase is MissionCinematicPhase.PrepareResume or
            MissionCinematicPhase.Completed && !anchorValid)
        {
            throw new ProtocolException(
                "PrepareResume and Completed require a valid resume anchor.");
        }

        if (payload.Flags.HasFlag(MissionCinematicStateFlags.ResumeTimedOut) &&
            payload.Phase is not MissionCinematicPhase.Completed and
            not MissionCinematicPhase.Aborted)
        {
            throw new ProtocolException(
                "ResumeTimedOut is valid only for Completed or Aborted.");
        }

        if (payload.Flags.HasFlag(MissionCinematicStateFlags.SkipPending) &&
            payload.Phase is not MissionCinematicPhase.Playing and
            not MissionCinematicPhase.Loading)
        {
            throw new ProtocolException(
                "SkipPending is valid only while a cinematic is playing or loading.");
        }
    }

    private static void ValidateMissionCinematicAction(
        MissionCinematicActionPayload payload)
    {
        const MissionCinematicActionFlags allowedFlags =
            MissionCinematicActionFlags.FallbackUsed;
        if (!payload.HostEntityId.IsValid ||
            payload.MissionEpoch == 0 ||
            payload.CinematicGeneration == 0 ||
            payload.ActionId == 0 ||
            !Enum.IsDefined(payload.Kind) ||
            payload.SenderSlot > 1)
        {
            throw new ProtocolException(
                "Mission cinematic action contains an invalid identity, generation, action id, kind, or sender slot.");
        }

        if ((payload.Flags & ~allowedFlags) != 0)
        {
            throw new ProtocolException(
                $"Mission cinematic action contains unknown flags 0x{(ushort)payload.Flags:X4}.");
        }

        if (payload.Flags.HasFlag(MissionCinematicActionFlags.FallbackUsed) &&
            payload.Kind != MissionCinematicActionKind.ResumeReady)
        {
            throw new ProtocolException(
                "FallbackUsed is valid only for ResumeReady.");
        }
    }

    private static void ValidateAnimSceneReplicaState(
        AnimSceneReplicaStatePayload payload)
    {
        const AnimSceneReplicaStateFlags allowedFlags =
            AnimSceneReplicaStateFlags.Active |
            AnimSceneReplicaStateFlags.Running |
            AnimSceneReplicaStateFlags.Loaded |
            AnimSceneReplicaStateFlags.CameraActive |
            AnimSceneReplicaStateFlags.OriginValid;
        var originValid = payload.Flags.HasFlag(
            AnimSceneReplicaStateFlags.OriginValid);
        if (!payload.HostEntityId.IsValid ||
            payload.MissionEpoch == 0 ||
            payload.CinematicGeneration == 0 ||
            payload.Revision == 0 ||
            payload.DictionaryHash == 0 ||
            !payload.Flags.HasFlag(AnimSceneReplicaStateFlags.Active) ||
            (payload.Flags & ~allowedFlags) != 0 ||
            !float.IsFinite(payload.Phase) ||
            payload.Phase is < 0 or > 1.05f ||
            !float.IsFinite(payload.DurationSeconds) ||
            payload.DurationSeconds is <= 0 or > 7200 ||
            !float.IsFinite(payload.Rate) ||
            payload.Rate is < 0 or > 4 ||
            !IsFinite(payload.OriginPosition) ||
            !IsFinite(payload.OriginRotation) ||
            (!originValid &&
             (payload.OriginPosition != Vector3.Zero ||
              payload.OriginRotation != Vector3.Zero)) ||
            payload.ActiveCameraCount > 32)
        {
            throw new ProtocolException(
                "AnimScene replica state contains invalid identity, phase, timing, origin or flags.");
        }
    }

    private static void ValidateAnimSceneDefinition(
        AnimSceneDefinitionPayload payload)
    {
        ValidateAnimSceneDefinitionShape(payload);
        if ((payload.FingerprintLow | payload.FingerprintHigh) == 0)
        {
            throw new ProtocolException(
                "AnimScene definition fingerprint must not be all zero.");
        }

        var expected = ComputeAnimSceneDefinitionFingerprint(payload);
        if (payload.FingerprintLow != expected.Low ||
            payload.FingerprintHigh != expected.High)
        {
            throw new ProtocolException(
                "AnimScene definition fingerprint does not match its canonical payload.");
        }
    }

    private static void ValidateAnimSceneDefinitionShape(
        AnimSceneDefinitionPayload payload)
    {
        if (payload is null)
        {
            throw new ProtocolException(
                "AnimScene definition payload must not be null.");
        }
        if (!payload.HostEntityId.IsValid ||
            payload.MissionEpoch == 0 ||
            payload.CinematicGeneration == 0 ||
            payload.DefinitionRevision == 0 ||
            payload.DictionaryHash == 0 ||
            (payload.CreateOptionFlags & ~0x03) != 0 ||
            !float.IsFinite(payload.DurationSeconds) ||
            payload.DurationSeconds is <= 0 or > 7200)
        {
            throw new ProtocolException(
                "AnimScene definition contains an invalid identity, revision, dictionary, or duration.");
        }
        if (payload.Roles is null ||
            payload.Roles.Length > AnimSceneDefinitionMaximumRoles)
        {
            throw new ProtocolException(
                $"AnimScene definition may contain at most {AnimSceneDefinitionMaximumRoles} roles.");
        }

        var resourceLength = ValidatePrintableAscii(
            payload.ResourceName,
            allowEmpty: false,
            AnimSceneResourceMaximumBytes,
            "AnimScene resource name");
        var playbackLength = ValidatePrintableAscii(
            payload.PlaybackList,
            allowEmpty: true,
            AnimScenePlaybackListMaximumBytes,
            "AnimScene playback list");
        var encodedSize = checked(
            AnimSceneDefinitionHeaderSize + resourceLength + playbackLength);
        string? previousRoleName = null;
        var mappedEntities = new HashSet<ulong>();
        foreach (var role in payload.Roles)
        {
            if (role is null)
            {
                throw new ProtocolException(
                    "AnimScene definition roles must not contain null entries.");
            }
            var roleNameLength = ValidatePrintableAscii(
                role.RoleName,
                allowEmpty: false,
                AnimSceneRoleNameMaximumBytes,
                "AnimScene role name");
            if (previousRoleName is not null &&
                string.CompareOrdinal(previousRoleName, role.RoleName) >= 0)
            {
                throw new ProtocolException(
                    "AnimScene roles must have unique names in strict ordinal order.");
            }
            previousRoleName = role.RoleName;

            const AnimSceneRoleFlags allowedFlags =
                AnimSceneRoleFlags.Required | AnimSceneRoleFlags.Player;
            if (!Enum.IsDefined(role.Kind) ||
                (role.Flags & ~allowedFlags) != 0)
            {
                throw new ProtocolException(
                    "AnimScene role contains an unknown kind or flag.");
            }

            var mapped = role.EntityId.IsValid;
            if (role.EntityId.Value != 0 && !mapped)
            {
                throw new ProtocolException(
                    "AnimScene role contains a partially invalid NetEntityId.");
            }
            if (mapped &&
                (!mappedEntities.Add(role.EntityId.Value) || role.ModelHash == 0))
            {
                throw new ProtocolException(
                    "AnimScene role mappings must use unique entities and non-zero model hashes.");
            }
            if (!mapped && (role.ModelHash != 0 || role.BindingFlags != 0))
            {
                throw new ProtocolException(
                    "An unbound AnimScene role must clear model and binding fields.");
            }
            if ((role.Flags &
                 (AnimSceneRoleFlags.Required | AnimSceneRoleFlags.Player)) != 0 &&
                !mapped)
            {
                throw new ProtocolException(
                    "Required and player AnimScene roles need a network entity mapping.");
            }
            if (role.Flags.HasFlag(AnimSceneRoleFlags.Player) &&
                role.Kind != AnimSceneRoleKind.Ped)
            {
                throw new ProtocolException(
                    "A player AnimScene role must use the Ped kind.");
            }
            encodedSize = checked(
                encodedSize + AnimSceneRoleBindingHeaderSize + roleNameLength);
        }
        if (encodedSize > AnimSceneDefinitionMaximumSize)
        {
            throw new ProtocolException(
                $"AnimScene definition exceeds {AnimSceneDefinitionMaximumSize} bytes.");
        }
    }

    private static void ValidateAnimSceneControl(AnimSceneControlPayload payload)
    {
        const AnimSceneControlFlags allowedFlags =
            AnimSceneControlFlags.ResourceLoaded |
            AnimSceneControlFlags.RequiredRolesBound |
            AnimSceneControlFlags.CacheHit |
            AnimSceneControlFlags.LateJoin |
            AnimSceneControlFlags.FallbackUsed;
        if (!payload.HostEntityId.IsValid ||
            payload.MissionEpoch == 0 ||
            payload.CinematicGeneration == 0 ||
            payload.DefinitionRevision == 0 ||
            payload.ActionId == 0 ||
            (payload.FingerprintLow | payload.FingerprintHigh) == 0 ||
            !Enum.IsDefined(payload.Kind) ||
            !Enum.IsDefined(payload.Reason) ||
            payload.SenderSlot > 1 ||
            (payload.Flags & ~allowedFlags) != 0 ||
            !float.IsFinite(payload.StartPhase) ||
            !float.IsFinite(payload.Rate))
        {
            throw new ProtocolException(
                "AnimScene control contains invalid identity, fingerprint, kind, sender, reason, flags, or timing.");
        }

        switch (payload.Kind)
        {
            case AnimSceneControlKind.GuestReady:
            {
                const AnimSceneControlFlags readyFlags =
                    AnimSceneControlFlags.ResourceLoaded |
                    AnimSceneControlFlags.RequiredRolesBound |
                    AnimSceneControlFlags.CacheHit;
                var required =
                    AnimSceneControlFlags.ResourceLoaded |
                    AnimSceneControlFlags.RequiredRolesBound;
                if (payload.SenderSlot != 1 ||
                    payload.Reason != AnimSceneControlReason.None ||
                    payload.PlayAtHostTick != 0 ||
                    payload.StartPhase != 0 ||
                    payload.Rate != 0 ||
                    (payload.Flags & required) != required ||
                    (payload.Flags & ~readyFlags) != 0)
                {
                    throw new ProtocolException(
                        "GuestReady must be a zero-timing guest acknowledgement with loaded and bound flags.");
                }
                break;
            }
            case AnimSceneControlKind.GuestRejected:
                if (payload.SenderSlot != 1 ||
                    payload.Reason == AnimSceneControlReason.None ||
                    payload.PlayAtHostTick != 0 ||
                    payload.StartPhase != 0 ||
                    payload.Rate != 0 ||
                    payload.Flags != AnimSceneControlFlags.None)
                {
                    throw new ProtocolException(
                        "GuestRejected must carry only a guest rejection reason.");
                }
                break;
            case AnimSceneControlKind.HostPlayCommit:
                if (payload.SenderSlot != 0 ||
                    payload.Reason != AnimSceneControlReason.None ||
                    payload.PlayAtHostTick == 0 ||
                    payload.StartPhase is < 0 or > 1.05f ||
                    payload.Rate is <= 0 or > 4 ||
                    (payload.Flags & ~AnimSceneControlFlags.LateJoin) != 0)
                {
                    throw new ProtocolException(
                        "HostPlayCommit requires host authority and valid scheduled playback timing.");
                }
                break;
            case AnimSceneControlKind.HostAbort:
                if (payload.SenderSlot != 0 ||
                    payload.Reason == AnimSceneControlReason.None ||
                    payload.PlayAtHostTick != 0 ||
                    payload.StartPhase != 0 ||
                    payload.Rate != 0 ||
                    (payload.Flags & ~AnimSceneControlFlags.FallbackUsed) != 0)
                {
                    throw new ProtocolException(
                        "HostAbort must carry only a host abort reason and optional fallback flag.");
                }
                break;
            default:
                throw new ProtocolException("AnimScene control kind is unknown.");
        }
    }

    private static byte[] EncodePrintableAscii(
        string value,
        bool allowEmpty,
        int maximumBytes,
        string fieldName)
    {
        _ = ValidatePrintableAscii(value, allowEmpty, maximumBytes, fieldName);
        return Encoding.ASCII.GetBytes(value);
    }

    private static string DecodePrintableAscii(
        ReadOnlySpan<byte> bytes,
        bool allowEmpty,
        int maximumBytes,
        string fieldName)
    {
        if ((!allowEmpty && bytes.IsEmpty) || bytes.Length > maximumBytes)
        {
            throw new ProtocolException(
                $"{fieldName} length is outside the protocol limit.");
        }
        foreach (var value in bytes)
        {
            if (value is < 0x20 or > 0x7E)
            {
                throw new ProtocolException(
                    $"{fieldName} must contain printable ASCII only.");
            }
        }
        return Encoding.ASCII.GetString(bytes);
    }

    private static int ValidatePrintableAscii(
        string value,
        bool allowEmpty,
        int maximumBytes,
        string fieldName)
    {
        if (value is null || (!allowEmpty && value.Length == 0) ||
            value.Length > maximumBytes)
        {
            throw new ProtocolException(
                $"{fieldName} length is outside the protocol limit.");
        }
        foreach (var character in value)
        {
            if (character is < (char)0x20 or > (char)0x7E)
            {
                throw new ProtocolException(
                    $"{fieldName} must contain printable ASCII only.");
            }
        }
        return value.Length;
    }

    private static void ValidateEquipmentState(EquipmentStatePayload payload)
    {
        const EquipmentStateFlags allowedFlags =
            EquipmentStateFlags.Equipped |
            EquipmentStateFlags.Reloading;
        if (!payload.EntityId.IsValid)
        {
            throw new ProtocolException(
                "Equipment state requires non-zero NetEntityId epoch and counter.");
        }

        if ((payload.Flags & ~allowedFlags) != 0)
        {
            throw new ProtocolException(
                $"Equipment state contains unknown flags 0x{(uint)payload.Flags:X8}.");
        }
    }

    private static void ValidatePauseVote(PauseVotePayload payload)
    {
        const PauseVoteFlags allowedFlags =
            PauseVoteFlags.HostVoted |
            PauseVoteFlags.GuestVoted |
            PauseVoteFlags.Paused;
        if (!Enum.IsDefined(payload.Kind) || payload.VoterSlot > 1)
        {
            throw new ProtocolException(
                "Pause vote contains an unknown kind or player slot.");
        }

        if ((payload.Flags & ~allowedFlags) != 0)
        {
            throw new ProtocolException(
                $"Pause vote contains unknown flags 0x{(byte)payload.Flags:X2}.");
        }

        if (payload.Kind == PauseVoteKind.RequestState &&
            (payload.Flags & ~PauseVoteFlags.Paused) != 0)
        {
            throw new ProtocolException(
                "Pause-state request may only carry the desired paused flag.");
        }
    }

    private static void ValidateCampaignCapability(CampaignCapabilityPayload payload)
    {
        if (!Enum.IsDefined(payload.Kind) || payload.RecordHash == 0 || payload.HostEventId == 0 || payload.GrantedAtUnixMilliseconds <= 0)
            throw new ProtocolException("Campaign capability payload is invalid.");
    }

    private static void ValidateMissionProgression(MissionProgressionPayload payload)
    {
        const MissionProgressionFlags allowed =
            MissionProgressionFlags.GuestCanStart |
            MissionProgressionFlags.VerifiedCompletionMapping;
        var completion = payload.Phase == MissionProgressionPhase.Completion;
        var applied = payload.Phase == MissionProgressionPhase.Applied;
        var appliesMapping =
            (payload.Flags & MissionProgressionFlags.VerifiedCompletionMapping) != 0;
        if (payload.MissionId == 0 || payload.MissionEpoch == 0 ||
            payload.EventId == 0 || !Enum.IsDefined(payload.Phase) ||
            (payload.Flags & ~allowed) != 0 ||
            (!completion && payload.CompletionRating != 0) ||
            (!completion && payload.CompletionCashAward != 0) ||
            payload.CompletionCashAward < 0 ||
            (appliesMapping && (!completion || payload.CompletionRating is < 2 or > 5)) ||
            (applied && payload.Flags != MissionProgressionFlags.None))
        {
            throw new ProtocolException(
                "Mission progression requires a catalog mission, epoch, event and valid flags.");
        }
    }

    private static void ValidateMissionObjective(MissionObjectivePayload payload)
    {
        if (!payload.HostEntityId.IsValid || payload.MissionEpoch == 0 ||
            payload.Revision == 0 || payload.Fingerprint == 0 ||
            string.IsNullOrWhiteSpace(payload.Text) ||
            Encoding.UTF8.GetByteCount(payload.Text) > MaximumMissionObjectiveUtf8Bytes ||
            payload.Text.Any(char.IsControl))
            throw new ProtocolException("Mission objective payload is invalid.");
    }

    private static void ValidateMissionDialogueCue(MissionDialogueCuePayload payload)
    {
        if (!payload.HostEntityId.IsValid || payload.MissionEpoch == 0 ||
            payload.CheckpointGeneration == 0 || payload.DialogueSequence == 0 ||
            payload.ProfileId == 0 || payload.RootId == 0 || payload.HostStartTick == 0)
            throw new ProtocolException("Mission dialogue cue payload is invalid.");
    }

    private static void ValidateMissionDialogueReady(MissionDialogueReadyPayload payload)
    {
        if (!payload.HostEntityId.IsValid || payload.MissionEpoch == 0 ||
            payload.CheckpointGeneration == 0 || payload.DialogueSequence == 0 ||
            payload.ProfileId == 0 || payload.RootId == 0 ||
            !Enum.IsDefined(payload.State))
            throw new ProtocolException("Mission dialogue readiness payload is invalid.");
    }

    private static void ValidateAmbientEncounterProposal(AmbientEncounterProposalPayload payload)
    {
        if (!payload.GuestEntityId.IsValid || payload.ProposalId == 0 ||
            !Enum.IsDefined(payload.Profile) || !IsFinite(payload.Anchor) ||
            !float.IsFinite(payload.RadiusMeters) || payload.RadiusMeters is < 8 or > 80 ||
            payload.LocalEvidenceHash == 0 || payload.SuggestedRosterSeed == 0)
            throw new ProtocolException("Ambient encounter proposal is invalid.");
    }

    private static void ValidateAmbientEncounterState(AmbientEncounterStatePayload payload)
    {
        var rejectedProposal = payload.Phase == AmbientEncounterPhase.Proposed &&
            payload.Rejection != AmbientEncounterRejection.None;
        if (!payload.HostEntityId.IsValid || payload.InstanceId == 0 ||
            !Enum.IsDefined(payload.Profile) || !Enum.IsDefined(payload.Phase) ||
            !Enum.IsDefined(payload.Rejection) || !Enum.IsDefined(payload.GuestDisposition))
            throw new ProtocolException("Ambient encounter state is invalid.");
        if (rejectedProposal)
        {
            if (payload.RosterCount != 0 || payload.HostStartTick != 0 ||
                payload.ExactEventId != 0 ||
                payload.GuestDisposition != AmbientEncounterPeerDisposition.Unknown)
                throw new ProtocolException("Rejected ambient encounter state is invalid.");
            return;
        }
        if (payload.Rejection != AmbientEncounterRejection.None ||
            payload.Phase == AmbientEncounterPhase.Proposed || !IsFinite(payload.Anchor) ||
            !float.IsFinite(payload.RadiusMeters) || payload.RadiusMeters is < 8 or > 80 ||
            payload.RosterSeed == 0 || payload.RosterCount is 0 or > 12 || payload.HostStartTick == 0)
            throw new ProtocolException("Accepted ambient encounter state is invalid.");
        if ((payload.ExactEventId == 0 &&
             payload.GuestDisposition != AmbientEncounterPeerDisposition.Unknown) ||
            (payload.ExactEventId != 0 &&
             payload.Profile != AmbientEncounterProfile.HostageRescue) ||
            (payload.ExactEventId != 0 &&
             payload.Phase != AmbientEncounterPhase.Preparing &&
             payload.GuestDisposition == AmbientEncounterPeerDisposition.Unknown))
            throw new ProtocolException("Exact ambient encounter state is invalid.");
    }

    private static void ValidateCampaignCapabilityAck(CampaignCapabilityAckPayload payload)
    {
        if (!Enum.IsDefined(payload.Kind) || payload.RecordHash == 0 || payload.HostEventId == 0)
            throw new ProtocolException("Campaign capability acknowledgement is invalid.");
    }

    private static void ValidatePickupCollected(PickupCollectedPayload payload)
    {
        if (!payload.ActorEntityId.IsValid || payload.CollectionId == 0 || payload.PickupHash == 0)
            throw new ProtocolException("Pickup collection payload is invalid.");
    }

    private static bool IsFinite(Vector3 value) =>
        float.IsFinite(value.X) &&
        float.IsFinite(value.Y) &&
        float.IsFinite(value.Z);

    private static void WriteVector3(Span<byte> destination, Vector3 value)
    {
        BinaryPrimitives.WriteSingleLittleEndian(destination, value.X);
        BinaryPrimitives.WriteSingleLittleEndian(destination[4..], value.Y);
        BinaryPrimitives.WriteSingleLittleEndian(destination[8..], value.Z);
    }

    private static Vector3 ReadVector3(ReadOnlySpan<byte> source) =>
        new(
            BinaryPrimitives.ReadSingleLittleEndian(source),
            BinaryPrimitives.ReadSingleLittleEndian(source[4..]),
            BinaryPrimitives.ReadSingleLittleEndian(source[8..]));

    private static void RequireLength(ReadOnlySpan<byte> payload, int expected, string name)
    {
        if (payload.Length != expected)
        {
            throw new ProtocolException(
                $"{name} payload must contain exactly {expected} bytes; received {payload.Length}.");
        }
    }
}
