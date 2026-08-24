namespace CoopStory.Launcher;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        ApplicationConfiguration.Initialize();
        using var singleInstance = LauncherSingleInstance.TryAcquire();
        if (singleInstance is null)
        {
            MessageBox.Show(
                "RDR2 Coop Story Launcher jest już uruchomiony dla tego użytkownika.\n\n" +
                "Wróć do istniejącego okna launchera.",
                "Launcher już działa",
                MessageBoxButtons.OK,
                MessageBoxIcon.Information);
            return;
        }

        var paths = LauncherPaths.CreateDefault();
        using var logger = new LauncherLogger(paths.LauncherLogPath);
        Application.ThreadException += (_, args) =>
        {
            logger.Error("ui.thread_exception", args.Exception);
            MessageBox.Show(
                $"Nieoczekiwany błąd: {args.Exception.Message}\n\n" +
                $"Szczegóły zapisano w:\n{paths.LogDirectory}",
                "RDR2 Coop Story",
                MessageBoxButtons.OK,
                MessageBoxIcon.Error);
        };

        try
        {
            var services = LauncherServices.Create(paths, logger);
            Application.Run(new MainForm(services));
        }
        catch (Exception exception)
        {
            logger.Error("launcher.fatal", exception);
            MessageBox.Show(
                $"Launcher nie może wystartować: {exception.Message}\n\n" +
                $"Szczegóły zapisano w:\n{paths.LogDirectory}",
                "RDR2 Coop Story",
                MessageBoxButtons.OK,
                MessageBoxIcon.Error);
        }
    }
}
