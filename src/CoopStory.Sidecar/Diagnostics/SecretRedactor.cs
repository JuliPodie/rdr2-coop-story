using System.Text.RegularExpressions;

namespace CoopStory.Sidecar.Diagnostics;

// Removes invite codes/session credentials from any text written to diagnostics or included in a support archive, even when a caller forgot to pre-redact it.
public static class SecretRedactor
{
    public const string Replacement = "[REDACTED]";

    private static readonly Regex SessionTokenPattern = new(
        @"(?<![A-Fa-f0-9])[A-Fa-f0-9]{32}\.[A-Za-z0-9_-]{43}(?![A-Za-z0-9_-])",
        RegexOptions.Compiled | RegexOptions.CultureInvariant,
        TimeSpan.FromMilliseconds(250));
    private static readonly Regex InviteCodePattern = new(
        @"R2C1\.[A-Za-z0-9_-]{16,}",
        RegexOptions.Compiled | RegexOptions.CultureInvariant,
        TimeSpan.FromMilliseconds(250));

    public static string Redact(string value, params string?[] knownSecrets)
    {
        ArgumentNullException.ThrowIfNull(value);
        var redacted = InviteCodePattern.Replace(value, Replacement);
        redacted = SessionTokenPattern.Replace(redacted, Replacement);
        // Also remove exact runtime values because a token might occur in a format that is valid but not recognised by the general regexes.
        foreach (var secret in knownSecrets)
        {
            if (!string.IsNullOrEmpty(secret))
            {
                redacted = redacted.Replace(
                    secret,
                    Replacement,
                    StringComparison.Ordinal);
            }
        }

        return redacted;
    }
}
