using System.Globalization;
using System.Text;

namespace CoopStory.Protocol;

// Canonical nickname rules shared by both peers.
// Validation before encoding and after decoding prevents HUD/control characters or a different Unicode view from creating mismatched player identities.
public static class PlayerIdentityRules
{
    public const int MinimumNicknameCharacters = 1;
    public const int MaximumNicknameCharacters = 24;
    public const int MaximumNicknameUtf8Bytes = 64;

    private static readonly UTF8Encoding StrictUtf8 =
        new(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: true);

    public static string ValidateNickname(string nickname)
    {
        ArgumentNullException.ThrowIfNull(nickname);
        if (!string.Equals(
                nickname,
                nickname.Trim(),
                StringComparison.Ordinal))
        {
            throw new ArgumentException(
                "Nickname cannot start or end with whitespace.",
                nameof(nickname));
        }

        // Count Unicode runes rather than UTF-16 code units so emoji/supplementary characters have one predictable display-character cost.
        var characterCount = 0;
        foreach (var rune in nickname.EnumerateRunes())
        {
            characterCount++;
            var category = Rune.GetUnicodeCategory(rune);
            if (category is UnicodeCategory.Control or
                UnicodeCategory.Format or
                UnicodeCategory.LineSeparator or
                UnicodeCategory.ParagraphSeparator ||
                rune.Value == '~')
            {
                throw new ArgumentException(
                    "Nickname cannot contain control, formatting, or reserved HUD characters.",
                    nameof(nickname));
            }
        }

        if (characterCount is <
                MinimumNicknameCharacters or >
                MaximumNicknameCharacters)
        {
            throw new ArgumentException(
                $"Nickname must contain {MinimumNicknameCharacters} to " +
                $"{MaximumNicknameCharacters} Unicode characters.",
                nameof(nickname));
        }

        int byteCount;
        try
        {
            byteCount = StrictUtf8.GetByteCount(nickname);
        }
        catch (EncoderFallbackException exception)
        {
            throw new ArgumentException(
                "Nickname is not valid Unicode text.",
                nameof(nickname),
                exception);
        }

        if (byteCount > MaximumNicknameUtf8Bytes)
        {
            throw new ArgumentException(
                $"Nickname cannot exceed {MaximumNicknameUtf8Bytes} UTF-8 bytes.",
                nameof(nickname));
        }

        return nickname;
    }

    internal static byte[] EncodeUtf8(string nickname) =>
        StrictUtf8.GetBytes(ValidateNickname(nickname));

    // Treat invalid bytes and invalid-but-decodable nicknames as protocol errors because both arrived from a peer rather than a local text box.
    internal static string DecodeUtf8(ReadOnlySpan<byte> bytes)
    {
        try
        {
            return ValidateNickname(StrictUtf8.GetString(bytes));
        }
        catch (DecoderFallbackException exception)
        {
            throw new ProtocolException(
                "Player identity nickname is not valid UTF-8.",
                exception);
        }
        catch (ArgumentException exception)
        {
            throw new ProtocolException(
                "Player identity nickname is invalid.",
                exception);
        }
    }
}
