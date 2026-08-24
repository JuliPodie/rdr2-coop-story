using System.Net;
using System.Security.Cryptography;
using System.Text;
using CoopStory.Protocol;

namespace CoopStory.Launcher;

public static class SessionPasswordService
{
    public const int MinimumLength = 4;
    public const int MaximumLength = 64;
    private const int DerivedLength = 48;
    private const int Iterations = 600_000;

    public static void Validate(string password)
    {
        ArgumentNullException.ThrowIfNull(password);
        var normalized = password.Normalize(NormalizationForm.FormKC);
        if (normalized.Length is < MinimumLength or > MaximumLength ||
            !string.Equals(password, password.Trim(), StringComparison.Ordinal) ||
            normalized.Any(char.IsControl) ||
            Encoding.UTF8.GetByteCount(normalized) > 128)
        {
            throw new LauncherException(
                "The session password must contain 4-64 characters, with no leading " +
                "or trailing spaces and no control characters.");
        }
    }

    public static string DeriveSessionToken(
        string password,
        string hostAddress)
    {
        Validate(password);
        var canonicalAddress = CanonicalIpv4(hostAddress);
        var normalized = password.Normalize(NormalizationForm.FormKC);
        var salt = Encoding.UTF8.GetBytes(
            "RDR2CoopStory/SessionPassword/v1\n" + canonicalAddress);
        var material = Rfc2898DeriveBytes.Pbkdf2(
            normalized,
            salt,
            Iterations,
            HashAlgorithmName.SHA256,
            DerivedLength);
        try
        {
            if (material.AsSpan(0, 16).IndexOfAnyExcept((byte)0) < 0)
            {
                material[0] = 1;
            }

            var sessionId = new Guid(material.AsSpan(0, 16));
            var secret = Convert.ToBase64String(material.AsSpan(16, 32))
                .TrimEnd('=')
                .Replace('+', '-')
                .Replace('/', '_');
            var token = $"{sessionId:N}.{secret}";
            _ = SessionCredentials.ParseToken(token);
            return token;
        }
        finally
        {
            CryptographicOperations.ZeroMemory(material);
            CryptographicOperations.ZeroMemory(salt);
        }
    }

    private static string CanonicalIpv4(string value)
    {
        if (!IPAddress.TryParse(value.Trim(), out var address) ||
            address.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork)
        {
            throw new LauncherException(
                "A session password can be bound only to a valid host IPv4 address.");
        }

        _ = InviteService.ValidateRemoteHost(address.ToString());
        return address.ToString();
    }
}
