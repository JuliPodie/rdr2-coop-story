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
                "Kod sesji jest pusty. Host powinien kliknąć „Nowy kod”, " +
                "a następnie bezpiecznie przekazać go znajomemu.");
        }

        try
        {
            _ = SessionCredentials.ParseToken(token.Trim());
        }
        catch (FormatException exception)
        {
            throw new LauncherException("Kod sesji ma nieprawidłowy format.", exception);
        }
    }
}
