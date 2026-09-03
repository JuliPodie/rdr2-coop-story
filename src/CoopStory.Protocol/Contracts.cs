using System.Numerics;

namespace CoopStory.Protocol;

// Shared data shapes for multiplayer messages. Think of a Payload as the
// contents of one network message, an enum as its allowed choices, and a
// Flags enum as several small on/off switches stored in one number.
// Other files turn these records into bytes, send them, validate them, and
// finally apply safe results in RDR2. This file does not do those jobs itself.

// Guest's first connection message. SessionId says which co-op session this is.
// InstanceId says which running guest program sent it. 
// Nonce and Proof are
// a one-time "I know the invite secret" check before gameplay messages begin.
public sealed record HelloPayload(
    Guid SessionId,
    Guid InstanceId,
    SessionRole Role,
    string Nonce,
    string Proof,
    ushort ProtocolVersion = ProtocolConstants.Version);

// Host's answer to Hello. "Accepted" says yes/no.
// "Reason" explains a rejection.
// When accepted, ServerNonce and Proof let the guest check if it reached the real
// host for this session rather than a random computer on the network.
public sealed record HelloAckPayload(
    Guid SessionId,
    Guid HostInstanceId,
    bool Accepted,
    string ServerNonce,
    string Proof,
    string? Reason = null,
    ushort ProtocolVersion = ProtocolConstants.Version);

// Small "I am still here" message. UnixTimeMilliseconds helps logs measure a connection gap,
// while LastReceivedTick shows whether messages flow both ways.
// It does not move a player, update an NPC, or change the game world.
public sealed record HeartbeatPayload(long UnixTimeMilliseconds, ulong LastReceivedTick);

// Structured network error. Code is a short error name for code/logs.
// Message is readable text. Fatal means the connection/session cannot safely continue.
// a non-fatal error may be shown/logged while the session stays alive.
public sealed record ErrorPayload(string Code, string Message, bool Fatal);

// Clean disconnect notice so the other peer can stop its local session and show/log why the connection ended instead of waiting for a timeout.
public sealed record GoodbyePayload(string Reason);

// The kinds of Story Mode unlocks the host may confirm for a guest. These are
// permissions only—for example, "this recipe is available", never a copy of a player's private inventory, money total, or whole save file.
public enum CampaignCapabilityKind : byte
{
    WeaponShopEligibility = 1,
    Recipe = 2,
    CapacityUpgrade = 3,
    ActivityGate = 4
}

// One host approved unlock for the guest.
// Kind says what sort of unlock it is;
// RecordHash names the exact unlock.
// HostEventId names this one grant event.
// If the network resends it, the guest can see it already saved/applied it.
public readonly record struct CampaignCapabilityPayload(
    CampaignCapabilityKind Kind,
    uint RecordHash,
    ulong HostEventId,
    long GrantedAtUnixMilliseconds);

// Guest reply confirming it received one specific capability event.
// It repeats Kind, RecordHash, and HostEventId so the host knows exactly which retry can stop.
// it is not a request for a new unlock.
public readonly record struct CampaignCapabilityAckPayload(CampaignCapabilityKind Kind, uint RecordHash, ulong HostEventId);

// Steps in the host controlled mission start and mission-completion handshake.
// The guest never starts a random host mission.
// Both saves must agree on the exact MissionId before the guest may use its own normal RDR2 mission prompt.
public enum MissionProgressionPhase : byte
{
    // Host asks whether the guest can enter this exact Story mission.
    Offer = 1,

    // Guest answers whether its own save can safely enter that mission.
    Eligibility = 2,

    // Host reports one completed mission transaction to the guest.
    Completion = 3,

    // Guest acknowledgement for a successfully persisted completion.
    // This lets the host retransmit a best-effort Completion frame until the save
    // transaction is known to be durable, without duplicating rewards.
    Applied = 4,

    // Host opens a short, exact-MissionData window in which the guest may use
    // their own vanilla prompt to enter the matching Story instance.
    StartBarrierOpen = 5,

    // The guest observed that exact MissionData entry become active locally.
    GuestInstanceStarted = 6,

    // Host acknowledged the matching guest instance.
    // This is an authority barrier, not a request to launch arbitrary game scripts.
    StartBarrierReleased = 7,

    // Guest could not enter the offered instance (or entered a different one).
    GuestInstanceRejected = 8,

    // Host closed the window after rejection or timeout. The guest returns to
    // normal companion-only mission isolation.
    StartBarrierAborted = 9
}

[Flags]
// Extra facts attached to a MissionProgressionPayload. More than one flag can
// be on at the same time: 1 << 0 is the first switch, 1 << 1 the next switch.
public enum MissionProgressionFlags : byte
{
    None = 0,

    // The guest checked its own save and can start the offered mission.
    GuestCanStart = 1 << 0,

    // This completion has a known safe guest side result. do not guess rewards.
    VerifiedCompletionMapping = 1 << 1
}

// One mission start or completion transaction.
// MissionId says which Story mission.
// MissionEpoch says which run of that mission.
// EventId says which one request/completion.
// Those IDs stop a delayed old packet changing a new run.
public readonly record struct MissionProgressionPayload(
    uint MissionId,
    uint MissionEpoch,
    ulong EventId,
    MissionProgressionPhase Phase,
    MissionProgressionFlags Flags,
    byte CompletionRating = 0,
    int CompletionCashAward = 0);

// Host's current Story objective text for the guest to display. Revision tells the guest whether this text is newer.
// Fingerprint helps ignore exact repeats.
// It is display only: the guest cannot send it back to change a mission script.
public readonly record struct MissionObjectivePayload(
    NetEntityId HostEntityId,
    uint MissionEpoch,
    uint Revision,
    ulong Fingerprint,
    string Text);

// Host cue telling the guest which approved mission dialogue line to play and when.
// ProfileId/RootId/LineIndex name one allowed catalogue line.
// HostStartTick gives its intended timing.
// No arbitrary Rockstar dialogue name can be sent.
// basically, the host is saying "play this line at this time" and the guest can accept or reject it.
// it reffers to the Dialogue line via the different id's and the line index, so the guest can look it up in its own local copy of the dialogue catalogue.w
public readonly record struct MissionDialogueCuePayload(
    NetEntityId HostEntityId,
    uint MissionEpoch,
    uint CheckpointGeneration,
    uint DialogueSequence,
    uint ProfileId,
    uint RootId,
    ushort LineIndex,
    ulong HostStartTick);

// Guest's result after trying to prepare a received dialogue cue.
// Ready means the guest can present it.
// the other values explain why it safely cannot.
// in short, the guest is saying "I can play this line" or "I cannot play this line because of X reason".
public enum MissionDialogueReadyState : byte
{
    Ready = 1,
    RootNotLoaded = 2,
    RootNotPlaying = 3,
    MissionMismatch = 4,
    StaleCue = 5,
    ProfileUnavailable = 6
}

// Guest's answer for one dialogue cue.
// It repeats the mission/checkpoint and cue IDs so the host cannot confuse an old reply with a newer dialogue line.
public readonly record struct MissionDialogueReadyPayload(
    NetEntityId HostEntityId,
    uint MissionEpoch,
    uint CheckpointGeneration,
    uint DialogueSequence,
    uint ProfileId,
    uint RootId,
    ushort LineIndex,
    MissionDialogueReadyState State);

// Types of co-op ambient encounters created by this project, such as an ambush or hostage rescue.
// They do not start Rockstar random-event scripts because those can differ between player saves and would make co-op unreliable.
// (might get fixed in the future, but for now, we are using our own custom encounters that are safe to run in co-op).
public enum AmbientEncounterProfile : byte
{
    RoadsideAmbush = 1,
    HostageRescue = 2,
    WagonDefense = 3,
    AnimalAttack = 4,
    CampClearout = 5
}

// Current stage of one ambient encounter:
// first suggested, then being prepared, then active, then ended as success/failure/abandoned.
public enum AmbientEncounterPhase : byte
{
    Proposed = 1,
    Preparing = 2,
    Active = 3,
    Succeeded = 4,
    Failed = 5,
    Abandoned = 6
}

// Reasons the host can refuse an ambient-encounter suggestion from the guest.
// For example, the players may be too far apart, in a mission, or not safe.
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

// The guest's role in an accepted encounter:
// active participant or a nearby companion who should only observe/follow it.
// This stops both games trying to give the guest conflicting jobs in the same encounter.
// unknown means the host has not yet decided whether the guest is a participant or companion.
public enum AmbientEncounterPeerDisposition : byte
{
    Unknown = 0,
    Participant = 1,
    Companion = 2
}

// Guest suggestion for an ambient encounter.
// Anchor and RadiusMeters say where it would happen.
// LocalEvidenceHash proves the guest saw an allowed trigger.
// The host validates all of it before deciding whether to create anything.
public readonly record struct AmbientEncounterProposalPayload(
    NetEntityId GuestEntityId,
    ulong ProposalId,
    AmbientEncounterProfile Profile,
    Vector3 Anchor,
    float RadiusMeters,
    uint LocalEvidenceHash,
    uint SuggestedRosterSeed);

// Host's current answer/state for one ambient encounter.
// InstanceId identifies this attempt.
// RosterSeed/RosterCount let both games make the same group of NPCs.
// Rejection explains why a proposed encounter did not start.
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

// Notice that an actor collected one approved native pickup.
// CollectionId makes this one collection event unique.
// It carries only proof of collection, never money, full inventory, or other private save data.
public readonly record struct PickupCollectedPayload(
    NetEntityId ActorEntityId,
    ulong CollectionId,
    uint PickupHash);

// Big player state changes.
// PlayerStatePayload carries fast movement snapshots.
// lifecycle is the separate state used for alive, downed, revive, and spectate rules, where dropping one update would be much more serious.
public enum PlayerLifecycle : byte
{
    Alive = 0,
    Downed = 1,
    Reviving = 2,
    Spectating = 3
}

// A special movement action with a clear beginning/end, currently jump or climb.
// None means ordinary walking/running.
// special actions get extra data.
public enum PlayerTraversalKind : byte
{
    None = 0,
    Jump = 1,
    Climb = 2
}

// The broad movement state used to choose how the remote player replica moves
// and animates:
// normal ground movement, aiming, climbing, falling, ragdoll, or mounted.
// It prevents one movement system fighting another.
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
// Yes/no facts sampled from a player's current game state.
// Several flags may be present in one PlayerStatePayload, such as Mounted + Aiming.
// They tell the other game which visual/interaction rules should be active right now.
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

    // Emitted only by the authenticated loopback peer.
    // It enables visual diagnostics in the bridge and is never set by a real player sampler.
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

// Frequently sent player movement snapshot.
// Position/Velocity/Heading say where the player is going.
// AimTarget and Flags describe actions.
// locomotion fields help the other game animate smoothly instead of guessing from positions.
//(could be causing desync if the package is not handled properly, but it is needed for smooth movement and animation).
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
// Evidence collected for one jump/climb action.
// It tells the receiver whether takeoff input, obstacle data, and expected landing are trustworthy.
// The bits are not commands—they are evidence the host/guest can validate.
public enum PlayerTraversalFlags : uint
{
    None = 0,
    InputEdgeDetected = 1 << 0,
    ObstacleValid = 1 << 1,
    ExpectedLandingValid = 1 << 2
}

// Reliable detail for one jump/climb action.
// TakeoffPosition/ApproachVelocity show how the action began.
// obstacle/landing values show where it should go.
// Fast PlayerState repeats ActionId.
// this message keeps the important details.
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

// Types of deliberate player actions that need host approval rather than being treated as ordinary movement snapshots.
// A guest may ask, but cannot declare its own lasso, knockdown, grapple, or crafting result as final.
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

// Stages of a player action.
// Begin/Active mean it is happening.
// Impact, Attached, or Bound mean an important result happened;
// End/Cancel/Reject say how it ended.
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
// Extra facts for a player-action transaction, including whether it is a player request (Intent), a host-approved result (Authoritative), or a reconnect copy (ResyncSnapshot).
// The other flags say which target/animation facts are valid.
// the << operator means each flag is a different bit in the uint, so multiple flags can be combined in one number.
// it basically moves the 1 bit left by the number of positions specified, creating a unique bit for each flag.
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

// Reliable action transaction.
// ActorEntityId/TargetEntityId say who is involved.
// ActionId/Revision stop late updates becoming new actions.
// Phase says progress.
// A guest may request it, then the host validates and broadcasts the result.
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

// Player-to-player or player-to-mount interactions that need host validation.
// Examples are revive, getting on/off a horse, and releasing a restraint. They
// use shared NetEntityIds, never local RDR2 handles that differ per PC.
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

// Stages of an interaction request. Begin starts it; Sustain means the player
// is still holding the button; Cancel stops it before completion. Revive is a
// typical action that uses Sustain.
public enum InteractionIntentPhase : byte
{
    None = 0,
    Begin = 1,
    Sustain = 2,
    Cancel = 3
}

[Flags]
// Describes what an interaction targets and whether it requires held input.
// Several switches can be on together, such as TargetPlayer + HoldRequired.
public enum InteractionIntentFlags : byte
{
    None = 0,
    TargetPlayer = 1 << 0,
    TargetMount = 1 << 1,
    HoldRequired = 1 << 2
}

// A player's request to interact. ActorEntityId is the player asking; target
// IDs name who/what they selected; InteractionId/Revision identify this try.
// The host checks distance and state before producing an InteractionResult.
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

// The host's current answer for an interaction request: accepted, still in
// progress, completed, rejected, or cancelled.
public enum InteractionResultStatus : byte
{
    None = 0,
    Accepted = 1,
    InProgress = 2,
    Completed = 3,
    Rejected = 4,
    Cancelled = 5
}

// Why the host rejected or cancelled an interaction, such as being too far
// away, targeting the wrong thing, using an old request, or a native failure.
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
// Extra information attached to an interaction result. Authoritative means the
// host made this decision; StateChanged says RDR2 should now look different.
public enum InteractionResultFlags : ushort
{
    None = 0,
    Authoritative = 1 << 0,
    StateChanged = 1 << 1,
    HoldRequired = 1 << 2
}

// Host-validated result for one interaction. Progress/RequiredDuration let both
// players show the same hold-to-complete bar. RejectReason explains failures.
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

// The replicated restraint state of a player: normal, lassoed, or hogtied.
public enum PlayerRestraintState : byte
{
    Free = 0,
    Lassoed = 1,
    Hogtied = 2
}

[Flags]
// Where a restraint update came from. EngineOwned means RDR2 owns the physical
// effect; Snapshot means this is the current state sent during resync/reconnect.
public enum RestraintStateFlags : byte
{
    None = 0,
    Authoritative = 1 << 0,
    EngineOwned = 1 << 1,
    Snapshot = 1 << 2
}

// Current lasso/hogtie state for one player. SubjectEntityId is restrained;
// OwnerEntityId owns the restraint; SourceInteractionId links it to the action
// that caused it, so a late packet cannot free/restrain the wrong player.
public readonly record struct RestraintStatePayload(
    NetEntityId SubjectEntityId,
    NetEntityId OwnerEntityId,
    uint SourceInteractionId,
    uint Revision,
    PlayerRestraintState State,
    RestraintStateFlags Flags);

// The stable multiplayer ID, host/guest slot, and display nickname for one
// connected player. This lets the other game label the right local replica.
public readonly record struct PlayerIdentityPayload(
    NetEntityId EntityId,
    byte Slot,
    string Nickname);

[Flags]
// Completeness and model type information for a replicated player appearance.
// A complete component set is needed before replacing all clothing/body pieces.
public enum PlayerAppearanceStateFlags : ushort
{
    None = 0,
    CompleteComponentSet = 1 << 0,
    StoryMetaPed = 1 << 1
}

// Player model/appearance snapshot. ModelHash chooses the ped model and
// ComponentHashes choose its pieces. Revision/Fingerprint ignore old or exactly
// repeated packets, avoiding needless model rebuilds and visual flicker.
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
// Facts about a player's horse or vehicle, including whether it exists, is
// mounted, is dead, or is being borrowed from the other player. Vehicle flags
// also say whether the player is driving or riding as a passenger.
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

// Snapshot for a player's mount or vehicle. PlayerEntityId names the rider;
// MountEntityId names the horse/vehicle; Generation stops an old mount packet
// from changing a newly spawned replacement mount replica.
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

// Facts about one host-owned NPC/object replica, such as type, combat state,
// firing, aiming, or mounting. Several flags can be on together. The host is
// the only side that decides these facts for shared NPCs/objects.
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

// The two kinds of world entities this system can mirror to the guest: a Ped
// (NPC/animal/horse) or a simple Object such as a prop/pickup-like object.
public enum WorldEntityKind : byte
{
    Ped = 1,
    Object = 2
}

// Which player a mirrored NPC is currently targeting in combat. This is a
// host decision so each game does not make the same NPC attack different people.
public enum WorldCombatTargetSlot : byte
{
    None = 0,
    Host = 1,
    Guest = 2
}

// The broad task the mirrored NPC/object is performing. This helps the guest
// choose an appropriate local task/animation instead of only teleporting it.
// It is a simple label, not a copied RDR2 task handle or AI brain.
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

// Host snapshot for one shared NPC or object. EntityId links the host entity to
// the guest's local proxy; ModelHash says what to spawn; ParentEntityId links
// riders/attachments; TaskTarget helps show what the entity is trying to do.
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

// Host instruction to remove the guest's local proxy for this shared entity.
// It contains only the shared ID because the guest looks up its own local handle.
public readonly record struct EntityDespawnPayload(NetEntityId EntityId);

// Report that a player tried to damage a shared entity. ShotSequence makes each
// shot attempt distinct. The host checks attacker, target, weapon, and range
// before applying damage, so a guest cannot simply claim an NPC is dead.
public readonly record struct DamageIntentPayload(
    NetEntityId AttackerId,
    NetEntityId TargetId,
    uint WeaponHash,
    float Damage,
    uint ShotSequence);

[Flags]
// Extra facts for a world time/weather snapshot. WeatherValid tells the guest
// whether WeatherFrom/WeatherTo contain usable weather data this update.
public enum WorldStateFlags : byte
{
    None = 0,
    WeatherValid = 1 << 0
}

// Host snapshot of shared time, date, and weather. WeatherFrom/WeatherTo name
// the old/new weather; Blend says how far through the transition RDR2 should be.
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

// Broad mission state on the host: idle, active mission, cutscene, loading, or
// recovery. The guest uses this to present a compatible view of Story Mode
// without trying to control the host's mission scripts.
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
// Extra facts about the current mission, including checkpoint recovery and game
// states that temporarily restrict normal player control, such as a transition,
// minigame, scripted vehicle, or RDR2 control lock.
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

// Host's current mission snapshot. MissionEpoch says which mission run; Revision
// says which update is newer; CheckpointGeneration says which checkpoint version.
// These stop reconnect traffic from taking the guest back to older mission state.
public readonly record struct MissionStatePayload(
    NetEntityId HostEntityId,
    uint MissionEpoch,
    uint Revision,
    uint CheckpointGeneration,
    MissionPhase Phase,
    MissionStateFlags Flags,
    Vector3 HostAnchor,
    float HostHeading);

// Stages of a host-led mission cinematic as seen by the guest: playing/loading,
// getting ready to resume normal play, completed, or safely aborted.
public enum MissionCinematicPhase : byte
{
    Playing = 1,
    Loading = 2,
    PrepareResume = 3,
    Completed = 4,
    Aborted = 5
}

[Flags]
// Extra facts for a cinematic, including whether a camera should be active and
// whether a skip/resume attempt is waiting, used a fallback, or timed out.
public enum MissionCinematicStateFlags : ushort
{
    None = 0,
    CameraExpected = 1 << 0,
    AnchorValid = 1 << 1,
    SkipPending = 1 << 2,
    ResumeTimedOut = 1 << 3
}

// Host cinematic snapshot. CinematicGeneration separates one cutscene from the
// next; ResumeAnchor/Heading tell the guest where to place its player when the
// shared cinematic presentation ends.
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

// Small one-off actions exchanged while a cinematic is loading, ready to resume,
// or being skipped. They are messages about presentation readiness, not commands
// to run arbitrary RDR2 cutscene scripts on the other PC.
public enum MissionCinematicActionKind : byte
{
    PresentationReady = 1,
    ResumeReady = 2,
    SkipRequest = 3
}

[Flags]
// Notes whether a cinematic action used a safe fallback route because the full
// preferred cinematic presentation could not be used on this computer.
public enum MissionCinematicActionFlags : ushort
{
    None = 0,
    FallbackUsed = 1 << 0
}

// One cinematic control action. ActionId names this exact action; MissionEpoch
// and CinematicGeneration ensure a late old-cutscene action cannot affect the
// current mission/cutscene.
public readonly record struct MissionCinematicActionPayload(
    NetEntityId HostEntityId,
    uint MissionEpoch,
    uint CinematicGeneration,
    uint ActionId,
    MissionCinematicActionKind Kind,
    byte SenderSlot,
    MissionCinematicActionFlags Flags);

[Flags]
// State of the guest's local replica of a host animation scene. Active/Running
// say what it is doing; Loaded says resources are ready; OriginValid says its
// copied position/rotation are safe to use.
public enum AnimSceneReplicaStateFlags : uint
{
    None = 0,
    Active = 1 << 0,
    Running = 1 << 1,
    Loaded = 1 << 2,
    CameraActive = 1 << 3,
    OriginValid = 1 << 4
}

// Frequent snapshot of an animation-scene replica: playback phase, rate,
// origin, loading state, and active camera count. It lets the guest keep a
// locally created scene close to the host without sharing engine-only handles.
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

// Kinds of game entity that can fill a named role in an animation scene, such
// as a ped role, horse role, prop/object role, vehicle, or pickup.
public enum AnimSceneRoleKind : byte
{
    Ped = 1,
    Horse = 2,
    Object = 3,
    Vehicle = 4,
    Pickup = 5
}

[Flags]
// Rules for an animation-scene role. Required means do not start without it;
// Player means the role must be filled by a player rather than an ordinary NPC.
public enum AnimSceneRoleFlags : ushort
{
    None = 0,
    Required = 1 << 0,
    Player = 1 << 1
}

// Binds one named animation-scene role to a shared entity and expected model.
// RoleName is the scene's slot name; EntityId/ModelHash tell the guest what may
// safely fill that slot in its own local scene.
public sealed record AnimSceneRoleBindingPayload(
    string RoleName,
    NetEntityId EntityId,
    uint ModelHash,
    AnimSceneRoleKind Kind,
    AnimSceneRoleFlags Flags,
    uint BindingFlags);

// Full approved animation-scene recipe sent by the host. ResourceName and
// PlaybackList say which known scene asset to use; Roles say who fills each
// part. Fingerprints let the guest check it received the exact same recipe.
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

// Two-part fingerprint used to identify one exact animation-scene definition.
// It is like a long label for the full recipe, useful for matching/rejecting it.
public readonly record struct AnimSceneDefinitionFingerprint(
    ulong Low,
    ulong High);

// Handshake actions for preparing, accepting, playing, or aborting an animation
// scene replica. GuestReady/GuestRejected are the guest's answer; HostPlayCommit
// /HostAbort are the host's final decision.
public enum AnimSceneControlKind : byte
{
    GuestReady = 1,
    GuestRejected = 2,
    HostPlayCommit = 3,
    HostAbort = 4
}

// Why an animation-scene preparation/control step was rejected or aborted—for
// example missing files/roles, a native error, a timeout, or an old generation.
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
// Facts about animation-scene preparation: resources loaded, all required roles
// bound, reconnect cache reused, late join, or a fallback presentation used.
public enum AnimSceneControlFlags : uint
{
    None = 0,
    ResourceLoaded = 1 << 0,
    RequiredRolesBound = 1 << 1,
    CacheHit = 1 << 2,
    LateJoin = 1 << 3,
    FallbackUsed = 1 << 4
}

// Reliable control message for one animation-scene definition. DefinitionRevision
// and fingerprints identify the recipe; PlayAtHostTick/StartPhase/Rate describe
// timing; Kind/Reason/Flags say whether it is ready, rejected, committed, or stopped.
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
// State of the host camera presentation during a mission transition: active,
// fading in/out, or which type of RDR2 camera supplied the sampled view.
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

// Host camera snapshot used only during mission transitions. Position/Rotation/
// FieldOfView tell the guest what to show. It sends camera movement, not the
// engine-owned camera or mission objects that cannot cross PCs.
public readonly record struct MissionCameraStatePayload(
    NetEntityId HostEntityId,
    uint MissionEpoch,
    uint CinematicGeneration,
    uint Revision,
    MissionCameraStateFlags Flags,
    Vector3 Position,
    Vector3 Rotation,
    float FieldOfView);

// Equipment facts sampled from a player. Equipped/Reloading say what the player
// is doing. The remote replica uses them for visuals without granting the other
// peer inventory power or letting it add/change items.
[Flags]
public enum EquipmentStateFlags : uint
{
    None = 0,
    Equipped = 1 << 0,
    Reloading = 1 << 1
}

// Current weapon and ammo snapshot for one shared player entity. WeaponHash
// names the equipped weapon; Ammo is the displayed/current amount for that weapon.
public readonly record struct EquipmentStatePayload(
    NetEntityId EntityId,
    uint WeaponHash,
    uint Ammo,
    EquipmentStateFlags Flags);

// Whether a pause message is a player's request or the host's final shared pause
// decision. Guests can vote/request; the host publishes the shared result.
public enum PauseVoteKind : byte
{
    RequestState = 1,
    AuthoritativeState = 2
}

[Flags]
// Which players voted and whether the host decided the shared session is paused.
// HostVoted + GuestVoted tell the UI who agreed; Paused is the final state.
public enum PauseVoteFlags : byte
{
    None = 0,
    HostVoted = 1 << 0,
    GuestVoted = 1 << 1,
    Paused = 1 << 2
}

// One player's pause request or the host's current vote result. VoterSlot names
// who asked. Generation is a round number that stops an old vote changing a
// newer pause decision after packets arrive late.
public readonly record struct PauseVotePayload(
    PauseVoteKind Kind,
    byte VoterSlot,
    PauseVoteFlags Flags,
    uint Generation);

// Named commands the Sidecar can ask the local Bridge to perform, such as spawn
// a replica, resync, teleport, or change spectator mode. The Bridge still checks
// role and state before any game-facing command is executed.
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
// Extra command behavior. Force asks the receiver to override normal limits;
// Acknowledge asks it to send back confirmation that it handled the command.
public enum CommandFlags : ushort
{
    None = 0,
    Force = 1 << 0,
    Acknowledge = 1 << 1
}

// One bridge command and its target/location data. Opcode says what to do;
// TargetEntityId/Position/Heading/Value give optional data for that command.
// Only the fields needed by its Opcode are used; the rest stay at defaults.
public readonly record struct CommandPayload(
    CommandOpcode Opcode,
    CommandFlags Flags,
    NetEntityId TargetEntityId,
    Vector3 Position,
    float Heading,
    float Value);

// Host-approved health/lifecycle update when a player is downed, revived, or
// returned to normal play. HealthFraction is a 0-to-1 part of full health.
public readonly record struct DownedStatePayload(
    NetEntityId EntityId,
    PlayerLifecycle Lifecycle,
    float HealthFraction);

// Request from one player to begin reviving another player. The host checks the
// two IDs, distance, and lifecycle before it starts a real revive interaction.
public readonly record struct ReviveRequestPayload(
    NetEntityId ReviverId,
    NetEntityId TargetId);

// Host-confirmed completion of a revive, including the target's restored health
// fraction. This is the final result; a ReviveRequest alone never revives anyone.
public readonly record struct ReviveCompletePayload(
    NetEntityId ReviverId,
    NetEntityId TargetId,
    float HealthFraction);
