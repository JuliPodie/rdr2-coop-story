namespace CoopStory.Protocol;

public static class ProtocolConstants
{
    public const uint Magic = 0x50433252;
    public const ushort Version = 20;
    public const int HeaderSize = 24;
    public const int MaxPayloadSize = 1024 * 1024;
    public const int MaxUdpDatagramSize = 1200;
    public const int AuthenticationTagSize = 16;
}

public enum MessageType : ushort
{
    Hello = 1,
    HelloAck = 2,
    Heartbeat = 3,
    PlayerState = 4,
    EntitySpawn = 5,
    EntityUpdate = 6,
    EntityDespawn = 7,
    DamageIntent = 8,
    DamageApplied = 9,
    DownedState = 10,
    ReviveRequest = 11,
    ReviveComplete = 12,
    MissionState = 13,
    SpectatorState = 14,
    Command = 15,
    ResyncRequest = 16,
    ResyncSnapshot = 17,
    Error = 18,
    Goodbye = 19,
    SessionMenuRequest = 20,
    SessionMenuStatus = 21,
    PlayerIdentity = 22,
    WorldState = 23,
    EquipmentState = 24,
    PauseVote = 25,
    PlayerMountState = 26,
    PlayerTraversal = 27,
    PlayerAnimationState = 28,
    MotionReplicationConfig = 29,
    PlayerAction = 30,
    MissionCameraState = 31,
    InteractionIntent = 32,
    InteractionResult = 33,
    RestraintState = 34,
    MissionCinematicState = 35,
    MissionCinematicAction = 36,
    PlayerAppearanceState = 37,
    AnimSceneReplicaState = 38,
    AnimSceneDefinition = 39,
    AnimSceneControl = 40
}

public enum SessionRole
{
    Host,
    Guest
}
