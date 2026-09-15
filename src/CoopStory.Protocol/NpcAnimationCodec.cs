using System.Buffers.Binary;
using System.Numerics;

namespace CoopStory.Protocol;

[Flags]
public enum NpcAnimationFlags : ushort
{
    None = 0, Reloading = 1, Ragdoll = 2, GettingUp = 4, Melee = 8,
    Mounted = 16, Dead = 32, Jumping = 64, Climbing = 128
}
[Flags]
public enum NpcAnimationEvents : ushort { None = 0, Shot = 1, HitReaction = 2 }

public readonly record struct NpcAnimationPayload(
    NetEntityId EntityId, uint Revision, NpcAnimationFlags Flags,
    NpcAnimationEvents Events, ushort LeaseMs, uint WeaponHash, uint ModelHash,
    Vector3 Target, Vector3 Velocity, float HealthFraction);

public static class NpcAnimationCodec
{
    public const int Size = 56;
    // Every local queue spends the same lease instead of renewing it on delivery.
    public static NpcAnimationPayload? Age(NpcAnimationPayload payload, long elapsedMs)
    {
        Validate(payload);
        return elapsedMs < 0 || elapsedMs > payload.LeaseMs - 100
            ? null : payload with { LeaseMs = (ushort)(payload.LeaseMs - elapsedMs) };
    }
    public static byte[] Encode(NpcAnimationPayload p)
    {
        Validate(p);
        var b = new byte[Size];
        BinaryPrimitives.WriteUInt64LittleEndian(b, p.EntityId.Value);
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(8), p.Revision);
        b[12] = 1;
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(14), (ushort)p.Flags);
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(16), (ushort)p.Events);
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(18), p.LeaseMs);
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(20), p.WeaponHash);
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(24), p.ModelHash);
        WriteVector(b.AsSpan(28), p.Target);
        WriteVector(b.AsSpan(40), p.Velocity);
        BinaryPrimitives.WriteSingleLittleEndian(b.AsSpan(52), p.HealthFraction);
        return b;
    }
    public static NpcAnimationPayload Decode(ReadOnlySpan<byte> b)
    {
        if (b.Length != Size || b[12] != 1 || b[13] != 0)
            throw new ProtocolException("Invalid NPC animation schema, size, or reserved byte.");
        var p = new NpcAnimationPayload(
            new NetEntityId(BinaryPrimitives.ReadUInt64LittleEndian(b)),
            BinaryPrimitives.ReadUInt32LittleEndian(b[8..]),
            (NpcAnimationFlags)BinaryPrimitives.ReadUInt16LittleEndian(b[14..]),
            (NpcAnimationEvents)BinaryPrimitives.ReadUInt16LittleEndian(b[16..]),
            BinaryPrimitives.ReadUInt16LittleEndian(b[18..]),
            BinaryPrimitives.ReadUInt32LittleEndian(b[20..]),
            BinaryPrimitives.ReadUInt32LittleEndian(b[24..]),
            ReadVector(b[28..]), ReadVector(b[40..]),
            BinaryPrimitives.ReadSingleLittleEndian(b[52..]));
        Validate(p); return p;
    }
    private static void Validate(NpcAnimationPayload p)
    {
        if (!p.EntityId.IsValid || p.Revision == 0 || p.ModelHash == 0 ||
            ((ushort)p.Flags & ~255) != 0 || ((ushort)p.Events & ~3) != 0 ||
            p.LeaseMs is < 100 or > 2000 ||
            ((p.Events & NpcAnimationEvents.Shot) != 0 && p.WeaponHash == 0) ||
            ((p.Flags & NpcAnimationFlags.Reloading) != 0 && p.WeaponHash == 0) ||
            ((p.Flags & NpcAnimationFlags.Dead) != 0 &&
             (p.Flags != NpcAnimationFlags.Dead || p.Events != NpcAnimationEvents.None)) ||
            !Finite(p.Target) || !Finite(p.Velocity) ||
            !float.IsFinite(p.HealthFraction) || p.HealthFraction is < 0 or > 1)
            throw new ProtocolException("Invalid NPC animation payload.");
    }
    private static bool Finite(Vector3 v) => float.IsFinite(v.X) && float.IsFinite(v.Y) &&
        float.IsFinite(v.Z) && MathF.Abs(v.X) <= 100000 && MathF.Abs(v.Y) <= 100000 && MathF.Abs(v.Z) <= 100000;
    private static void WriteVector(Span<byte> b, Vector3 v)
    {
        BinaryPrimitives.WriteSingleLittleEndian(b, v.X);
        BinaryPrimitives.WriteSingleLittleEndian(b[4..], v.Y);
        BinaryPrimitives.WriteSingleLittleEndian(b[8..], v.Z);
    }
    private static Vector3 ReadVector(ReadOnlySpan<byte> b) => new(
        BinaryPrimitives.ReadSingleLittleEndian(b), BinaryPrimitives.ReadSingleLittleEndian(b[4..]),
        BinaryPrimitives.ReadSingleLittleEndian(b[8..]));
}
