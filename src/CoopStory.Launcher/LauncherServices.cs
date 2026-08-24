namespace CoopStory.Launcher;

public sealed class LauncherServices : IDisposable
{
    private LauncherServices(
        LauncherPaths paths,
        LauncherLogger logger,
        SettingsStore settings,
        InstallationService installation,
        DiagnosticsService diagnostics,
        SidecarProcessService sidecar,
        PackageLayout package)
    {
        Paths = paths;
        Logger = logger;
        Settings = settings;
        Installation = installation;
        Diagnostics = diagnostics;
        Sidecar = sidecar;
        Package = package;
    }

    public LauncherPaths Paths { get; }

    public LauncherLogger Logger { get; }

    public SettingsStore Settings { get; }

    public InstallationService Installation { get; }

    public DiagnosticsService Diagnostics { get; }

    public SidecarProcessService Sidecar { get; }

    public PackageLayout Package { get; }

    public static LauncherServices Create(
        LauncherPaths paths,
        LauncherLogger logger)
    {
        paths.EnsureDirectories();
        var package = PackageLocator.Locate();
        var installation = new InstallationService(
            paths,
            LauncherPolicy.Production,
            logger);
        return new LauncherServices(
            paths,
            logger,
            new SettingsStore(paths),
            installation,
            new DiagnosticsService(paths, LauncherPolicy.Production, logger),
            new SidecarProcessService(paths, installation, logger),
            package);
    }

    public void Dispose() => Sidecar.Dispose();
}
