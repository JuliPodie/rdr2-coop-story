using System.Buffers.Binary;
using System.Text;

namespace CoopStory.Protocol;

public enum SessionMenuAction : byte
{
    Host = 1,
    JoinFromClipboard = 2,
    ToggleSoloTest = 3,
    ToggleGhostRecord = 4,
    ToggleGhostReplay = 5,
    StopSession = 6,
    ToggleGuestWorldView = 7
}

public enum SessionMenuStatusKind : byte
{
    Waiting = 0,
    StartingHost = 1,
    StartingGuest = 2,
    ReadyHost = 3,
    ReadyGuest = 4,
    Error = 5
}

public readonly record struct SessionMenuRequestPayload(
    SessionMenuAction Action,
    string InviteCode);

public readonly record struct SessionMenuStatusPayload(
    SessionMenuStatusKind Kind,
    string Message,
    string InviteCode = "");

public static class SessionMenuPayloadCodec
{
    public const int MaximumInviteCodeBytes = 768;
    public const int MaximumMessageBytes = 384;
    private const int StatusHeaderSize = 5;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    public static byte[] EncodeRequest(SessionMenuRequestPayload payload)
    {
        if (!Enum.IsDefined(payload.Action))
        {
            throw new ProtocolException("Session menu request has an unknown action.");
        }

        var invite = EncodeBounded(
            payload.InviteCode.Trim(),
            MaximumInviteCodeBytes,
            "invite code");
        var bytes = new byte[1 + invite.Length];
        bytes[0] = (byte)payload.Action;
        invite.CopyTo(bytes.AsSpan(1));
        return bytes;
    }

    public static SessionMenuRequestPayload DecodeRequest(ReadOnlySpan<byte> payload)
    {
        if (payload.IsEmpty ||
            !Enum.IsDefined((SessionMenuAction)payload[0]))
        {
            throw new ProtocolException(
                "Session menu request is empty or contains an unknown action.");
        }

        if (payload.Length - 1 > MaximumInviteCodeBytes)
        {
            throw new ProtocolException("Session invite code is too large.");
        }

        return new SessionMenuRequestPayload(
            (SessionMenuAction)payload[0],
            DecodeUtf8(payload[1..], "invite code").Trim());
    }

    public static byte[] EncodeStatus(SessionMenuStatusPayload payload)
    {
        if (!Enum.IsDefined(payload.Kind))
        {
            throw new ProtocolException("Session menu status has an unknown kind.");
        }

        var message = EncodeBounded(
            payload.Message,
            MaximumMessageBytes,
            "status message");
        var invite = EncodeBounded(
            payload.InviteCode.Trim(),
            MaximumInviteCodeBytes,
            "invite code");
        var bytes = new byte[StatusHeaderSize + message.Length + invite.Length];
        bytes[0] = (byte)payload.Kind;
        BinaryPrimitives.WriteUInt16LittleEndian(
            bytes.AsSpan(1),
            checked((ushort)message.Length));
        BinaryPrimitives.WriteUInt16LittleEndian(
            bytes.AsSpan(3),
            checked((ushort)invite.Length));
        message.CopyTo(bytes.AsSpan(StatusHeaderSize));
        invite.CopyTo(bytes.AsSpan(StatusHeaderSize + message.Length));
        return bytes;
    }

    public static SessionMenuStatusPayload DecodeStatus(ReadOnlySpan<byte> payload)
    {
        if (payload.Length < StatusHeaderSize ||
            !Enum.IsDefined((SessionMenuStatusKind)payload[0]))
        {
            throw new ProtocolException(
                "Session menu status is truncated or contains an unknown kind.");
        }

        var messageLength = BinaryPrimitives.ReadUInt16LittleEndian(payload[1..]);
        var inviteLength = BinaryPrimitives.ReadUInt16LittleEndian(payload[3..]);
        if (messageLength > MaximumMessageBytes ||
            inviteLength > MaximumInviteCodeBytes ||
            payload.Length != StatusHeaderSize + messageLength + inviteLength)
        {
            throw new ProtocolException("Session menu status has invalid lengths.");
        }

        var message = DecodeUtf8(
            payload.Slice(StatusHeaderSize, messageLength),
            "status message");
        var invite = DecodeUtf8(
            payload.Slice(StatusHeaderSize + messageLength, inviteLength),
            "invite code");
        return new SessionMenuStatusPayload(
            (SessionMenuStatusKind)payload[0],
            message,
            invite);
    }

    private static byte[] EncodeBounded(string value, int maximum, string name)
    {
        var bytes = StrictUtf8.GetBytes(value);
        if (bytes.Length > maximum)
        {
            throw new ProtocolException($"Session {name} is too large.");
        }
        return bytes;
    }

    private static string DecodeUtf8(ReadOnlySpan<byte> value, string name)
    {
        try
        {
            return StrictUtf8.GetString(value);
        }
        catch (DecoderFallbackException exception)
        {
            throw new ProtocolException(
                $"Session {name} is not valid UTF-8.",
                exception);
        }
    }
}
