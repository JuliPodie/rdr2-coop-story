using System.Security.Cryptography;
using System.Text;

namespace CoopStory.Protocol;

public sealed class SessionCredentials
{
    private const int SecretLength = 32;
    private readonly byte[] _secret;

    private SessionCredentials(Guid sessionId, byte[] secret)
    {
        SessionId = sessionId;
        _secret = secret;
    }

    public Guid SessionId { get; }

    public static SessionCredentials Generate()
    {
        var secret = RandomNumberGenerator.GetBytes(SecretLength);
        return new SessionCredentials(Guid.NewGuid(), secret);
    }

    public static SessionCredentials ParseToken(string token)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(token);
        var separator = token.IndexOf('.');
        if (separator != 32 || separator == token.Length - 1)
        {
            throw new FormatException("Session token has an invalid format.");
        }

        if (!Guid.TryParseExact(token.AsSpan(0, separator), "N", out var sessionId))
        {
            throw new FormatException("Session token contains an invalid session identifier.");
        }

        byte[] secret;
        try
        {
            secret = Base64UrlDecode(token[(separator + 1)..]);
        }
        catch (FormatException exception)
        {
            throw new FormatException("Session token contains invalid Base64URL data.", exception);
        }

        if (secret.Length != SecretLength)
        {
            CryptographicOperations.ZeroMemory(secret);
            throw new FormatException($"Session secret must contain {SecretLength} bytes.");
        }

        return new SessionCredentials(sessionId, secret);
    }

    public string ExportToken() =>
        $"{SessionId:N}.{Base64UrlEncode(_secret)}";

    public string CreateClientProof(
        Guid instanceId,
        SessionRole role,
        string nonce) =>
        ComputeProof($"client\n{SessionId:N}\n{instanceId:N}\n{role}\n{nonce}");

    public bool VerifyClientProof(
        Guid instanceId,
        SessionRole role,
        string nonce,
        string suppliedProof) =>
        VerifyProof(
            $"client\n{SessionId:N}\n{instanceId:N}\n{role}\n{nonce}",
            suppliedProof);

    public string CreateServerProof(
        Guid hostInstanceId,
        Guid guestInstanceId,
        string clientNonce,
        string serverNonce) =>
        ComputeProof(
            $"server\n{SessionId:N}\n{hostInstanceId:N}\n{guestInstanceId:N}\n{clientNonce}\n{serverNonce}");

    public bool VerifyServerProof(
        Guid hostInstanceId,
        Guid guestInstanceId,
        string clientNonce,
        string serverNonce,
        string suppliedProof) =>
        VerifyProof(
            $"server\n{SessionId:N}\n{hostInstanceId:N}\n{guestInstanceId:N}\n{clientNonce}\n{serverNonce}",
            suppliedProof);

    public static string CreateNonce() =>
        Base64UrlEncode(RandomNumberGenerator.GetBytes(16));

    internal byte[] ComputeAuthenticationTag(ReadOnlySpan<byte> data)
    {
        var fullTag = HMACSHA256.HashData(_secret, data);
        var result = fullTag[..ProtocolConstants.AuthenticationTagSize];
        CryptographicOperations.ZeroMemory(fullTag);
        return result;
    }

    private string ComputeProof(string input)
    {
        var bytes = Encoding.UTF8.GetBytes(input);
        var tag = HMACSHA256.HashData(_secret, bytes);
        return Base64UrlEncode(tag);
    }

    private bool VerifyProof(string input, string suppliedProof)
    {
        byte[] supplied;
        try
        {
            supplied = Base64UrlDecode(suppliedProof);
        }
        catch (FormatException)
        {
            return false;
        }

        var expected = HMACSHA256.HashData(_secret, Encoding.UTF8.GetBytes(input));
        var valid = supplied.Length == expected.Length &&
            CryptographicOperations.FixedTimeEquals(supplied, expected);
        CryptographicOperations.ZeroMemory(expected);
        CryptographicOperations.ZeroMemory(supplied);
        return valid;
    }

    private static string Base64UrlEncode(ReadOnlySpan<byte> value) =>
        Convert.ToBase64String(value)
            .TrimEnd('=')
            .Replace('+', '-')
            .Replace('/', '_');

    private static byte[] Base64UrlDecode(string value)
    {
        var padded = value.Replace('-', '+').Replace('_', '/');
        padded += (padded.Length % 4) switch
        {
            0 => string.Empty,
            2 => "==",
            3 => "=",
            _ => throw new FormatException("Invalid Base64URL length.")
        };
        return Convert.FromBase64String(padded);
    }
}

public static class AuthenticatedDatagramCodec
{
    public const int SenderInstanceIdSize = 16;

    public static byte[] Encode(
        ProtocolEnvelope envelope,
        SessionCredentials credentials) =>
        EncodeCore(envelope, credentials, Guid.Empty);

    public static byte[] Encode(
        ProtocolEnvelope envelope,
        SessionCredentials credentials,
        Guid senderInstanceId)
    {
        if (senderInstanceId == Guid.Empty)
        {
            throw new ArgumentException(
                "Authenticated UDP sender instance identifier cannot be empty.",
                nameof(senderInstanceId));
        }

        return EncodeCore(envelope, credentials, senderInstanceId);
    }

    public static ProtocolEnvelope Decode(
        ReadOnlySpan<byte> datagram,
        SessionCredentials credentials) =>
        DecodeCore(datagram, credentials, Guid.Empty);

    public static ProtocolEnvelope Decode(
        ReadOnlySpan<byte> datagram,
        SessionCredentials credentials,
        Guid expectedSenderInstanceId)
    {
        if (expectedSenderInstanceId == Guid.Empty)
        {
            throw new ArgumentException(
                "Expected UDP sender instance identifier cannot be empty.",
                nameof(expectedSenderInstanceId));
        }

        return DecodeCore(datagram, credentials, expectedSenderInstanceId);
    }

    private static byte[] EncodeCore(
        ProtocolEnvelope envelope,
        SessionCredentials credentials,
        Guid senderInstanceId)
    {
        ArgumentNullException.ThrowIfNull(credentials);
        var frame = ProtocolCodec.Encode(envelope);
        var totalLength = checked(
            ProtocolConstants.AuthenticationTagSize +
            SenderInstanceIdSize +
            frame.Length);
        if (totalLength > ProtocolConstants.MaxUdpDatagramSize)
        {
            throw new ProtocolException(
                $"Authenticated UDP datagram is {totalLength} bytes; maximum is " +
                $"{ProtocolConstants.MaxUdpDatagramSize}.");
        }

        var result = new byte[totalLength];
        var authenticatedBody = result.AsSpan(ProtocolConstants.AuthenticationTagSize);
        if (!senderInstanceId.TryWriteBytes(
                authenticatedBody[..SenderInstanceIdSize],
                bigEndian: true,
                out var bytesWritten) ||
            bytesWritten != SenderInstanceIdSize)
        {
            throw new ProtocolException("Could not encode UDP sender instance identifier.");
        }

        frame.CopyTo(authenticatedBody[SenderInstanceIdSize..]);
        var tag = credentials.ComputeAuthenticationTag(authenticatedBody);
        try
        {
            tag.CopyTo(result, 0);
        }
        finally
        {
            CryptographicOperations.ZeroMemory(tag);
        }

        return result;
    }

    private static ProtocolEnvelope DecodeCore(
        ReadOnlySpan<byte> datagram,
        SessionCredentials credentials,
        Guid expectedSenderInstanceId)
    {
        ArgumentNullException.ThrowIfNull(credentials);
        if (datagram.Length > ProtocolConstants.MaxUdpDatagramSize ||
            datagram.Length <
                ProtocolConstants.AuthenticationTagSize +
                SenderInstanceIdSize +
                ProtocolConstants.HeaderSize)
        {
            throw new ProtocolException("Authenticated UDP datagram has an invalid length.");
        }

        var suppliedTag = datagram[..ProtocolConstants.AuthenticationTagSize];
        var authenticatedBody = datagram[ProtocolConstants.AuthenticationTagSize..];
        var expectedTag = credentials.ComputeAuthenticationTag(authenticatedBody);
        try
        {
            if (!CryptographicOperations.FixedTimeEquals(suppliedTag, expectedTag))
            {
                throw new ProtocolException("UDP datagram authentication failed.");
            }
        }
        finally
        {
            CryptographicOperations.ZeroMemory(expectedTag);
        }

        Span<byte> expectedInstanceBytes = stackalloc byte[SenderInstanceIdSize];
        if (!expectedSenderInstanceId.TryWriteBytes(
                expectedInstanceBytes,
                bigEndian: true,
                out var bytesWritten) ||
            bytesWritten != SenderInstanceIdSize ||
            !CryptographicOperations.FixedTimeEquals(
                authenticatedBody[..SenderInstanceIdSize],
                expectedInstanceBytes))
        {
            throw new ProtocolException("UDP datagram sender instance does not match the TCP peer.");
        }

        var frame = authenticatedBody[SenderInstanceIdSize..];
        return ProtocolCodec.Decode(frame);
    }
}
