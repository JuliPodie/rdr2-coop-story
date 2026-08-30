using System.Numerics;

namespace CoopStory.Protocol;

public sealed record HelloPayload(
    Guid SessionId,
    Guid InstanceId,
    SessionRole Role,
    string Nonce,
    string Proof,
    ushort ProtocolVersion = ProtocolConstants.Version);

public sealed record HelloAckPayload(
    Guid SessionId,
    Guid HostInstanceId,
    bool Accepted,
    string ServerNonce,
    string Proof,
    string? Reason = null,
    ushort ProtocolVersion = ProtocolConstants.Version);

public sealed record HeartbeatPayload(long UnixTimeMilliseconds, ulong LastReceivedTick);

public sealed record ErrorPayload(string Code, string Message, bool Fatal);

public sealed record GoodbyePayload(string Reason);

public enum CampaignCapabilityKind : byte
{
    WeaponShopEligibility = 1,
    Recipe = 2,
    CapacityUpgrade = 3,
    ActivityGate = 4
}

// Host-authoritative, idempotent entitlement. It contains no private player data.
public readonly record struct CampaignCapabilityPayload(
    CampaignCapabilityKind Kind,
    uint RecordHash,
    ulong HostEventId,
    long GrantedAtUnixMilliseconds);

public readonly record struct CampaignCapabilityAckPayload(
    CampaignCapabilityKind Kind,
    uint RecordHash,
    ulong HostEventId);

public enum MissionProgressionPhase : byte
{
    Offer = 1,
    Eligibility = 2,
    Completion = 3,
    // Guest acknowledgement for a successfully persisted completion. This
    // lets the host retransmit a best-effort Completion frame until the save
    // transaction is known to be durable, without duplicating rewards.
    Applied = 4,
    // Host opens a short, exact-MissionData window in which the guest may use
    // their own vanilla prompt to enter the matching Story instance.
    StartBarrierOpen = 5,
    // The guest observed that exact MissionData entry become active locally.
    GuestInstanceStarted = 6,
    // Host acknowledged the matching guest instance. This is an authority
    // barrier, not a request to launch arbitrary game scripts.
    StartBarrierReleased = 7,
    // Guest could not enter the offered instance (or entered a different one).
    GuestInstanceRejected = 8,
    // Host closed the window after rejection or timeout. The guest returns to
    // normal companion-only mission isolation.
    StartBarrierAborted = 9
}

[Flags]
public enum MissionProgressionFlags : byte
{
    None = 0,
    GuestCanStart = 1 << 0,
    VerifiedCompletionMapping = 1 << 1
}

public readonly record struct MissionProgressionPayload(
    uint MissionId,
    uint MissionEpoch,
    ulong EventId,
    MissionProgressionPhase Phase,
    MissionProgressionFlags Flags,
    byte CompletionRating = 0,
    int CompletionCashAward = 0);

// Host-owned current Story objective text. It is presentation only: the guest
// never sends it back and cannot use it to mutate a mission script.
public readonly record struct MissionObjectivePayload(
    NetEntityId HostEntityId,
    uint MissionEpoch,
    uint Revision,
    ulong Fingerprint,
    string Text);

// Only catalog-owned numeric IDs travel on the wire. A peer can never name an
// arbitrary Rockstar conversation or request a synthetic line.
public readonly record struct MissionDialogueCuePayload(
    NetEntityId HostEntityId,
    uint MissionEpoch,
    uint CheckpointGeneration,
    uint DialogueSequence,
    uint ProfileId,
    uint RootId,
    ushort LineIndex,
    ulong HostStartTick);

public enum MissionDialogueReadyState : byte
{
    Ready = 1,
    RootNotLoaded = 2,
    RootNotPlaying = 3,
    MissionMismatch = 4,
    StaleCue = 5,
    ProfileUnavailable = 6
}

public readonly record struct MissionDialogueReadyPayload(
    NetEntityId HostEntityId,
    uint MissionEpoch,
    uint CheckpointGeneration,
    uint DialogueSequence,
    uint ProfileId,
    uint RootId,
    ushort LineIndex,
    MissionDialogueReadyState State);

// Bridge-owned ambient activities. These deliberately do not name or start
// Rockstar random-event scripts, which are not deterministic across saves.
public enum AmbientEncounterProfile : byte
{
    RoadsideAmbush = 1,
    HostageRescue = 2,
    WagonDefense = 3,
    AnimalAttack = 4,
    CampClearout = 5
}

public enum AmbientEncounterPhase : byte
{
    Proposed = 1,
    Preparing = 2,
    Active = 3,
    Succeeded = 4,
    Failed = 5,
    Abandoned = 6
}

public enum AmbientEncounterRejection : byte
{
    None = 0,
    UnsupportedProfile = 1,
    HostUnavailable = 2,
    ParticipantUnsafe = 3,
    TooFarAway = 4,
    Busy = 5,
    InvalidAnchor = 6
}

public enum AmbientEncounterPeerDisposition : byte
{
    Unknown = 0,
    Participant = 1,
    Companion = 2
}

public readonly record struct AmbientEncounterProposalPayload(
    NetEntityId GuestEntityId,
    ulong ProposalId,
    AmbientEncounterProfile Profile,
    Vector3 Anchor,
    float RadiusMeters,
    uint LocalEvidenceHash,
    uint SuggestedRosterSeed);

public readonly record struct AmbientEncounterStatePayload(
    NetEntityId HostEntityId,
    ulong InstanceId,
    AmbientEncounterProfile Profile,
    AmbientEncounterPhase Phase,
    AmbientEncounterRejection Rejection,
    Vector3 Anchor,
    float RadiusMeters,
    uint RosterSeed,
    ushort RosterCount,
    ulong HostStartTick,
    uint ExactEventId,
    AmbientEncounterPeerDisposition GuestDisposition);

// Positive native collection evidence only. No money, items, or private
// inventory is allowed on this channel.
public readonly record struct PickupCollectedPayload(
    NetEntityId ActorEntityId,
    ulong CollectionId,
    uint PickupHash);

public enum PlayerLifecycle : byte
{
    Alive = 0,
    Downed = 1,
    Reviving = 2,
    Spectating = 3
}

public enum PlayerTraversalKind : byte
{
    None = 0,
    Jump = 1,
    Climb = 2
}

public enum PlayerLocomotionMode : byte
{
    Grounded = 0,
    Aiming = 1,
    Traversal = 2,
    Airborne = 3,
    Ragdoll = 4,
    Mounted = 5
}

[Flags]
public enum PlayerStateFlags : uint
{
    None = 0,
    InMission = 1 << 0,
    InCutscene = 1 << 1,
    Mounted = 1 << 2,
    Aiming = 1 << 3,
    Firing = 1 << 4,
    AimTargetValid = 1 << 5,
    MeleeCombat = 1 << 6,
    // Emitted only by the authenticated loopback peer. It enables visual
    // diagnostics in the bridge and is never set by a real player sampler.
    SyntheticTest = 1 << 7,
    Jumping = 1 << 8,
    Climbing = 1 << 9,
    StealthMovement = 1 << 10,
    PeerCombatTarget = 1 << 11,
    PeerLassoActive = 1 << 12,
    MeleeBlocking = 1 << 13,
    MeleeGrappling = 1 << 14,
    PeerKnockdown = 1 << 15,
    InCover = 1 << 16,
    GoingIntoCover = 1 << 17,
    CoverFacingLeft = 1 << 18,
    AimingFromCover = 1 << 19,
    InWater = 1 << 20,
    Swimming = 1 << 21,
    SwimmingUnderwater = 1 << 22,
    OnlineModeDetected = 1u << 31
}

public readonly record struct PlayerStatePayload(
    NetEntityId EntityId,
    byte Slot,
    PlayerLifecycle Lifecycle,
    Vector3 Position,
    Vector3 Velocity,
    float Heading,
    float HealthFraction,
    PlayerStateFlags Flags,
    Vector3 AimTarget = default,
    uint FireSequence = 0,
    float MovementHeading = 0,
    float LocalForwardSpeed = 0,
    float LocalRightSpeed = 0,
    float DesiredMoveBlend = 0,
    ushort LocomotionEpoch = 0,
    ushort TraversalActionId = 0,
    PlayerTraversalKind TraversalKind = PlayerTraversalKind.None,
    PlayerLocomotionMode LocomotionMode = PlayerLocomotionMode.Grounded,
    Vector3 TraversalAnchor = default,
    float TraversalHeading = 0);

[Flags]
public enum PlayerTraversalFlags : uint
{
    None = 0,
    InputEdgeDetected = 1 << 0,
    ObstacleValid = 1 << 1,
    ExpectedLandingValid = 1 << 2
}

/// <summary>
/// Reliable semantic transaction for a jump/vault/climb. PlayerState keeps
/// carrying the action id redundantly on UDP, while this payload preserves the
/// pre-takeoff context and later transaction revisions over TCP.
/// </summary>
public readonly record struct PlayerTraversalPayload(
    NetEntityId EntityId,
    byte Slot,
    PlayerTraversalKind Kind,
    ushort ActionId,
    ushort Revision,
    ushort LocomotionEpoch,
    PlayerTraversalFlags Flags,
    float TakeoffHeading,
    Vector3 TakeoffPosition,
    Vector3 ApproachVelocity,
    Vector3 ObstaclePoint,
    Vector3 ObstacleNormal,
    float ObstacleTopZ,
    Vector3 ExpectedLanding);

/// <summary>
/// A reliable, revisioned player-action transaction. Guests originate intent;
/// the host resolves it and broadcasts the authoritative state transition.
/// </summary>
public enum PlayerActionKind : byte
{
    None = 0,
    Aim = 1,
    MeleeAttack = 2,
    MeleeBlock = 3,
    Grapple = 4,
    Lasso = 5,
    Hogtie = 6,
    Knockdown = 7,
    Crafting = 8
}

public enum PlayerActionPhase : byte
{
    None = 0,
    Begin = 1,
    Active = 2,
    Impact = 3,
    Attached = 4,
    Sustain = 5,
    Bound = 6,
    Recover = 7,
    End = 8,
    Cancel = 9,
    Reject = 10,
    Snapshot = 11
}

[Flags]
public enum PlayerActionFlags : uint
{
    None = 0,
    Intent = 1 << 0,
    Authoritative = 1 << 1,
    TargetEntityValid = 1 << 2,
    TargetPointValid = 1 << 3,
    ActorAnchorValid = 1 << 4,
    Persistent = 1 << 5,
    PhysicalTargetEffect = 1 << 6,
    ResyncSnapshot = 1 << 7,
    VariantValid = 1 << 8,
    AnimationSampleValid = 1 << 9,
    NormalizedPhaseValid = 1 << 10
}

public readonly record struct PlayerActionPayload(
    NetEntityId ActorEntityId,
    NetEntityId TargetEntityId,
    uint Sequence,
    uint ActionId,
    ushort Revision,
    byte ActorSlot,
    byte AuthoritySlot,
    PlayerActionKind Kind,
    PlayerActionPhase Phase,
    PlayerActionFlags Flags,
    uint DurationMilliseconds,
    uint PhaseElapsedMilliseconds,
    uint WeaponHash,
    uint VariantHash,
    uint AnimationSampleSequence,
    Vector3 ActorAnchor,
    Vector3 TargetPoint,
    float FacingHeading,
    float NormalizedPhase);

/// <summary>
/// A semantic interaction is requested by either player, validated only by
/// the host and completed idempotently. It deliberately contains stable
/// network identities rather than process-local RDR2 handles.
/// </summary>
public enum InteractionKind : byte
{
    None = 0,
    ReleaseRestraint = 1,
    Revive = 2,
    DismountPeer = 3,
    MountDriver = 4,
    MountPassenger = 5,
    DismountSelf = 6,
    EmergencyRecover = 7
}

public enum InteractionIntentPhase : byte
{
    None = 0,
    Begin = 1,
    Sustain = 2,
    Cancel = 3
}

[Flags]
public enum InteractionIntentFlags : byte
{
    None = 0,
    TargetPlayer = 1 << 0,
    TargetMount = 1 << 1,
    HoldRequired = 1 << 2
}

public readonly record struct InteractionIntentPayload(
    NetEntityId ActorEntityId,
    NetEntityId TargetEntityId,
    NetEntityId SecondaryEntityId,
    uint InteractionId,
    ushort Revision,
    byte ActorSlot,
    InteractionKind Kind,
    InteractionIntentPhase Phase,
    InteractionIntentFlags Flags,
    uint RequestedDurationMilliseconds);

public enum InteractionResultStatus : byte
{
    None = 0,
    Accepted = 1,
    InProgress = 2,
    Completed = 3,
    Rejected = 4,
    Cancelled = 5
}

public enum InteractionRejectReason : byte
{
    None = 0,
    InvalidActor = 1,
    InvalidTarget = 2,
    TooFar = 3,
    InvalidState = 4,
    NotAuthority = 5,
    Busy = 6,
    Stale = 7,
    NativeFailed = 8,
    SessionReset = 9
}

[Flags]
public enum InteractionResultFlags : ushort
{
    None = 0,
    Authoritative = 1 << 0,
    StateChanged = 1 << 1,
    HoldRequired = 1 << 2
}

public readonly record struct InteractionResultPayload(
    NetEntityId ActorEntityId,
    NetEntityId TargetEntityId,
    NetEntityId SecondaryEntityId,
    uint InteractionId,
    ushort Revision,
    InteractionKind Kind,
    InteractionResultStatus Status,
    InteractionRejectReason RejectReason,
    InteractionResultFlags Flags,
    uint ProgressMilliseconds,
    uint RequiredDurationMilliseconds);

public enum PlayerRestraintState : byte
{
    Free = 0,
    Lassoed = 1,
    Hogtied = 2
}

[Flags]
public enum RestraintStateFlags : byte
{
    None = 0,
    Authoritative = 1 << 0,
    EngineOwned = 1 << 1,
    Snapshot = 1 << 2
}

public readonly record struct RestraintStatePayload(
    NetEntityId SubjectEntityId,
    NetEntityId OwnerEntityId,
    uint SourceInteractionId,
    uint Revision,
    PlayerRestraintState State,
    RestraintStateFlags Flags);

public readonly record struct PlayerIdentityPayload(
    NetEntityId EntityId,
    byte Slot,
    string Nickname);

[Flags]
public enum PlayerAppearanceStateFlags : ushort
{
    None = 0,
    CompleteComponentSet = 1 << 0,
    StoryMetaPed = 1 << 1
}

public sealed record PlayerAppearanceStatePayload(
    NetEntityId EntityId,
    byte Slot,
    byte SchemaVersion,
    PlayerAppearanceStateFlags Flags,
    uint Revision,
    uint ModelHash,
    ulong Fingerprint,
    uint[] ComponentHashes);

[Flags]
public enum PlayerMountStateFlags : byte
{
    None = 0,
    Present = 1 << 0,
    Mounted = 1 << 1,
    Dead = 1 << 2,
    BorrowedPeerMount = 1 << 3,
    Vehicle = 1 << 4,
    VehicleDriver = 1 << 5,
    VehiclePassenger = 1 << 6
}

public readonly record struct PlayerMountStatePayload(
    NetEntityId PlayerEntityId,
    NetEntityId MountEntityId,
    byte Slot,
    PlayerMountStateFlags Flags,
    uint ModelHash,
    Vector3 Position,
    Vector3 Velocity,
    float Heading,
    float HealthFraction,
    uint Generation);

[Flags]
public enum WorldEntityStateFlags : byte
{
    None = 0,
    Human = 1 << 0,
    Horse = 1 << 1,
    Dead = 1 << 2,
    InCombat = 1 << 3,
    Firing = 1 << 4,
    Aiming = 1 << 5,
    Mounted = 1 << 6,
    ScriptOwned = 1 << 7
}

public enum WorldEntityKind : byte
{
    Ped = 1,
    Object = 2
}

public enum WorldCombatTargetSlot : byte
{
    None = 0,
    Host = 1,
    Guest = 2
}

public enum WorldTaskKind : byte
{
    Idle = 0,
    Locomotion = 1,
    Scenario = 2,
    Fleeing = 3,
    Combat = 4,
    Mounted = 5,
    Dead = 6,
    Cinematic = 7
}

public readonly record struct WorldEntityStatePayload(
    NetEntityId EntityId,
    uint ModelHash,
    WorldEntityKind Kind,
    WorldEntityStateFlags Flags,
    WorldCombatTargetSlot CombatTargetSlot,
    Vector3 Position,
    Vector3 Velocity,
    float Heading,
    float HealthFraction,
    uint WeaponHash,
    WorldTaskKind TaskKind = WorldTaskKind.Idle,
    NetEntityId ParentEntityId = default,
    Vector3 TaskTarget = default);

public readonly record struct EntityDespawnPayload(NetEntityId EntityId);

public readonly record struct DamageIntentPayload(
    NetEntityId AttackerId,
    NetEntityId TargetId,
    uint WeaponHash,
    float Damage,
    uint ShotSequence);

[Flags]
public enum WorldStateFlags : byte
{
    None = 0,
    WeatherValid = 1 << 0
}

public readonly record struct WorldStatePayload(
    byte Hour,
    byte Minute,
    byte Second,
    WorldStateFlags Flags,
    uint WeatherFrom,
    uint WeatherTo,
    float Blend,
    byte Day,
    byte Month,
    ushort Year);

public enum MissionPhase : byte
{
    Idle = 0,
    Active = 1,
    Cutscene = 2,
    Loading = 3,
    Recovery = 4,
    SoloOverride = 5
}

[Flags]
public enum MissionStateFlags : byte
{
    None = 0,
    MissionActive = 1 << 0,
    AnchorValid = 1 << 1,
    CheckpointRecovery = 1 << 2,
    ScriptedControlLock = 1 << 3,
    ScreenTransition = 1 << 4,
    ScenarioActivity = 1 << 5,
    ScriptedVehicleTransition = 1 << 6,
    MinigameActivity = 1 << 7
}

public readonly record struct MissionStatePayload(
    NetEntityId HostEntityId,
    uint MissionEpoch,
    uint Revision,
    uint CheckpointGeneration,
    MissionPhase Phase,
    MissionStateFlags Flags,
    Vector3 HostAnchor,
    float HostHeading);

public enum MissionCinematicPhase : byte
{
    Playing = 1,
    Loading = 2,
    PrepareResume = 3,
    Completed = 4,
    Aborted = 5
}

[Flags]
public enum MissionCinematicStateFlags : ushort
{
    None = 0,
    CameraExpected = 1 << 0,
    AnchorValid = 1 << 1,
    SkipPending = 1 << 2,
    ResumeTimedOut = 1 << 3
}

public readonly record struct MissionCinematicStatePayload(
    NetEntityId HostEntityId,
    uint MissionEpoch,
    uint CinematicGeneration,
    uint Revision,
    uint CheckpointGeneration,
    MissionCinematicPhase Phase,
    MissionCinematicStateFlags Flags,
    Vector3 ResumeAnchor,
    float ResumeHeading);

public enum MissionCinematicActionKind : byte
{
    PresentationReady = 1,
    ResumeReady = 2,
    SkipRequest = 3
}

[Flags]
public enum MissionCinematicActionFlags : ushort
{
    None = 0,
    FallbackUsed = 1 << 0
}

public readonly record struct MissionCinematicActionPayload(
    NetEntityId HostEntityId,
    uint MissionEpoch,
    uint CinematicGeneration,
    uint ActionId,
    MissionCinematicActionKind Kind,
    byte SenderSlot,
    MissionCinematicActionFlags Flags);

[Flags]
public enum AnimSceneReplicaStateFlags : uint
{
    None = 0,
    Active = 1 << 0,
    Running = 1 << 1,
    Loaded = 1 << 2,
    CameraActive = 1 << 3,
    OriginValid = 1 << 4
}

public readonly record struct AnimSceneReplicaStatePayload(
    NetEntityId HostEntityId,
    uint MissionEpoch,
    uint CinematicGeneration,
    uint DefinitionRevision,
    uint Revision,
    uint DictionaryHash,
    AnimSceneReplicaStateFlags Flags,
    float Phase,
    float DurationSeconds,
    float Rate,
    Vector3 OriginPosition,
    Vector3 OriginRotation,
    ushort ActiveCameraCount);

public enum AnimSceneRoleKind : byte
{
    Ped = 1,
    Horse = 2,
    Object = 3,
    Vehicle = 4,
    Pickup = 5
}

[Flags]
public enum AnimSceneRoleFlags : ushort
{
    None = 0,
    Required = 1 << 0,
    Player = 1 << 1
}

public sealed record AnimSceneRoleBindingPayload(
    string RoleName,
    NetEntityId EntityId,
    uint ModelHash,
    AnimSceneRoleKind Kind,
    AnimSceneRoleFlags Flags,
    uint BindingFlags);

public sealed record AnimSceneDefinitionPayload(
    NetEntityId HostEntityId,
    uint MissionEpoch,
    uint CinematicGeneration,
    uint DefinitionRevision,
    uint DictionaryHash,
    ulong FingerprintLow,
    ulong FingerprintHigh,
    float DurationSeconds,
    uint SceneFlags,
    byte CreateOptionFlags,
    string ResourceName,
    string PlaybackList,
    AnimSceneRoleBindingPayload[] Roles);

public readonly record struct AnimSceneDefinitionFingerprint(
    ulong Low,
    ulong High);

public enum AnimSceneControlKind : byte
{
    GuestReady = 1,
    GuestRejected = 2,
    HostPlayCommit = 3,
    HostAbort = 4
}

public enum AnimSceneControlReason : byte
{
    None = 0,
    InvalidDefinition = 1,
    UnsupportedResource = 2,
    LoadTimeout = 3,
    MissingBinding = 4,
    EntityMismatch = 5,
    NativeFailure = 6,
    Superseded = 7,
    StaleGeneration = 8
}

[Flags]
public enum AnimSceneControlFlags : uint
{
    None = 0,
    ResourceLoaded = 1 << 0,
    RequiredRolesBound = 1 << 1,
    CacheHit = 1 << 2,
    LateJoin = 1 << 3,
    FallbackUsed = 1 << 4
}

public readonly record struct AnimSceneControlPayload(
    NetEntityId HostEntityId,
    uint MissionEpoch,
    uint CinematicGeneration,
    uint DefinitionRevision,
    uint ActionId,
    ulong FingerprintLow,
    ulong FingerprintHigh,
    ulong PlayAtHostTick,
    float StartPhase,
    float Rate,
    AnimSceneControlKind Kind,
    byte SenderSlot,
    AnimSceneControlReason Reason,
    AnimSceneControlFlags Flags);

[Flags]
public enum MissionCameraStateFlags : uint
{
    None = 0,
    Active = 1 << 0,
    ScreenFadedOut = 1 << 1,
    ScreenFadingOut = 1 << 2,
    ScreenFadingIn = 1 << 3,
    SourceRenderingScriptCamera = 1 << 4,
    SourceCinematicGameplayCamera = 1 << 5,
    SourceGameplayCameraFallback = 1 << 6
}

/// <summary>
/// Host-rendered camera snapshot used only while presenting a mission
/// transition to the guest. It deliberately carries no AnimScene or mission
/// script identity: those engine-owned objects are not portable between the
/// two Story Mode processes.
/// </summary>
public readonly record struct MissionCameraStatePayload(
    NetEntityId HostEntityId,
    uint MissionEpoch,
    uint CinematicGeneration,
    uint Revision,
    MissionCameraStateFlags Flags,
    Vector3 Position,
    Vector3 Rotation,
    float FieldOfView);

[Flags]
public enum EquipmentStateFlags : uint
{
    None = 0,
    Equipped = 1 << 0,
    Reloading = 1 << 1
}

public readonly record struct EquipmentStatePayload(
    NetEntityId EntityId,
    uint WeaponHash,
    uint Ammo,
    EquipmentStateFlags Flags);

public enum PauseVoteKind : byte
{
    RequestState = 1,
    AuthoritativeState = 2
}

[Flags]
public enum PauseVoteFlags : byte
{
    None = 0,
    HostVoted = 1 << 0,
    GuestVoted = 1 << 1,
    Paused = 1 << 2
}

public readonly record struct PauseVotePayload(
    PauseVoteKind Kind,
    byte VoterSlot,
    PauseVoteFlags Flags,
    uint Generation);

public enum CommandOpcode : ushort
{
    SpawnReplica = 1,
    ApplyTransform = 2,
    DespawnReplica = 3,
    SpectatorOn = 4,
    SpectatorOff = 5,
    TeleportGuest = 6,
    EnterDowned = 7,
    CompleteRevive = 8,
    RetryCheckpoint = 9,
    SoloOverrideOn = 10,
    SoloOverrideOff = 11,
    Resync = 12,
    Unload = 13,
    ToggleDiagnostics = 14,
    ResyncEquipment = 15,
    DiagnosticMarker = 16
}

[Flags]
public enum CommandFlags : ushort
{
    None = 0,
    Force = 1 << 0,
    Acknowledge = 1 << 1
}

public readonly record struct CommandPayload(
    CommandOpcode Opcode,
    CommandFlags Flags,
    NetEntityId TargetEntityId,
    Vector3 Position,
    float Heading,
    float Value);

public readonly record struct DownedStatePayload(
    NetEntityId EntityId,
    PlayerLifecycle Lifecycle,
    float HealthFraction);

public readonly record struct ReviveRequestPayload(
    NetEntityId ReviverId,
    NetEntityId TargetId);

public readonly record struct ReviveCompletePayload(
    NetEntityId ReviverId,
    NetEntityId TargetId,
    float HealthFraction);
