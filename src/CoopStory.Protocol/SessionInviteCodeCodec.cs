using System.Buffers.Binary;
using System.Text;

namespace CoopStory.Protocol;

public sealed record SessionInviteCode(
    string HostAddress,
    ushort TcpPort,
    ushort UdpPort,
    string SessionToken);

public static class SessionInviteCodeCodec
{
    public const string Prefix = "R2C1.";
    private const byte FormatVersion = 1;
    private const int HeaderSize = 7;
    private const int MaximumHostBytes = 253;
    private const int MaximumTokenBytes = 256;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    public static string Encode(SessionInviteCode invite)
    {
        ArgumentNullException.ThrowIfNull(invite);
        var host = StrictUtf8.GetBytes(invite.HostAddress.Trim());
        var tokenText = invite.SessionToken.Trim();
        var token = StrictUtf8.GetBytes(tokenText);
        ValidateLengths(host.Length, token.Length);
        ValidatePorts(invite.TcpPort, invite.UdpPort);
        _ = SessionCredentials.ParseToken(tokenText);

        var bytes = new byte[HeaderSize + host.Length + token.Length];
        bytes[0] = FormatVersion;
        bytes[1] = checked((byte)host.Length);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(2), invite.TcpPort);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(4), invite.UdpPort);
        bytes[6] = checked((byte)token.Length);
        host.CopyTo(bytes.AsSpan(HeaderSize));
        token.CopyTo(bytes.AsSpan(HeaderSize + host.Length));
        return Prefix + Base64UrlEncode(bytes);
    }

    public static SessionInviteCode Decode(string code)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(code);
        var trimmed = code.Trim();
        if (!trimmed.StartsWith(Prefix, StringComparison.Ordinal))
        {
            throw new FormatException("Invite code has an invalid prefix.");
        }

        byte[] bytes;
        try
        {
            bytes = Base64UrlDecode(trimmed[Prefix.Length..]);
        }
        catch (FormatException exception)
        {
            throw new FormatException("Invite code contains invalid Base64URL data.", exception);
        }

        if (bytes.Length < HeaderSize || bytes[0] != FormatVersion)
        {
            throw new FormatException("Invite code has an unsupported format.");
        }

        var hostLength = bytes[1];
        var tcpPort = BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(2));
        var udpPort = BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(4));
        var tokenLength = bytes[6];
        ValidateLengths(hostLength, tokenLength);
        ValidatePorts(tcpPort, udpPort);
        if (bytes.Length != HeaderSize + hostLength + tokenLength)
        {
            throw new FormatException("Invite code has invalid field lengths.");
        }

        string host;
        string token;
        try
        {
            host = StrictUtf8.GetString(bytes, HeaderSize, hostLength);
            token = StrictUtf8.GetString(
                bytes,
                HeaderSize + hostLength,
                tokenLength);
        }
        catch (DecoderFallbackException exception)
        {
            throw new FormatException("Invite code contains invalid UTF-8.", exception);
        }

        if (string.IsNullOrWhiteSpace(host) ||
            !string.Equals(host, host.Trim(), StringComparison.Ordinal) ||
            string.IsNullOrWhiteSpace(token) ||
            !string.Equals(token, token.Trim(), StringComparison.Ordinal))
        {
            throw new FormatException("Invite code contains empty or padded fields.");
        }
        _ = SessionCredentials.ParseToken(token);
        return new SessionInviteCode(host, tcpPort, udpPort, token);
    }

    private static void ValidateLengths(int hostLength, int tokenLength)
    {
        if (hostLength is < 1 or > MaximumHostBytes)
        {
            throw new FormatException("Invite host address has an invalid length.");
        }
        if (tokenLength is < 1 or > MaximumTokenBytes)
        {
            throw new FormatException("Invite session token has an invalid length.");
        }
    }

    private static void ValidatePorts(ushort tcpPort, ushort udpPort)
    {
        if (tcpPort == 0 || udpPort == 0)
        {
            throw new FormatException("Invite ports must be non-zero.");
        }
    }

    private static string Base64UrlEncode(ReadOnlySpan<byte> bytes) =>
        Convert.ToBase64String(bytes)
            .TrimEnd('=')
            .Replace('+', '-')
            .Replace('/', '_');

    private static byte[] Base64UrlDecode(string text)
    {
        var standard = text.Replace('-', '+').Replace('_', '/');
        standard += (standard.Length % 4) switch
        {
            0 => string.Empty,
            2 => "==",
            3 => "=",
            _ => throw new FormatException("Invalid Base64URL length.")
        };
        return Convert.FromBase64String(standard);
    }
}
