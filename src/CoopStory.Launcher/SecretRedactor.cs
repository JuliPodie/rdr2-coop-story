using System.Text.RegularExpressions;
using System.Text.Json;

namespace CoopStory.Launcher;

public sealed class SecretRedactor
{
    private static readonly Regex SessionTokenField = new(
        "(\"sessionToken\"\\s*:\\s*\")[^\"]*(\")",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant,
        TimeSpan.FromSeconds(1));

    private static readonly Regex PairingTokenLine = new(
        "(PAIRING_TOKEN\\s*=\\s*)[^\\s\"']+",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant,
        TimeSpan.FromSeconds(1));
    private static readonly Regex InviteCodePattern = new(
        @"R2C1\.[A-Za-z0-9_-]{16,}",
        RegexOptions.CultureInvariant,
        TimeSpan.FromSeconds(1));

    private readonly string[] _exactSecrets;

    public SecretRedactor(IEnumerable<string?> exactSecrets)
    {
        _exactSecrets = exactSecrets
            .Where(static value => !string.IsNullOrWhiteSpace(value))
            .Select(static value => value!)
            .SelectMany(ExpandRepresentations)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .OrderByDescending(static value => value.Length)
            .ToArray();
    }

    public string Redact(string value)
    {
        ArgumentNullException.ThrowIfNull(value);
        var redacted = value;
        foreach (var secret in _exactSecrets)
        {
            redacted = redacted.Replace(
                secret,
                "<REDACTED>",
                StringComparison.OrdinalIgnoreCase);
        }

        redacted = SessionTokenField.Replace(
            redacted,
            "$1<REDACTED>$2");
        redacted = InviteCodePattern.Replace(redacted, "<REDACTED>");
        return PairingTokenLine.Replace(redacted, "$1<REDACTED>");
    }

    private static IEnumerable<string> ExpandRepresentations(string secret)
    {
        foreach (var representation in ExpandPathSeparators(secret))
        {
            yield return representation;

            // A path embedded in JSON uses doubled Windows separators. Some
            // producers encode the separator as \u005C instead, so cover both
            // forms without relying on the input being valid JSON as a whole.
            var jsonEscaped =
                JsonEncodedText.Encode(representation).ToString();
            if (!jsonEscaped.Equals(
                    representation,
                    StringComparison.Ordinal))
            {
                yield return jsonEscaped;
                yield return jsonEscaped.Replace(
                    "\\\\",
                    "\\u005C",
                    StringComparison.Ordinal);
            }
        }
    }

    private static IEnumerable<string> ExpandPathSeparators(string value)
    {
        yield return value;
        if (value.Contains('\\'))
        {
            yield return value.Replace('\\', '/');
        }
    }
}
