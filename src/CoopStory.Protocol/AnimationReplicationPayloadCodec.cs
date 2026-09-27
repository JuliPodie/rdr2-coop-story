using System.Buffers.Binary;

namespace CoopStory.Protocol;

/// <summary>
/// Binary codecs for the optional AnimGraph replication lane.
/// The payloads are independently schema-versioned in addition to the frame protocol.
/// </summary>
public static class AnimationReplicationPayloadCodec
{
    public const byte PlayerAnimationStateSchemaVersion = 1;
    public const byte MotionReplicationConfigSchemaVersion = 1;
    public const int PlayerAnimationStateSize = 72;
    public const int MotionReplicationConfigSize = 8;

    private const float MaximumAbsolutePlaybackRate = 8f;

    private const PlayerAnimationCapabilities KnownCapabilities =
        PlayerAnimationCapabilities.GraphIdentifier |
        PlayerAnimationCapabilities.StateIdentifier |
        PlayerAnimationCapabilities.ClipIdentifiers |
        PlayerAnimationCapabilities.NormalizedPhase |
        PlayerAnimationCapabilities.PlaybackRate |
        PlayerAnimationCapabilities.BlendWeights |
        PlayerAnimationCapabilities.TransitionProgress |
        PlayerAnimationCapabilities.RuntimeFlags;

    private const PlayerAnimationStateFlags KnownStateFlags =
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

    // Optional high-detail layer over PlayerState.
    // It identifies the source animation graph/clip only when the capability and validity flags agree.
    public static byte[] EncodePlayerAnimationState(
        PlayerAnimationStatePayload payload)
    {
        ValidatePlayerAnimationState(payload);
        var bytes = new byte[PlayerAnimationStateSize];
        var span = bytes.AsSpan();
        BinaryPrimitives.WriteUInt64LittleEndian(span, payload.EntityId.Value);
        span[8] = payload.Slot;
        span[9] = payload.SchemaVersion;
        BinaryPrimitives.WriteUInt16LittleEndian(span[10..], payload.LocomotionEpoch);
        BinaryPrimitives.WriteUInt32LittleEndian(span[12..], payload.SampleSequence);
        BinaryPrimitives.WriteUInt32LittleEndian(span[16..], (uint)payload.Capabilities);
        BinaryPrimitives.WriteUInt32LittleEndian(span[20..], (uint)payload.Flags);
        BinaryPrimitives.WriteUInt32LittleEndian(span[24..], payload.GraphHash);
        BinaryPrimitives.WriteUInt32LittleEndian(span[28..], payload.StateHash);
        BinaryPrimitives.WriteUInt32LittleEndian(span[32..], payload.PrimaryClipHash);
        BinaryPrimitives.WriteUInt32LittleEndian(span[36..], payload.SecondaryClipHash);
        BinaryPrimitives.WriteSingleLittleEndian(span[40..], payload.PrimaryNormalizedPhase);
        BinaryPrimitives.WriteSingleLittleEndian(span[44..], payload.SecondaryNormalizedPhase);
        BinaryPrimitives.WriteSingleLittleEndian(span[48..], payload.PrimaryPlaybackRate);
        BinaryPrimitives.WriteSingleLittleEndian(span[52..], payload.SecondaryPlaybackRate);
        BinaryPrimitives.WriteSingleLittleEndian(span[56..], payload.PrimaryBlendWeight);
        BinaryPrimitives.WriteSingleLittleEndian(span[60..], payload.SecondaryBlendWeight);
        BinaryPrimitives.WriteSingleLittleEndian(span[64..], payload.TransitionProgress);
        span[68] = (byte)payload.Source;
        // 69..71 are reserved for a future schema and remain zero.
        return bytes;
    }

    public static PlayerAnimationStatePayload DecodePlayerAnimationState(
        ReadOnlySpan<byte> payload)
    {
        RequireLength(payload, PlayerAnimationStateSize, nameof(PlayerAnimationStatePayload));
        if (payload[69] != 0 || payload[70] != 0 || payload[71] != 0)
        {
            throw new ProtocolException(
                "Player animation state reserved field must be zero.");
        }

        // Read fixed offsets first, then validate the cross-field contract so a peer cannot claim a phase/rate for a clip it did not identify.
        var result = new PlayerAnimationStatePayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(payload)),
            payload[8],
            payload[9],
            (PlayerAnimationSampleSource)payload[68],
            BinaryPrimitives.ReadUInt16LittleEndian(payload[10..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[12..]),
            (PlayerAnimationCapabilities)BinaryPrimitives.ReadUInt32LittleEndian(payload[16..]),
            (PlayerAnimationStateFlags)BinaryPrimitives.ReadUInt32LittleEndian(payload[20..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[24..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[28..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[32..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[36..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[40..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[44..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[48..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[52..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[56..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[60..]),
            BinaryPrimitives.ReadSingleLittleEndian(payload[64..]));
        ValidatePlayerAnimationState(result);
        return result;
    }

    // Both peers exchange this small schema/mode record before accepting live animation samples, preventing incompatible puppet engines from mixing.
    public static byte[] EncodeMotionReplicationConfig(
        MotionReplicationConfigPayload payload)
    {
        ValidateMotionReplicationConfig(payload);
        var bytes = new byte[MotionReplicationConfigSize];
        bytes[0] = payload.SchemaVersion;
        bytes[1] = (byte)payload.Mode;
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(2), (ushort)payload.Flags);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(4), payload.Revision);
        return bytes;
    }

    public static MotionReplicationConfigPayload DecodeMotionReplicationConfig(
        ReadOnlySpan<byte> payload)
    {
        RequireLength(
            payload,
            MotionReplicationConfigSize,
            nameof(MotionReplicationConfigPayload));
        var result = new MotionReplicationConfigPayload(
            payload[0],
            (MotionReplicationWireMode)payload[1],
            (MotionReplicationConfigFlags)BinaryPrimitives.ReadUInt16LittleEndian(payload[2..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[4..]));
        ValidateMotionReplicationConfig(result);
        return result;
    }

    private static void ValidatePlayerAnimationState(
        PlayerAnimationStatePayload payload)
    {
        // Canonical validation keeps a malformed optional overlay from corrupting the reliable player-transform lane that must continue to function.
        if (!payload.EntityId.IsValid ||
            payload.Slot > (byte)SessionRole.Guest ||
            payload.SchemaVersion != PlayerAnimationStateSchemaVersion ||
            !Enum.IsDefined(payload.Source) ||
            payload.LocomotionEpoch == 0 ||
            payload.SampleSequence == 0 ||
            (payload.Capabilities & ~KnownCapabilities) != 0 ||
            (payload.Flags & ~KnownStateFlags) != 0)
        {
            throw new ProtocolException(
                "Player animation state contains an invalid identity, schema, epoch, sample sequence, capability or flag.");
        }

        if ((payload.Source == PlayerAnimationSampleSource.None) !=
            (payload.Capabilities == PlayerAnimationCapabilities.None &&
             payload.Flags == PlayerAnimationStateFlags.None))
        {
            throw new ProtocolException(
                "Player animation sample source None is valid only for an empty capability probe.");
        }

        RequireCapability(
            payload,
            PlayerAnimationStateFlags.GraphHashValid,
            PlayerAnimationCapabilities.GraphIdentifier);
        RequireCapability(
            payload,
            PlayerAnimationStateFlags.StateHashValid,
            PlayerAnimationCapabilities.StateIdentifier);
        RequireCapability(
            payload,
            PlayerAnimationStateFlags.PrimaryClipHashValid |
                PlayerAnimationStateFlags.SecondaryClipHashValid,
            PlayerAnimationCapabilities.ClipIdentifiers);
        RequireCapability(
            payload,
            PlayerAnimationStateFlags.PrimaryPhaseValid |
                PlayerAnimationStateFlags.SecondaryPhaseValid,
            PlayerAnimationCapabilities.NormalizedPhase);
        RequireCapability(
            payload,
            PlayerAnimationStateFlags.PrimaryPlaybackRateValid |
                PlayerAnimationStateFlags.SecondaryPlaybackRateValid,
            PlayerAnimationCapabilities.PlaybackRate);
        RequireCapability(
            payload,
            PlayerAnimationStateFlags.PrimaryBlendWeightValid |
                PlayerAnimationStateFlags.SecondaryBlendWeightValid,
            PlayerAnimationCapabilities.BlendWeights);
        RequireCapability(
            payload,
            PlayerAnimationStateFlags.TransitionProgressValid |
                PlayerAnimationStateFlags.Transitioning,
            PlayerAnimationCapabilities.TransitionProgress);
        RequireCapability(
            payload,
            PlayerAnimationStateFlags.RootMotionActive |
                PlayerAnimationStateFlags.Looping,
            PlayerAnimationCapabilities.RuntimeFlags);

        RequireHash(payload.GraphHash, Has(payload, PlayerAnimationStateFlags.GraphHashValid));
        RequireHash(payload.StateHash, Has(payload, PlayerAnimationStateFlags.StateHashValid));
        RequireHash(
            payload.PrimaryClipHash,
            Has(payload, PlayerAnimationStateFlags.PrimaryClipHashValid));
        RequireHash(
            payload.SecondaryClipHash,
            Has(payload, PlayerAnimationStateFlags.SecondaryClipHashValid));

        RequireNormalizedOrZero(
            payload.PrimaryNormalizedPhase,
            Has(payload, PlayerAnimationStateFlags.PrimaryPhaseValid),
            "primary phase");
        RequireNormalizedOrZero(
            payload.SecondaryNormalizedPhase,
            Has(payload, PlayerAnimationStateFlags.SecondaryPhaseValid),
            "secondary phase");
        RequirePlaybackRateOrZero(
            payload.PrimaryPlaybackRate,
            Has(payload, PlayerAnimationStateFlags.PrimaryPlaybackRateValid),
            "primary playback rate");
        RequirePlaybackRateOrZero(
            payload.SecondaryPlaybackRate,
            Has(payload, PlayerAnimationStateFlags.SecondaryPlaybackRateValid),
            "secondary playback rate");
        RequireNormalizedOrZero(
            payload.PrimaryBlendWeight,
            Has(payload, PlayerAnimationStateFlags.PrimaryBlendWeightValid),
            "primary blend weight");
        RequireNormalizedOrZero(
            payload.SecondaryBlendWeight,
            Has(payload, PlayerAnimationStateFlags.SecondaryBlendWeightValid),
            "secondary blend weight");
        RequireNormalizedOrZero(
            payload.TransitionProgress,
            Has(payload, PlayerAnimationStateFlags.TransitionProgressValid),
            "transition progress");

        var primaryClipValid =
            Has(payload, PlayerAnimationStateFlags.PrimaryClipHashValid);
        var secondaryClipValid =
            Has(payload, PlayerAnimationStateFlags.SecondaryClipHashValid);
        if ((!primaryClipValid &&
             (Has(payload, PlayerAnimationStateFlags.PrimaryPhaseValid) ||
              Has(payload, PlayerAnimationStateFlags.PrimaryPlaybackRateValid) ||
              Has(payload, PlayerAnimationStateFlags.PrimaryBlendWeightValid))) ||
            (!secondaryClipValid &&
             (Has(payload, PlayerAnimationStateFlags.SecondaryPhaseValid) ||
              Has(payload, PlayerAnimationStateFlags.SecondaryPlaybackRateValid) ||
              Has(payload, PlayerAnimationStateFlags.SecondaryBlendWeightValid))) ||
            (secondaryClipValid && !primaryClipValid) ||
            (Has(payload, PlayerAnimationStateFlags.Transitioning) &&
             !Has(payload, PlayerAnimationStateFlags.TransitionProgressValid)))
        {
            throw new ProtocolException(
                "Player animation state contains inconsistent clip or transition validity.");
        }
    }

    private static void ValidateMotionReplicationConfig(
        MotionReplicationConfigPayload payload)
    {
        const MotionReplicationConfigFlags knownFlags =
            MotionReplicationConfigFlags.AllowTaskNavmeshFallback |
            MotionReplicationConfigFlags.EnableAnimSceneStoryVmProbe;
        if (payload.SchemaVersion != MotionReplicationConfigSchemaVersion ||
            !Enum.IsDefined(payload.Mode) ||
            (payload.Flags & ~knownFlags) != 0 ||
            payload.Revision == 0)
        {
            throw new ProtocolException(
                "Motion replication config contains an invalid schema, mode, flag or revision.");
        }
    }

    private static bool Has(
        PlayerAnimationStatePayload payload,
        PlayerAnimationStateFlags flag) =>
        (payload.Flags & flag) != 0;

    private static void RequireCapability(
        PlayerAnimationStatePayload payload,
        PlayerAnimationStateFlags validityFlags,
        PlayerAnimationCapabilities capability)
    {
        if ((payload.Flags & validityFlags) != 0 &&
            (payload.Capabilities & capability) == 0)
        {
            throw new ProtocolException(
                $"Player animation state uses {validityFlags} without capability {capability}.");
        }
    }

    private static void RequireHash(uint value, bool valid)
    {
        if ((valid && value == 0) || (!valid && value != 0))
        {
            throw new ProtocolException(
                "Player animation identifier must be non-zero only when its validity flag is set.");
        }
    }

    private static void RequireNormalizedOrZero(
        float value,
        bool valid,
        string field)
    {
        if (!float.IsFinite(value) ||
            (valid ? value is < 0f or > 1f : value != 0f))
        {
            throw new ProtocolException(
                $"Player animation {field} must be finite and canonical for its validity flag.");
        }
    }

    private static void RequirePlaybackRateOrZero(
        float value,
        bool valid,
        string field)
    {
        if (!float.IsFinite(value) ||
            (valid
                ? MathF.Abs(value) > MaximumAbsolutePlaybackRate
                : value != 0f))
        {
            throw new ProtocolException(
                $"Player animation {field} must be finite, within range and canonical for its validity flag.");
        }
    }

    private static void RequireLength(
        ReadOnlySpan<byte> payload,
        int expected,
        string name)
    {
        if (payload.Length != expected)
        {
            throw new ProtocolException(
                $"{name} payload must contain exactly {expected} bytes; received {payload.Length}.");
        }
    }
}
