namespace CoopStory.Protocol;

/// <summary>
/// Describes which internal animation-graph values the sender can read for the current executable.
/// Capabilities do not imply that a value is present in every sample; <see cref="PlayerAnimationStateFlags"/> carries per-sample validity.
/// </summary>
[Flags]
public enum PlayerAnimationCapabilities : uint
{
    None = 0,
    GraphIdentifier = 1 << 0,
    StateIdentifier = 1 << 1,
    ClipIdentifiers = 1 << 2,
    NormalizedPhase = 1 << 3,
    PlaybackRate = 1 << 4,
    BlendWeights = 1 << 5,
    TransitionProgress = 1 << 6,
    RuntimeFlags = 1 << 7
}

/// <summary>
/// Per-sample validity and graph state.
/// A sender must leave unavailable identifiers and numeric values at their canonical zero representation.
/// </summary>
[Flags]
public enum PlayerAnimationStateFlags : uint
{
    None = 0,
    GraphHashValid = 1 << 0,
    StateHashValid = 1 << 1,
    PrimaryClipHashValid = 1 << 2,
    SecondaryClipHashValid = 1 << 3,
    PrimaryPhaseValid = 1 << 4,
    SecondaryPhaseValid = 1 << 5,
    PrimaryPlaybackRateValid = 1 << 6,
    SecondaryPlaybackRateValid = 1 << 7,
    PrimaryBlendWeightValid = 1 << 8,
    SecondaryBlendWeightValid = 1 << 9,
    TransitionProgressValid = 1 << 10,
    Transitioning = 1 << 16,
    RootMotionActive = 1 << 17,
    Looping = 1 << 18
}

/// <summary>
/// Identifies where the sample was obtained.
/// In particular, graph/state/clip hashes from a versioned memory reader are not confused with higher-level locomotion values exposed by public natives.
/// </summary>
public enum PlayerAnimationSampleSource : byte
{
    None = 0,
    LocomotionNative = 1,
    MoveNetworkNative = 2,
    VersionedMemoryReader = 3
}

/// <summary>
/// Optional exact animation-graph sample.
/// Hash fields deliberately contain no assumed RDR2 identifiers: an implementation may set them only after a version-specific reader has positively resolved the corresponding value.
/// </summary>
public readonly record struct PlayerAnimationStatePayload(
    NetEntityId EntityId,
    byte Slot,
    byte SchemaVersion,
    PlayerAnimationSampleSource Source,
    ushort LocomotionEpoch,
    uint SampleSequence,
    PlayerAnimationCapabilities Capabilities,
    PlayerAnimationStateFlags Flags,
    uint GraphHash,
    uint StateHash,
    uint PrimaryClipHash,
    uint SecondaryClipHash,
    float PrimaryNormalizedPhase,
    float SecondaryNormalizedPhase,
    float PrimaryPlaybackRate,
    float SecondaryPlaybackRate,
    float PrimaryBlendWeight,
    float SecondaryBlendWeight,
    float TransitionProgress);

// The "Wire" suffix intentionally avoids colliding with the sidecar and launcher configuration enums, which live at a different abstraction layer.
// The negotiated wire mode determines how the native bridge presents remote movement; it is distinct from protocol frame transport/reliability choice.
public enum MotionReplicationWireMode : byte
{
    TaskNavmesh = 0,
    AnimGraphReplica = 1
}

[Flags]
// Optional feature switches agreed by both peers before a motion configuration is delivered to their native bridges.
public enum MotionReplicationConfigFlags : ushort
{
    None = 0,
    AllowTaskNavmeshFallback = 1 << 0,
    EnableAnimSceneStoryVmProbe = 1 << 1
}

/// <summary>
/// Small sidecar-to-bridge control payload.
/// Revision gives the bridge a deterministic last-write-wins key when the pipe reconnects.
/// </summary>
public readonly record struct MotionReplicationConfigPayload(
    byte SchemaVersion,
    MotionReplicationWireMode Mode,
    MotionReplicationConfigFlags Flags,
    uint Revision);
