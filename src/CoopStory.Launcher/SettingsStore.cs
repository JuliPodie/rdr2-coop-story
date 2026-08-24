using System.Text.Json;

namespace CoopStory.Launcher;

public sealed class SettingsStore(LauncherPaths paths)
{
    public LauncherSettings Load()
    {
        if (!File.Exists(paths.SettingsPath))
        {
            return new LauncherSettings();
        }

        try
        {
            var settings = JsonSerializer.Deserialize<LauncherSettings>(
                File.ReadAllBytes(paths.SettingsPath),
                JsonSupport.Options);
            if (settings is null || settings.SchemaVersion != 1)
            {
                throw new LauncherException(
                    "Plik ustawień ma nieobsługiwaną wersję.");
            }

            return settings;
        }
        catch (JsonException exception)
        {
            throw new LauncherException(
                "Plik ustawień launchera jest uszkodzony.", exception);
        }
    }

    public void Save(LauncherSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        AtomicFile.WriteJson(paths.SettingsPath, settings, JsonSupport.Options);
    }
}
