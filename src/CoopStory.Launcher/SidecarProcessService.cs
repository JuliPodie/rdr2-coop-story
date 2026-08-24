using System.Diagnostics;
using System.Net;
using System.Net.NetworkInformation;
using System.Text;
using System.Text.Json;

namespace CoopStory.Launcher;

public enum GameLaunchTarget
{
    Steam,
    Rockstar
}

public sealed class SidecarProcessService : IDisposable
{
    private readonly LauncherPaths _paths;
    private readonly InstallationService _installation;
    private readonly LauncherLogger _logger;
    private readonly object _consoleLogGate = new();
    private readonly object _lobbyGate = new();
    private Process? _process;
    private CancellationTokenSource? _lobbyMonitorStop;
    private LauncherLobbySnapshot _lobby = LauncherLobbySnapshot.Empty;
    private bool _disposed;

    public SidecarProcessService(
        LauncherPaths paths,
        InstallationService installation,
        LauncherLogger logger)
    {
        _paths = paths;
        _installation = installation;
        _logger = logger;
    }

    public event EventHandler<bool>? RunningChanged;

    public event EventHandler<LauncherLobbySnapshot>? LobbyChanged;

    public bool IsRunning => _process is { HasExited: false };

    public LauncherLobbySnapshot Lobby
    {
        get
        {
            lock (_lobbyGate)
            {
                return _lobby;
            }
        }
    }

    public Task StartStoryModeAsync(
        LauncherSettings settings,
        PackageLayout package,
        GameLaunchTarget launchTarget,
        Action? launchGame = null,
        CancellationToken cancellationToken = default) =>
        StartProcessAsync(
            settings,
            package,
            launchTarget,
            localTest: false,
            launchGame,
            cancellationToken);

    public Task StartSoloTestAsync(
        LauncherSettings settings,
        PackageLayout package,
        GameLaunchTarget launchTarget,
        Action? launchGame = null,
        CancellationToken cancellationToken = default) =>
        StartProcessAsync(
            settings with { Role = LauncherRole.Host },
            package,
            launchTarget,
            localTest: true,
            launchGame,
            cancellationToken);

    private async Task StartProcessAsync(
        LauncherSettings settings,
        PackageLayout package,
        GameLaunchTarget launchTarget,
        bool localTest,
        Action? launchGame,
        CancellationToken cancellationToken)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (IsRunning)
        {
            throw new LauncherException("The sidecar is already running.");
        }

        if (_process is not null)
        {
            _process.Dispose();
            _process = null;
        }

        if (IsProcessRunning("RDR2"))
        {
            throw new LauncherException(
                "RDR2 is already running. Close the game so the launcher can start from the Story Mode menu.");
        }

        _ = _installation.ValidateInstalled(package);
        SidecarConfiguration.Save(settings, _paths);
        if (!File.Exists(package.SidecarExePath))
        {
            throw new LauncherException("CoopStory.Sidecar.exe is missing from the package.");
        }

        var start = new ProcessStartInfo
        {
            FileName = package.SidecarExePath,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            WorkingDirectory = package.SidecarDirectory
        };
        start.ArgumentList.Add(localTest ? "local-test" : "run");
        start.ArgumentList.Add("--config");
        start.ArgumentList.Add(_paths.SidecarConfigPath);
        if (localTest)
        {
            start.ArgumentList.Add("--motion-profile");
            start.ArgumentList.Add("puppet");
            start.ArgumentList.Add("--wait-for-f9");
            start.ArgumentList.Add("true");
            start.ArgumentList.Add("--ghost-recording");
            start.ArgumentList.Add(Path.Combine(
                _paths.StateDirectory,
                "recordings",
                "ghost-last.json"));
        }

        var process = new Process
        {
            StartInfo = start,
            EnableRaisingEvents = true
        };
        process.OutputDataReceived += (_, args) =>
            CaptureLine("stdout", args.Data);
        process.ErrorDataReceived += (_, args) =>
            CaptureLine("stderr", args.Data);
        process.Exited += (_, _) =>
        {
            var exitCode = TryGetExitCode(process);
            _logger.Info(
                "sidecar.exited",
                exitCode is null
                    ? "The sidecar exited."
                    : $"The sidecar exited with code {exitCode}.");
            ReportRunning(false);
            StopLobbyMonitor();
            UpdateLobby(current => current with
            {
                SidecarRunning = false,
                PeerConnected = false,
                GameBridgeConnected = false,
                PingMilliseconds = null
            });
        };

        if (!process.Start())
        {
            process.Dispose();
            throw new LauncherException("Failed to start the sidecar.");
        }

        _process = process;
        UpdateLobby(_ => new LauncherLobbySnapshot(
            settings.Nickname,
            settings.Role,
            localTest ? "SOLO BOT" : string.Empty,
            settings.Role == LauncherRole.Host
                ? LauncherRole.Guest
                : LauncherRole.Host,
            settings.Role == LauncherRole.Guest
                ? settings.HostAddress
                : string.Empty,
            true,
            false,
            false,
            null));
        StartLobbyMonitor();
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        ReportRunning(true);
        _logger.Info(
            "sidecar.started",
            $"Started sidecar pid={process.Id}, role={settings.Role}, " +
            $"mode={(localTest ? "solo-test-f9" : "multiplayer")}.");

        await Task.Delay(800, cancellationToken).ConfigureAwait(true);
        if (process.HasExited)
        {
            var exitCode = process.ExitCode;
            process.Dispose();
            _process = null;
            ReportRunning(false);
            throw new LauncherException(
                $"The sidecar exited before the game started (code {exitCode}). " +
                $"Check logs in {_paths.LogDirectory}.");
        }

        try
        {
            (launchGame ?? (() => OpenRdr2(launchTarget, settings.GameExePath)))();
            _logger.Info(
                "game.launch_requested",
                $"Sent the RDR2 start request through {launchTarget}. " +
                "The user must select Story Mode only.");
        }
        catch
        {
            Stop();
            throw;
        }
    }

    public void Stop()
    {
        if (_process is null)
        {
            return;
        }

        try
        {
            if (!_process.HasExited)
            {
                _process.Kill(entireProcessTree: true);
                _process.WaitForExit(5000);
            }
        }
        catch (InvalidOperationException)
        {
            // Process already exited.
        }
        finally
        {
            StopLobbyMonitor();
            _logger.Info("sidecar.stopped", "Sidecar stopped.");
            _process.Dispose();
            _process = null;
            ReportRunning(false);
            UpdateLobby(current => current with
            {
                SidecarRunning = false,
                PeerConnected = false,
                GameBridgeConnected = false,
                PingMilliseconds = null
            });
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        Stop();
        _disposed = true;
    }

    public static ProcessStartInfo CreateGameStartInfo(
        GameLaunchTarget launchTarget,
        string gameExePath)
    {
        if (launchTarget == GameLaunchTarget.Steam)
        {
            return new ProcessStartInfo
            {
                FileName = "steam://rungameid/1174180",
                UseShellExecute = true
            };
        }

        if (launchTarget != GameLaunchTarget.Rockstar)
        {
            throw new LauncherException("Unsupported game launch method.");
        }

        ArgumentException.ThrowIfNullOrWhiteSpace(gameExePath);
        var fullGameExePath = Path.GetFullPath(gameExePath);
        if (!File.Exists(fullGameExePath) ||
            !string.Equals(
                Path.GetFileName(fullGameExePath),
                "RDR2.exe",
                StringComparison.OrdinalIgnoreCase))
        {
            throw new LauncherException(
                "For Rockstar, select an existing RDR2.exe file.");
        }

        return new ProcessStartInfo
        {
            FileName = fullGameExePath,
            WorkingDirectory = Path.GetDirectoryName(fullGameExePath)
                ?? throw new LauncherException("RDR2.exe has no parent directory."),
            UseShellExecute = true
        };
    }

    private static void OpenRdr2(
        GameLaunchTarget launchTarget,
        string gameExePath)
    {
        var start = CreateGameStartInfo(launchTarget, gameExePath);
        _ = Process.Start(start)
            ?? throw new LauncherException(
                $"{launchTarget} did not accept the game start request.");
    }

    private static bool IsProcessRunning(string processName)
    {
        var processes = Process.GetProcessesByName(processName);
        try
        {
            return processes.Length > 0;
        }
        finally
        {
            foreach (var process in processes)
            {
                process.Dispose();
            }
        }
    }

    private static int? TryGetExitCode(Process process)
    {
        try
        {
            return process.HasExited ? process.ExitCode : null;
        }
        catch (InvalidOperationException)
        {
            return null;
        }
    }

    private void ReportRunning(bool running)
    {
        foreach (var subscriber in RunningChanged?
                     .GetInvocationList()
                     .Cast<EventHandler<bool>>() ?? [])
        {
            try
            {
                subscriber(this, running);
            }
            catch
            {
                // A UI subscriber cannot be allowed to break process cleanup.
            }
        }
    }

    private void CaptureLine(string stream, string? line)
    {
        if (string.IsNullOrWhiteSpace(line))
        {
            return;
        }

        try
        {
            if (line.StartsWith(
                    "COOP_LOBBY_STATUS=",
                    StringComparison.Ordinal))
            {
                UpdateLobby(current => ApplyLobbyStatusLine(current, line));
            }
            _logger.WriteSidecarLine(stream, line);
            Directory.CreateDirectory(_paths.LogDirectory);
            lock (_consoleLogGate)
            {
                File.AppendAllText(
                    _paths.SidecarConsoleLogPath,
                    $"{DateTimeOffset.UtcNow:o} [{stream}] {line}{Environment.NewLine}",
                    new UTF8Encoding(false));
            }
        }
        catch (Exception exception)
        {
            try
            {
                _logger.Error("sidecar.console_capture_failed", exception);
            }
            catch
            {
                // Do not throw from Process asynchronous data callbacks.
            }
        }
    }

    internal static LauncherLobbySnapshot ApplyLobbyStatusLine(
        LauncherLobbySnapshot current,
        string line)
    {
        const string prefix = "COOP_LOBBY_STATUS=";
        if (!line.StartsWith(prefix, StringComparison.Ordinal))
        {
            return current;
        }

        try
        {
            using var document = JsonDocument.Parse(line[prefix.Length..]);
            var root = document.RootElement;
            var eventName = root.GetProperty("event").GetString();
            return eventName switch
            {
                "peer" => current with
                {
                    PeerConnected = root.GetProperty("connected").GetBoolean(),
                    RemoteAddress = root.TryGetProperty("address", out var address)
                        ? address.GetString() ?? string.Empty
                        : current.RemoteAddress,
                    PingMilliseconds = null
                },
                "identity" => current with
                {
                    RemoteNickname = root.TryGetProperty("nickname", out var nickname)
                        ? nickname.GetString() ?? string.Empty
                        : current.RemoteNickname
                },
                "bridge" => current with
                {
                    GameBridgeConnected =
                        root.GetProperty("connected").GetBoolean()
                },
                _ => current
            };
        }
        catch (JsonException)
        {
            return current;
        }
        catch (InvalidOperationException)
        {
            return current;
        }
        catch (KeyNotFoundException)
        {
            return current;
        }
    }

    private void StartLobbyMonitor()
    {
        StopLobbyMonitor();
        _lobbyMonitorStop = new CancellationTokenSource();
        _ = MonitorLobbyPingAsync(_lobbyMonitorStop.Token);
    }

    private void StopLobbyMonitor()
    {
        var stop = Interlocked.Exchange(ref _lobbyMonitorStop, null);
        if (stop is null)
        {
            return;
        }

        try
        {
            stop.Cancel();
        }
        finally
        {
            stop.Dispose();
        }
    }

    private async Task MonitorLobbyPingAsync(CancellationToken cancellationToken)
    {
        using var ping = new Ping();
        while (!cancellationToken.IsCancellationRequested)
        {
            var snapshot = Lobby;
            long? roundTrip = null;
            if (snapshot.SidecarRunning && snapshot.PeerConnected &&
                IPAddress.TryParse(snapshot.RemoteAddress, out var address))
            {
                try
                {
                    var reply = await ping.SendPingAsync(
                            address,
                            900)
                        .ConfigureAwait(false);
                    if (reply.Status == IPStatus.Success)
                    {
                        roundTrip = reply.RoundtripTime;
                    }
                }
                catch (PingException)
                {
                }
                catch (OperationCanceledException) when (
                    cancellationToken.IsCancellationRequested)
                {
                    return;
                }
            }

            UpdateLobby(current => current with
            {
                PingMilliseconds = current.PeerConnected
                    ? roundTrip
                    : null
            });
            try
            {
                await Task.Delay(2_000, cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (
                cancellationToken.IsCancellationRequested)
            {
                return;
            }
        }
    }

    private void UpdateLobby(
        Func<LauncherLobbySnapshot, LauncherLobbySnapshot> update)
    {
        LauncherLobbySnapshot next;
        lock (_lobbyGate)
        {
            next = update(_lobby);
            if (next == _lobby)
            {
                return;
            }
            _lobby = next;
        }

        foreach (var subscriber in LobbyChanged?
                     .GetInvocationList()
                     .Cast<EventHandler<LauncherLobbySnapshot>>() ?? [])
        {
            try
            {
                subscriber(this, next);
            }
            catch
            {
                // Lobby presentation cannot affect the sidecar lifecycle.
            }
        }
    }
}
