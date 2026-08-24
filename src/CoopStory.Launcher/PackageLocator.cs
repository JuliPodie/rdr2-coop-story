namespace CoopStory.Launcher;

public static class PackageLocator
{
    public static PackageLayout Locate(string? startingDirectory = null)
    {
        var start = Path.GetFullPath(startingDirectory ?? AppContext.BaseDirectory);
        var candidates = new List<string>
        {
            start,
            Path.Combine(start, "dist")
        };

        var cursor = new DirectoryInfo(start);
        for (var depth = 0; cursor is not null && depth < 6; depth++, cursor = cursor.Parent)
        {
            candidates.Add(cursor.FullName);
            candidates.Add(Path.Combine(cursor.FullName, "dist"));
        }

        foreach (var candidate in candidates.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            if (TryCreate(candidate, out var layout))
            {
                return layout;
            }
        }

        throw new LauncherException(
            "Nie znaleziono kompletnej paczki moda. Oczekiwano " +
            "CoopStoryBridge.asi oraz sidecar\\CoopStory.Sidecar.exe obok launchera.");
    }

    public static bool TryCreate(string root, out PackageLayout layout)
    {
        layout = null!;
        var fullRoot = Path.GetFullPath(root);
        if (!Directory.Exists(fullRoot) || IsReparsePoint(fullRoot))
        {
            return false;
        }

        var bridge = Path.Combine(fullRoot, "CoopStoryBridge.asi");
        var config = Path.Combine(fullRoot, "config", "coopstory.example.json");
        var sidecarDirectory = Path.Combine(fullRoot, "sidecar");
        var sidecar = Path.Combine(sidecarDirectory, "CoopStory.Sidecar.exe");
        if (!File.Exists(bridge) ||
            !File.Exists(config) ||
            !Directory.Exists(sidecarDirectory) ||
            !File.Exists(sidecar))
        {
            return false;
        }

        layout = new PackageLayout(
            fullRoot,
            bridge,
            config,
            sidecarDirectory,
            sidecar);
        return true;
    }

    public static RuntimeLayout LocateRuntime(string selectedFolder)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(selectedFolder);
        var root = Path.GetFullPath(selectedFolder);
        if (!Directory.Exists(root) || IsReparsePoint(root))
        {
            throw new LauncherException(
                "Wybrany katalog ScriptHook nie istnieje albo jest dowiązaniem.");
        }

        foreach (var candidate in new[] { root, Path.Combine(root, "bin") })
        {
            if (!Directory.Exists(candidate) || IsReparsePoint(candidate))
            {
                continue;
            }

            var scriptHook = Path.Combine(candidate, "ScriptHookRDR2.dll");
            var dinput = Path.Combine(candidate, "dinput8.dll");
            if (!File.Exists(scriptHook) || !File.Exists(dinput))
            {
                continue;
            }

            if (IsReparsePoint(scriptHook) || IsReparsePoint(dinput))
            {
                throw new LauncherException(
                    "Pliki runtime ScriptHook nie mogą być dowiązaniami.");
            }

            return new RuntimeLayout(
                root,
                scriptHook,
                dinput,
                File.Exists(Path.Combine(candidate, "NativeTrainer.asi")) ||
                File.Exists(Path.Combine(root, "NativeTrainer.asi")));
        }

        throw new LauncherException(
            "W wybranym folderze ani jego podfolderze bin nie znaleziono " +
            "ScriptHookRDR2.dll i dinput8.dll.");
    }

    public static bool IsReparsePoint(string path) =>
        (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0;
}
