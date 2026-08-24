using CoopStory.Protocol;

namespace CoopStory.Launcher;

public static class SessionCodeService
{
    public static string Generate() => SessionCredentials.Generate().ExportToken();

    public static void Validate(string token)
    {
        if (string.IsNullOrWhiteSpace(token))
        {
            throw new LauncherException(
                "The session code is empty. The host should click 'New code' " +
                "and share it with the invited player through a private channel.");
        }

        try
        {
            _ = SessionCredentials.ParseToken(token.Trim());
        }
        catch (FormatException exception)
        {
            throw new LauncherException("The session code has an invalid format.", exception);
        }
    }
}
