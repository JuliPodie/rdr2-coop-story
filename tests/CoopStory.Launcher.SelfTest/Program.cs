using System.IO.Compression;
using System.Net;
using System.Text;
using System.Text.Json;
using CoopStory.Launcher;
using CoopStory.Protocol;
using CoopStory.Sidecar.Configuration;

var tests = new (string Name, Action Run)[]
{
    ("launcher is single-instance per user", TestSingleInstance),
    ("Hamachi is preferred over LAN detection", TestHamachiDetectionPriority),
    ("session code and invite round-trip", TestInviteRoundTrip),
    ("session password derives a private protocol credential", TestSessionPassword),
    ("invite rejects non-remote addresses", TestInviteRejectsLocalAddresses),
    ("owned runtime install and uninstall", TestOwnedRuntime),
    ("pre-existing runtime is preserved", TestPreexistingRuntime),
    ("foreign ASI blocks installation", TestForeignAsi),
    ("modified owned bridge blocks uninstall", TestModifiedBridge),
    ("prepared partial staging can be recovered", TestPreparedRecovery),
    ("rollback preserves a foreign committed manifest", TestForeignManifestRace),
    ("wrong game hash is rejected", TestWrongGameHash),
    ("guest sidecar config is correct", TestGuestConfig),
    ("launcher lobby follows sidecar status", TestLobbyStatus),
    ("host config passes normal sidecar validation", TestHostConfigValidation),
    ("motion replication mode defaults safely and round-trips",
        TestMotionReplicationModeConfig),
    ("nickname config and validation are safe", TestNicknameConfig),
    ("in-game menu can bootstrap without a pre-created invite", TestMenuBootstrapConfig),
    ("runtime is forbidden inside package", TestBundledRuntimeRejected),
    ("changed package is rejected before start", TestChangedPackageRejected),
    ("installed package can be updated in one operation", TestPackageUpdate),
    ("Steam and Rockstar launch requests are correct", TestGameLaunchTargets),
    ("diagnostics redact token and private paths", TestDiagnosticsRedaction),
    ("diagnostics build correlated timeline and anomalies",
        TestDiagnosticsTimelineAndAnomalies),
    ("quiet diagnostics do not invent divergence or transport gaps",
        TestDiagnosticsQuietSessionNoFalsePositives),
    ("diagnostics replace the previous archive", TestDiagnosticsReplacement),
    ("diagnostics use the last in-game role", TestDiagnosticsObservedRole),
    ("redactor covers escaped Windows JSON paths", TestEscapedWindowsPathRedaction)
};

var passed = 0;
foreach (var (name, run) in tests)
{
    try
    {
        run();
        Console.WriteLine($"PASS {name}");
        passed++;
    }
    catch (Exception exception)
    {
        Console.Error.WriteLine($"FAIL {name}: {exception}");
    }
}

Console.WriteLine($"RESULT {passed}/{tests.Length}");
return passed == tests.Length ? 0 : 1;

static void TestSingleInstance()
{
    var key = $"launcher-selftest-{Guid.NewGuid():N}";
    using var first = LauncherSingleInstance.TryAcquire(key)
        ?? throw new InvalidOperationException(
            "First launcher instance did not acquire its mutex.");
    using var second = LauncherSingleInstance.TryAcquire(key);
    Assert(second is null, "Second launcher instance acquired the same mutex.");

    first.Dispose();
    using var third = LauncherSingleInstance.TryAcquire(key);
    Assert(third is not null, "Mutex was not released after the first instance closed.");
}

static void TestHamachiDetectionPriority()
{
    var selected = InviteService.SelectSuggestedLanAddress(
    [
        IPAddress.Parse("192.168.0.10"),
        IPAddress.Parse("25.0.0.25"),
        IPAddress.Parse("10.0.0.10")
    ]);
    Assert(
        selected == "25.0.0.25",
        "Hamachi 25/8 did not take priority over private LAN addresses.");
}

static void TestGameLaunchTargets()
{
    using var context = TestContext.Create();
    var steam = SidecarProcessService.CreateGameStartInfo(
        GameLaunchTarget.Steam,
        context.GameExe);
    Assert(
        steam.FileName == "steam://rungameid/1174180",
        "Steam launch URI is incorrect.");
    Assert(steam.UseShellExecute, "Steam launch must use the Windows shell.");

    var rockstar = SidecarProcessService.CreateGameStartInfo(
        GameLaunchTarget.Rockstar,
        context.GameExe);
    Assert(
        rockstar.FileName == Path.GetFullPath(context.GameExe),
        "Rockstar launch does not use the selected RDR2.exe.");
    Assert(
        rockstar.WorkingDirectory == Path.GetFullPath(context.GameRoot),
        "Rockstar launch has an incorrect working directory.");
    Assert(rockstar.UseShellExecute, "Rockstar launch must use the Windows shell.");

    ExpectThrows<LauncherException>(
        () => SidecarProcessService.CreateGameStartInfo(
            (GameLaunchTarget)999,
            context.GameExe));
}

static void TestInviteRoundTrip()
{
    using var context = TestContext.Create();
    var token = SessionCodeService.Generate();
    var host = context.Settings with
    {
        HostAddress = "192.168.50.10",
        SessionToken = token
    };
    var invitePath = Path.Combine(context.Root, "test.coopjoin");
    InviteService.Export(invitePath, host);
    var imported = InviteService.Import(invitePath, new LauncherSettings());
    Assert(imported.Role == LauncherRole.Guest, "Invite did not set Guest role.");
    Assert(imported.HostAddress == host.HostAddress, "Invite host mismatch.");
    Assert(imported.SessionToken == token, "Invite token mismatch.");
}

static void TestSessionPassword()
{
    const string password = "Public-Test!29";
    const string hostAddress = "192.168.0.10";
    var first = SessionPasswordService.DeriveSessionToken(
        password,
        hostAddress);
    var second = SessionPasswordService.DeriveSessionToken(
        password,
        hostAddress);
    var different = SessionPasswordService.DeriveSessionToken(
        "Public-Test!30",
        hostAddress);

    Assert(first == second, "The same password and IPv4 derived different credentials.");
    Assert(first != different, "Different passwords derived the same credential.");
    _ = SessionCredentials.ParseToken(first);
    SessionPasswordService.Validate("1234");
    ExpectThrows<LauncherException>(() => SessionPasswordService.Validate("123"));
    ExpectThrows<LauncherException>(() =>
        SessionPasswordService.Validate(" leading-space"));

    using var context = TestContext.Create();
    var store = new SettingsStore(context.Paths);
    store.Save(context.Settings with { SessionToken = first });
    var settingsJson = File.ReadAllText(context.Paths.SettingsPath);
    Assert(
        !settingsJson.Contains(password, StringComparison.Ordinal),
        "The clear-text session password was persisted in launcher settings.");
    Assert(
        settingsJson.Contains(first, StringComparison.Ordinal),
        "The derived protocol credential was not persisted.");

    var configBytes = SidecarConfiguration.CreateBytes(
        context.Settings with
        {
            HostAddress = hostAddress,
            SessionToken = first
        },
        context.Paths);
    var configJson = Encoding.UTF8.GetString(configBytes);
    Assert(
        !configJson.Contains(password, StringComparison.Ordinal),
        "The clear-text session password leaked into sidecar configuration.");
}

static void TestInviteRejectsLocalAddresses()
{
    Assert(
        InviteService.ValidateRemoteHost("25.0.0.25") ==
        "25.0.0.25",
        "Hamachi IPv4 address was rejected.");
    foreach (var address in new[]
             {
                 "127.0.0.1",
                 "localhost",
                 "0.0.0.0",
                 "255.255.255.255",
                 "224.0.0.1"
             })
    {
        ExpectThrows<LauncherException>(
            () => InviteService.ValidateRemoteHost(address));
    }
}

static void TestOwnedRuntime()
{
    using var context = TestContext.Create();
    var manifest = context.Installation.Install(context.Request);
    Assert(manifest.Phase == InstallPhase.Committed, "Manifest is not committed.");
    Assert(manifest.Files.All(file => file.Owned), "All files should be owned.");
    Assert(File.Exists(Path.Combine(context.GameRoot, "CoopStoryBridge.asi")), "Bridge missing.");
    Assert(File.Exists(Path.Combine(context.GameRoot, "ScriptHookRDR2.dll")), "Runtime missing.");
    Assert(!File.Exists(Path.Combine(context.GameRoot, "NativeTrainer.asi")), "Trainer was copied.");
    Assert(
        !File.Exists(Path.Combine(context.GameRoot, "CoopStory.config.json")),
        "Secret-bearing sidecar config was copied into the game root.");
    foreach (var installed in Directory.EnumerateFiles(context.GameRoot))
    {
        Assert(
            !Encoding.UTF8.GetString(File.ReadAllBytes(installed))
                .Contains(context.Settings.SessionToken, StringComparison.Ordinal),
            $"Session token leaked into game root file {Path.GetFileName(installed)}.");
    }
    Assert(context.Installation.Verify(context.Request).IsInstalled, "Install did not verify.");

    context.Installation.Uninstall();
    Assert(!File.Exists(context.Paths.InstallManifestPath), "Manifest was not removed.");
    Assert(!File.Exists(Path.Combine(context.GameRoot, "CoopStoryBridge.asi")), "Bridge remains.");
    Assert(!File.Exists(Path.Combine(context.GameRoot, "ScriptHookRDR2.dll")), "Owned runtime remains.");
    Assert(File.Exists(context.GameExe), "Game executable was removed.");
}

static void TestPreexistingRuntime()
{
    using var context = TestContext.Create();
    File.Copy(
        Path.Combine(context.RuntimeBin, "ScriptHookRDR2.dll"),
        Path.Combine(context.GameRoot, "ScriptHookRDR2.dll"));
    File.Copy(
        Path.Combine(context.RuntimeBin, "dinput8.dll"),
        Path.Combine(context.GameRoot, "dinput8.dll"));

    var manifest = context.Installation.Install(context.Request);
    Assert(
        manifest.Files
            .Where(file => file.RelativePath is "ScriptHookRDR2.dll" or "dinput8.dll")
            .All(file => !file.Owned),
        "Pre-existing runtime was marked owned.");
    context.Installation.Uninstall();
    Assert(
        File.Exists(Path.Combine(context.GameRoot, "ScriptHookRDR2.dll")),
        "Pre-existing ScriptHook was removed.");
    Assert(
        File.Exists(Path.Combine(context.GameRoot, "dinput8.dll")),
        "Pre-existing dinput was removed.");
}

static void TestForeignAsi()
{
    using var context = TestContext.Create();
    File.WriteAllText(Path.Combine(context.GameRoot, "OtherMod.asi"), "foreign");
    var report = context.Installation.Verify(context.Request);
    Assert(!report.IsValid, "Foreign ASI was accepted.");
    Assert(
        report.Summary.Contains("OtherMod.asi", StringComparison.Ordinal),
        "Foreign ASI was not named.");
    Assert(!File.Exists(context.Paths.InstallManifestPath), "Verify wrote a manifest.");
    Assert(
        !File.Exists(Path.Combine(context.GameRoot, "CoopStoryBridge.asi")),
        "Verify changed game root.");
}

static void TestModifiedBridge()
{
    using var context = TestContext.Create();
    context.Installation.Install(context.Request);
    var bridge = Path.Combine(context.GameRoot, "CoopStoryBridge.asi");
    File.WriteAllText(bridge, "modified");
    ExpectThrows<LauncherException>(() => context.Installation.Uninstall());
    Assert(File.Exists(bridge), "Modified bridge was deleted.");
    Assert(File.Exists(context.Paths.InstallManifestPath), "Recovery manifest was deleted.");
}

static void TestPreparedRecovery()
{
    using var context = TestContext.Create();
    var committed = context.Installation.Install(context.Request);
    var bridge = committed.Files.Single(
        file => file.RelativePath == "CoopStoryBridge.asi");
    File.Delete(Path.Combine(context.GameRoot, bridge.RelativePath));
    var partialStage = Path.Combine(
        context.GameRoot,
        bridge.InstallStageRelativePath);
    File.WriteAllText(partialStage, "partial power-loss write");
    AtomicFile.WriteJson(
        context.Paths.InstallManifestPath,
        committed with { Phase = InstallPhase.Prepared },
        JsonSupport.Options);

    context.Installation.Uninstall();
    Assert(!File.Exists(partialStage), "Partial staging file remains.");
    Assert(!File.Exists(context.Paths.InstallManifestPath), "Recovery manifest remains.");
    Assert(
        !File.Exists(Path.Combine(context.GameRoot, "ScriptHookRDR2.dll")),
        "Owned runtime remains after recovery.");
}

static void TestForeignManifestRace()
{
    using var context = TestContext.Create();
    var foreignInstallId = Guid.NewGuid();
    var racingService = new InstallationService(
        context.Paths,
        context.Policy,
        context.Logger,
        () => false,
        prepared =>
        {
            AtomicFile.WriteJson(
                context.Paths.InstallManifestPath,
                prepared with
                {
                    InstallId = foreignInstallId,
                    Phase = InstallPhase.Committed
                },
                JsonSupport.Options);
            throw new IOException("Injected manifest replacement race.");
        });

    ExpectThrows<LauncherException>(() => racingService.Install(context.Request));
    Assert(
        File.Exists(context.Paths.InstallManifestPath),
        "Rollback deleted the foreign manifest.");
    var remaining = JsonSerializer.Deserialize<InstallManifest>(
        File.ReadAllBytes(context.Paths.InstallManifestPath),
        JsonSupport.Options)
        ?? throw new InvalidOperationException(
            "Foreign manifest became unreadable.");
    Assert(
        remaining.InstallId == foreignInstallId &&
        remaining.Phase == InstallPhase.Committed,
        "Rollback replaced or altered the foreign committed manifest.");
    Assert(
        !File.Exists(Path.Combine(context.GameRoot, "CoopStoryBridge.asi")),
        "Failed race left an installed bridge.");
}

static void TestWrongGameHash()
{
    using var context = TestContext.Create();
    File.WriteAllText(context.GameExe, "different game");
    var report = context.Installation.Verify(context.Request);
    Assert(!report.IsValid, "Wrong game hash was accepted.");
    Assert(
        report.Summary.Contains("Unsupported", StringComparison.Ordinal),
        "Wrong game error is unclear.");
}

static void TestGuestConfig()
{
    using var context = TestContext.Create();
    var guest = context.Settings with
    {
        Role = LauncherRole.Guest,
        HostAddress = "192.168.0.10"
    };
    var bytes = SidecarConfiguration.CreateBytes(guest, context.Paths);
    using var document = JsonDocument.Parse(bytes);
    var root = document.RootElement;
    Assert(root.GetProperty("role").GetString() == "Guest", "Role is not Guest.");
    Assert(
        root.GetProperty("hostAddress").GetString() == "192.168.0.10",
        "Host address mismatch.");
    Assert(
        root.GetProperty("sessionToken").GetString() == guest.SessionToken,
        "Token mismatch.");
    Assert(
        root.GetProperty("nickname").GetString() == guest.Nickname,
        "Nickname mismatch.");
    Assert(
        !root.GetProperty("inGameMenuEnabled").GetBoolean(),
        "Complete launcher JOIN should not wait for the in-game menu.");
    Assert(root.GetProperty("safety").GetProperty("storyModeOnly").GetBoolean(), "Safety off.");
    Assert(root.GetProperty("safety").GetProperty("refuseOnlineMode").GetBoolean(), "RDO guard off.");
}

static void TestLobbyStatus()
{
    var initial = new LauncherLobbySnapshot(
        "Arthur",
        LauncherRole.Host,
        string.Empty,
        LauncherRole.Guest,
        string.Empty,
        true,
        false,
        false,
        null);
    var peer = SidecarProcessService.ApplyLobbyStatusLine(
        initial,
        "COOP_LOBBY_STATUS={\"event\":\"peer\",\"connected\":true," +
        "\"address\":\"192.168.0.10\"}");
    var identity = SidecarProcessService.ApplyLobbyStatusLine(
        peer,
        "COOP_LOBBY_STATUS={\"event\":\"identity\"," +
        "\"nickname\":\"Sadie\"}");
    var bridge = SidecarProcessService.ApplyLobbyStatusLine(
        identity,
        "COOP_LOBBY_STATUS={\"event\":\"bridge\",\"connected\":true}");

    Assert(bridge.PeerConnected, "Peer state did not reach the lobby.");
    Assert(
        bridge.RemoteAddress == "192.168.0.10",
        "Remote IPv4 did not reach the lobby.");
    Assert(
        bridge.RemoteNickname == "Sadie",
        "Remote nickname did not reach the lobby.");
    Assert(
        bridge.GameBridgeConnected,
        "Game bridge state did not reach the lobby.");
    Assert(
        SidecarProcessService.ApplyLobbyStatusLine(
            bridge,
            "COOP_LOBBY_STATUS={broken") == bridge,
        "Malformed sidecar status changed the lobby.");
}

static void TestNicknameConfig()
{
    using var context = TestContext.Create();
    var settings = context.Settings with { Nickname = "Ranger 🤠" };
    var bytes = SidecarConfiguration.CreateBytes(settings, context.Paths);
    using var document = JsonDocument.Parse(bytes);
    Assert(
        document.RootElement.GetProperty("nickname").GetString() ==
        settings.Nickname,
        "Unicode nickname did not reach sidecar config.");

    foreach (var invalid in new[]
             {
                 string.Empty,
                 new string('x', 25),
                 string.Concat(Enumerable.Repeat("🤠", 17)),
                 " leading",
                 "line\nbreak",
                 "left\u202Eright",
                 "bad~n~name"
             })
    {
        ExpectThrows<LauncherException>(
            () => SidecarConfiguration.ValidateSettings(
                settings with { Nickname = invalid }));
    }
}

static void TestMotionReplicationModeConfig()
{
    using var context = TestContext.Create();

    var defaultSettings = new LauncherSettings();
    Assert(
        defaultSettings.MotionReplicationMode ==
        LauncherMotionReplicationMode.TaskNavmesh,
        "New launcher settings did not default to Task/Navmesh.");
    Assert(
        !defaultSettings.AnimSceneStoryVmProbeEnabled,
        "New launcher settings enabled Story VM Probe by default.");
    var defaultBytes = SidecarConfiguration.CreateBytes(
        context.Settings with
        {
            MotionReplicationMode =
                LauncherMotionReplicationMode.TaskNavmesh
        },
        context.Paths);
    using (var defaultDocument = JsonDocument.Parse(defaultBytes))
    {
        Assert(
            defaultDocument.RootElement
                .GetProperty("motionReplicationMode")
                .GetString() == "task_navmesh",
            "Default sidecar config did not select task_navmesh.");
        Assert(
            !defaultDocument.RootElement
                .GetProperty("animSceneStoryVmProbeEnabled")
                .GetBoolean(),
            "Default sidecar config enabled Story VM Probe.");
    }

    var experimental = context.Settings with
    {
        MotionReplicationMode =
            LauncherMotionReplicationMode.AnimGraphReplica,
        AnimSceneStoryVmProbeEnabled = true
    };
    SidecarConfiguration.Save(experimental, context.Paths);
    var sidecar = SidecarConfigStore
        .LoadAsync(context.Paths.SidecarConfigPath)
        .GetAwaiter()
        .GetResult();
    Assert(
        sidecar.MotionReplicationMode ==
        MotionReplicationMode.AnimGraphReplica,
        "AnimGraph Replica did not round-trip through sidecar config.");
    Assert(
        sidecar.AnimSceneStoryVmProbeEnabled,
        "Story VM Probe did not round-trip through sidecar config.");

    var store = new SettingsStore(context.Paths);
    store.Save(experimental);
    Assert(
        store.Load().MotionReplicationMode ==
        LauncherMotionReplicationMode.AnimGraphReplica,
        "AnimGraph Replica did not persist in launcher settings.");
    Assert(
        store.Load().AnimSceneStoryVmProbeEnabled,
        "Story VM Probe did not persist in launcher settings.");

    File.WriteAllText(
        context.Paths.SettingsPath,
        "{\"schemaVersion\":1}");
    Assert(
        store.Load().MotionReplicationMode ==
        LauncherMotionReplicationMode.TaskNavmesh,
        "Legacy launcher settings did not safely fall back to Task/Navmesh.");
    Assert(
        !store.Load().AnimSceneStoryVmProbeEnabled,
        "Legacy launcher settings did not keep Story VM Probe disabled.");
}

static void TestHostConfigValidation()
{
    using var context = TestContext.Create();
    var savePath = Path.Combine(context.Root, "SRDR30015");
    File.WriteAllText(savePath, "local host save fixture");
    var host = context.Settings with
    {
        Role = LauncherRole.Host,
        HostAddress = "192.168.50.10",
        HostSavePath = savePath
    };
    SidecarConfiguration.Save(host, context.Paths);
    var loaded = SidecarConfigStore
        .LoadAsync(context.Paths.SidecarConfigPath)
        .GetAwaiter()
        .GetResult();
    Assert(
        loaded.HostAddress == host.HostAddress,
        "Launcher replaced host LAN address.");
    Assert(
        loaded.Nickname == host.Nickname,
        "Launcher replaced the player nickname.");
    Assert(
        loaded.HostSave is
        {
            FileName: "SRDR30015",
            SelectedLocally: true,
            AutomaticGameLoad: false
        },
        "Host save metadata was not preserved.");
    Assert(
        loaded.HostSave!.Sha256 == Hashing.FileSha256(savePath),
        "Host save hash mismatch.");
    Assert(
        !File.ReadAllText(context.Paths.SidecarConfigPath)
            .Contains(savePath, StringComparison.OrdinalIgnoreCase),
        "Private host save path leaked into sidecar config.");
    _ = HostAddressValidator.Validate(loaded.HostAddress, requireRemote: true);
}

static void TestMenuBootstrapConfig()
{
    using var context = TestContext.Create();
    var blank = context.Settings with
    {
        HostAddress = string.Empty,
        SessionToken = string.Empty
    };
    var bytes = SidecarConfiguration.CreateBytes(blank, context.Paths);
    using var document = JsonDocument.Parse(bytes);
    var root = document.RootElement;
    Assert(root.GetProperty("inGameMenuEnabled").GetBoolean(), "Menu bootstrap disabled.");
    Assert(
        root.GetProperty("hostAddress").GetString() == "127.0.0.1",
        "Blank bootstrap host did not use the local placeholder.");
    Assert(
        !string.IsNullOrWhiteSpace(root.GetProperty("sessionToken").GetString()),
        "Blank bootstrap did not receive a temporary local token.");
}

static void TestBundledRuntimeRejected()
{
    using var context = TestContext.Create();
    File.WriteAllText(Path.Combine(context.Package.Root, "dinput8.dll"), "bundled");
    var report = context.Installation.Verify(context.Request);
    Assert(!report.IsValid, "Bundled runtime was accepted.");
    Assert(
        report.Summary.Contains("cannot contain", StringComparison.Ordinal),
        "Bundled runtime error is unclear.");
}

static void TestChangedPackageRejected()
{
    using var context = TestContext.Create();
    context.Installation.Install(context.Request);
    File.AppendAllText(context.Package.SidecarExePath, "tampered");
    ExpectThrows<LauncherException>(
        () => context.Installation.ValidateInstalled(context.Package));
}

static void TestPackageUpdate()
{
    using var context = TestContext.Create();
    var previous = context.Installation.Install(context.Request);
    File.WriteAllText(context.Package.BridgePath, "updated bridge payload");

    Assert(
        context.Installation.IsPackageUpdateAvailable(context.Package),
        "Changed package was not recognized as an available update.");
    var updated = context.Installation.UpdateToPackage(context.Request);

    Assert(
        updated.InstallId != previous.InstallId,
        "Update reused the previous install identity.");
    Assert(
        File.ReadAllText(Path.Combine(context.GameRoot, "CoopStoryBridge.asi")) ==
        "updated bridge payload",
        "Updated bridge was not installed.");
    _ = context.Installation.ValidateInstalled(context.Package);
}

static void TestDiagnosticsRedaction()
{
    using var context = TestContext.Create();
    const string inviteCode =
        "R2C1.ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_abcdefghijklmnopqrstuvwxyz";
    context.Installation.Install(context.Request);
    context.Logger.Info(
        "test.secret",
        $"sessionToken={context.Settings.SessionToken}; " +
        $"invite={inviteCode}; machine={Environment.MachineName}; " +
        $"root={context.GameRoot}");
    File.WriteAllText(
        context.Paths.SidecarConsoleLogPath,
        $"PAIRING_TOKEN={context.Settings.SessionToken}\n{context.Root}");
    File.WriteAllText(
        Path.Combine(context.Paths.LogDirectory, "bridge.log"),
        $"[ERROR][PUPPET_PLAYER] teleport failed; root={context.GameRoot}; " +
        $"token={context.Settings.SessionToken}{Environment.NewLine}");
    File.AppendAllLines(
        Path.Combine(context.Paths.LogDirectory, "bridge.log"),
        Enumerable.Range(0, 305).Select(
            static index =>
                $"[INFO][PUPPET_MOTION] recent-index-marker-{index:D3}"));
    File.AppendAllLines(
        Path.Combine(context.Paths.LogDirectory, "bridge.log"),
        [
            "[INFO][ANIMGRAPH_REPLICA] diagnostics-replica-marker enabled: direct network root",
            "[INFO][ANIMGRAPH_SAMPLE] diagnostics-sample-marker source=move-network phase=0.25",
            "[INFO][ACTION_TX] diagnostics-action-tx-marker actionId=7 phase=begin",
            "[INFO][ACTION_FSM] diagnostics-action-fsm-marker active=1 transitions=2",
            "[ERROR][ACTION_APPLY] diagnostics-action-apply-marker failed to cancel native task",
            "[INFO][ACTION_EPOCH] diagnostics-action-epoch-marker epoch=4 reset=1",
            "[INFO][MELEE_VISUAL] diagnostics-melee-visual-marker actionId=7 terminal=1",
            "[INFO][PEER_DISMOUNT] diagnostics-peer-dismount-marker actionId=10 confirmed=1",
            "[INFO][PEER_COMBAT] diagnostics-peer-combat-marker isolation=1",
            "[INFO][AIM_POSE] diagnostics-aim-pose-marker applied=18 frames=300",
            "[INFO][LASSO_ROPE] diagnostics-lasso-rope-marker actionId=8 state=attached",
            "[INFO][VICTIM_CONSTRAINT] diagnostics-victim-constraint-marker active=1 frames=120",
            "[INFO][MISSION_FSM] diagnostics-mission-fsm-marker epoch=3 phase=active",
            "[INFO][MISSION_TX] diagnostics-mission-tx-marker epoch=3 sequence=17",
            "[INFO][MISSION_RX] diagnostics-mission-rx-marker epoch=3 sequence=17",
            "[INFO][MISSION_WORLD] diagnostics-mission-world-marker state=mirrored",
            "[INFO][MISSION_PREFLIGHT] diagnostics-mission-preflight-marker ready=true",
            "[INFO][MISSION_SPECTATOR] diagnostics-mission-spectator-marker active=1",
            "[INFO][MISSION_DAMAGE] diagnostics-mission-damage-marker suppressed=2",
            "[INFO][MISSION_CHECKPOINT] diagnostics-mission-checkpoint-marker generation=4",
            "[INFO][MISSION_CAMERA][TX] diagnostics-mission-camera-tx-marker active=1",
            "[INFO][MISSION_CAMERA][RX] diagnostics-mission-camera-rx-marker active=1",
            "[WARN][MISSION_CAMERA][FALLBACK] diagnostics-mission-camera-fallback-marker stale=1",
            "[INFO][MISSION_OBJECTIVE] diagnostics-mission-objective-marker anchor-source=live-player",
            "[INFO][MISSION_SKIP][PREEMPTIVE] diagnostics-mission-skip-marker active=1",
            "[INFO][ANIMSCENE_HYBRID][CAPTURED] diagnostics-animscene-hybrid-marker roles=7",
            "[INFO][ANIMSCENE_REPLICA][ATTACHED] diagnostics-animscene-replica-marker phase=0.25",
            "[INFO][MISSION_ISOLATION] diagnostics-mission-isolation-marker quarantine=0",
            "[INFO][MISSION_ANCHOR] diagnostics-mission-anchor-marker source=live-player",
            "[INFO][ANIMGRAPH_TRAVERSAL] diagnostics-animgraph-traversal-marker task=vault",
            "[INFO][MOUNT_TX] diagnostics-mount-tx-marker actionId=9 phase=commit",
            "[INFO][MOUNT_LOCAL] diagnostics-mount-local-marker id=21 generation=2",
            "[WARN][MOUNT_REGISTRY] diagnostics-mount-registry-marker orphaned proxy",
            "[INFO][REMOTE_MOUNT_RX] diagnostics-remote-mount-rx-marker id=21 generation=2",
            "[INFO][REMOTE_MOUNT_RELATION] diagnostics-remote-mount-relation-marker reused=true",
            "[INFO][REMOTE_MOUNT] diagnostics-remote-mount-marker frames=300 max-error=0.4",
            "[INFO][WORLD_POOL] diagnostics-world-pool-summary-marker count=48 active=45",
            "[INFO][WORLD_PROXY_PHYSICS] diagnostics-world-proxy-marker corrections=3 frames=300",
            "[INFO][ENTITY_GRAPH] diagnostics-entity-graph-marker count=44 window=5s"
        ]);
    File.AppendAllText(
        context.Paths.SidecarLogPath,
        """{"timestampUtc":"2026-08-05T12:00:00Z","level":"info","event":"network.motion-mode.negotiated","message":"diagnostics-motion-mode-marker","data":{"mode":"AnimGraphReplica","peerModeNegotiated":true}}""" +
        Environment.NewLine,
        new UTF8Encoding(false));
    var recordings = Path.Combine(context.Paths.StateDirectory, "recordings");
    Directory.CreateDirectory(recordings);
    File.WriteAllText(
        Path.Combine(recordings, "ghost-last.json"),
        "{\"formatVersion\":1,\"frames\":[]}",
        new UTF8Encoding(false));
    var escapedGameRoot = JsonEncodedText.Encode(context.GameRoot).ToString();
    Assert(
        ReadSharedText(context.Paths.LauncherLogPath)
            .Contains(escapedGameRoot, StringComparison.OrdinalIgnoreCase),
        "Test fixture did not create a JSON-escaped game path.");

    var destination = Path.Combine(context.Root, "diagnostics.zip");
    var diagnostics = new DiagnosticsService(
        context.Paths,
        context.Policy,
        context.Logger);
    diagnostics.Export(destination, context.Settings, context.Package);

    using var archive = ZipFile.OpenRead(destination);
    Assert(
        archive.GetEntry("logs/bridge.log") is not null,
        "Persistent bridge log was not included in diagnostics.");
    Assert(
        archive.GetEntry("recordings/ghost-last.json") is not null,
        "Ghost Record route was not included in diagnostics.");
    Assert(
        archive.GetEntry("DIAGNOSTICS_SUMMARY.json") is not null &&
        archive.GetEntry("DIAGNOSTICS_ERRORS.txt") is not null &&
        archive.GetEntry("DIAGNOSTICS_WARNINGS.txt") is not null &&
        archive.GetEntry("DIAGNOSTICS_RUNTIME_SUMMARIES.txt") is not null,
        "Extracted diagnostic summaries and alerts were not included.");
    Assert(
        archive.GetEntry("TIMELINE.md") is not null &&
        archive.GetEntry("TIMELINE.json") is not null &&
        archive.GetEntry("ANOMALIES.md") is not null &&
        archive.GetEntry("ANOMALIES.json") is not null,
        "Timeline and anomaly reports were not included in the one diagnostics ZIP.");
    var indexEntry = archive.GetEntry("DIAGNOSTICS_INDEX.txt");
    Assert(indexEntry is not null, "Sorted diagnostics index was not included.");
    using (var indexReader = new StreamReader(indexEntry!.Open(), Encoding.UTF8))
    {
        var index = indexReader.ReadToEnd();
        Assert(
            index.Contains("ERRORS (", StringComparison.Ordinal) &&
            index.Contains("PUPPET_PLAYER (", StringComparison.Ordinal) &&
            index.Contains("ANIMGRAPH_REPLICA (", StringComparison.Ordinal) &&
            index.Contains("ANIMGRAPH_SAMPLE (", StringComparison.Ordinal) &&
            index.Contains("NETWORK_MOTION_MODE (", StringComparison.Ordinal) &&
            index.Contains("ACTION_TX (", StringComparison.Ordinal) &&
            index.Contains("ACTION_FSM (", StringComparison.Ordinal) &&
            index.Contains("ACTION_APPLY (", StringComparison.Ordinal) &&
            index.Contains("ACTION_EPOCH (", StringComparison.Ordinal) &&
            index.Contains("MELEE_VISUAL (", StringComparison.Ordinal) &&
            index.Contains("PEER_DISMOUNT (", StringComparison.Ordinal) &&
            index.Contains("PEER_COMBAT (", StringComparison.Ordinal) &&
            index.Contains("AIM_POSE (", StringComparison.Ordinal) &&
            index.Contains("LASSO_ROPE (", StringComparison.Ordinal) &&
            index.Contains("VICTIM_CONSTRAINT (", StringComparison.Ordinal) &&
            index.Contains("MISSION_FSM (", StringComparison.Ordinal) &&
            index.Contains("MISSION_TX (", StringComparison.Ordinal) &&
            index.Contains("MISSION_RX (", StringComparison.Ordinal) &&
            index.Contains("MISSION_WORLD (", StringComparison.Ordinal) &&
            index.Contains("MISSION_PREFLIGHT (", StringComparison.Ordinal) &&
            index.Contains("MISSION_SPECTATOR (", StringComparison.Ordinal) &&
            index.Contains("MISSION_DAMAGE (", StringComparison.Ordinal) &&
            index.Contains("MISSION_CHECKPOINT (", StringComparison.Ordinal) &&
            index.Contains("MISSION_CAMERA_TX (", StringComparison.Ordinal) &&
            index.Contains("MISSION_CAMERA_RX (", StringComparison.Ordinal) &&
            index.Contains("MISSION_CAMERA_FALLBACK (", StringComparison.Ordinal) &&
            index.Contains("MISSION_OBJECTIVE (", StringComparison.Ordinal) &&
            index.Contains("MISSION_SKIP (", StringComparison.Ordinal) &&
            index.Contains("ANIMSCENE_HYBRID (", StringComparison.Ordinal) &&
            index.Contains("ANIMSCENE_REPLICA (", StringComparison.Ordinal) &&
            index.Contains("MISSION_ISOLATION (", StringComparison.Ordinal) &&
            index.Contains("MISSION_ANCHOR (", StringComparison.Ordinal) &&
            index.Contains("ANIMGRAPH_TRAVERSAL (", StringComparison.Ordinal) &&
            index.Contains("MOUNT_TX (", StringComparison.Ordinal) &&
            index.Contains("MOUNT_REGISTRY (", StringComparison.Ordinal) &&
            index.Contains("REMOTE_MOUNT (", StringComparison.Ordinal) &&
            index.Contains("WORLD_POOL (", StringComparison.Ordinal) &&
            index.Contains("WORLD_PROXY_PHYSICS (", StringComparison.Ordinal) &&
            index.Contains("ENTITY_GRAPH (", StringComparison.Ordinal) &&
            index.Contains("SESSION_HEALTH (", StringComparison.Ordinal) &&
            index.Contains("MISSION_TIMELINE (", StringComparison.Ordinal) &&
            index.Contains("PLAYER_DIVERGENCE (", StringComparison.Ordinal) &&
            index.Contains("ENTITY_DIVERGENCE (", StringComparison.Ordinal) &&
            index.Contains("COMBAT_LIFECYCLE (", StringComparison.Ordinal) &&
            index.Contains("LASSO_LIFECYCLE (", StringComparison.Ordinal) &&
            index.Contains("MOUNT_LIFECYCLE (", StringComparison.Ordinal) &&
            index.Contains("TRANSPORT_GAP (", StringComparison.Ordinal) &&
            index.Contains("GHOST_RECORD_REPLAY (", StringComparison.Ordinal),
            "Diagnostics index did not contain expected categories.");
        Assert(
            index.Contains("diagnostics-replica-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-sample-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-motion-mode-marker", StringComparison.Ordinal),
            "Diagnostics index did not route the AnimGraph and motion-mode markers.");
        Assert(
            index.Contains("diagnostics-action-tx-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-action-epoch-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-melee-visual-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-peer-dismount-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-peer-combat-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-victim-constraint-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-mission-fsm-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-mission-preflight-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-mission-damage-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-mission-camera-tx-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-mission-camera-rx-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-mission-camera-fallback-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-mission-objective-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-mission-isolation-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-mission-anchor-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-animgraph-traversal-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-mount-local-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-mount-registry-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-remote-mount-rx-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-remote-mount-relation-marker", StringComparison.Ordinal) &&
            index.Contains("diagnostics-world-proxy-marker", StringComparison.Ordinal),
            "Diagnostics index did not route the action, mount, and world markers.");
        var networkStart = index.IndexOf(
            "===== NETWORK (",
            StringComparison.Ordinal);
        var networkEnd = networkStart < 0
            ? -1
            : index.IndexOf(
                "=====",
                networkStart + "===== NETWORK (".Length,
                StringComparison.Ordinal);
        var networkSection = networkStart < 0
            ? string.Empty
            : index[networkStart..(
                networkEnd < 0 ? index.Length : networkEnd)];
        Assert(
            !networkSection.Contains(
                "diagnostics-peer-combat-marker",
                StringComparison.Ordinal),
            "Generic peer combat text was incorrectly routed to NETWORK.");
        Assert(
            index.Contains(
                "recent-index-marker-304",
                StringComparison.Ordinal),
            "Diagnostics index did not include the newest categorized line.");
        Assert(
            !index.Contains(
                "recent-index-marker-000",
                StringComparison.Ordinal),
            "Diagnostics index retained an obsolete categorized line.");
    }
    using (var summary = ReadJsonEntry(archive, "summary.json"))
    {
        Assert(
            summary.RootElement
                .GetProperty("motionReplicationMode")
                .GetString() == context.Settings.MotionReplicationMode.ToString(),
            "Diagnostics summary omitted the selected motion replication mode.");
    }
    using (var diagnosticSummary = ReadJsonEntry(
               archive,
               "DIAGNOSTICS_SUMMARY.json"))
    {
        var categories = diagnosticSummary.RootElement.GetProperty("categories");
        Assert(
            categories.GetProperty("ACTION_TX").GetProperty("matched").GetInt32() > 0 &&
            categories.GetProperty("ACTION_EPOCH").GetProperty("matched").GetInt32() > 0 &&
            categories.GetProperty("MELEE_VISUAL").GetProperty("matched").GetInt32() > 0 &&
            categories.GetProperty("PEER_DISMOUNT").GetProperty("matched").GetInt32() > 0 &&
            categories.GetProperty("PEER_COMBAT").GetProperty("matched").GetInt32() > 0 &&
            categories.GetProperty("MISSION_FSM").GetProperty("matched").GetInt32() > 0 &&
            categories.GetProperty("MISSION_TX").GetProperty("matched").GetInt32() > 0 &&
            categories.GetProperty("MISSION_RX").GetProperty("matched").GetInt32() > 0 &&
            categories.GetProperty("MISSION_WORLD").GetProperty("matched").GetInt32() > 0 &&
            categories.GetProperty("MISSION_PREFLIGHT").GetProperty("matched").GetInt32() > 0 &&
            categories.GetProperty("MISSION_SPECTATOR").GetProperty("matched").GetInt32() > 0 &&
            categories.GetProperty("MISSION_DAMAGE").GetProperty("matched").GetInt32() > 0 &&
            categories.GetProperty("MISSION_CHECKPOINT").GetProperty("matched").GetInt32() > 0 &&
            categories.GetProperty("MISSION_CAMERA_TX").GetProperty("matched").GetInt32() > 0 &&
            categories.GetProperty("MISSION_CAMERA_RX").GetProperty("matched").GetInt32() > 0 &&
            categories.GetProperty("MISSION_CAMERA_FALLBACK").GetProperty("matched").GetInt32() > 0 &&
            categories.GetProperty("MISSION_OBJECTIVE").GetProperty("matched").GetInt32() > 0 &&
            categories.GetProperty("MOUNT_TX").GetProperty("matched").GetInt32() >= 2 &&
            categories.GetProperty("MOUNT_REGISTRY").GetProperty("matched").GetInt32() >= 3 &&
            categories.GetProperty("WORLD_POOL").GetProperty("matched").GetInt32() > 0,
            "Structured diagnostics summary omitted categorized matches.");
    }
    Assert(
        ReadZipText(archive, "DIAGNOSTICS_ERRORS.txt")
            .Contains("diagnostics-action-apply-marker", StringComparison.Ordinal),
        "Extracted errors did not contain the newest action failure.");
    Assert(
        ReadZipText(archive, "DIAGNOSTICS_WARNINGS.txt")
            .Contains("diagnostics-mount-registry-marker", StringComparison.Ordinal),
        "Extracted warnings did not contain the mount registry warning.");
    Assert(
        ReadZipText(archive, "DIAGNOSTICS_RUNTIME_SUMMARIES.txt")
            .Contains("diagnostics-world-pool-summary-marker", StringComparison.Ordinal),
        "Extracted runtime summaries did not contain the world-pool summary.");
    var privateValues = new[]
        {
            context.GameRoot,
            context.Settings.HostAddress,
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData)
        }
        .Where(static value => !string.IsNullOrWhiteSpace(value))
        .ToArray();
    foreach (var entry in archive.Entries)
    {
        using var reader = new StreamReader(entry.Open(), Encoding.UTF8);
        var text = reader.ReadToEnd();
        Assert(
            !text.Contains(context.Settings.SessionToken, StringComparison.Ordinal),
            $"Token leaked in {entry.FullName}.");
        Assert(
            !text.Contains(inviteCode, StringComparison.Ordinal),
            $"Invite code leaked in {entry.FullName}.");
        Assert(
            !text.Contains(Environment.MachineName, StringComparison.OrdinalIgnoreCase),
            $"Machine name leaked in {entry.FullName}.");
        foreach (var privateValue in privateValues)
        {
            Assert(
                !text.Contains(privateValue, StringComparison.OrdinalIgnoreCase),
                $"Literal private path leaked in {entry.FullName}.");
            Assert(
                !text.Contains(
                    JsonEncodedText.Encode(privateValue).ToString(),
                    StringComparison.OrdinalIgnoreCase),
                $"JSON-escaped private path leaked in {entry.FullName}.");
            Assert(
                !NormalizePath(text).Contains(
                    NormalizePath(privateValue),
                    StringComparison.OrdinalIgnoreCase),
                $"Normalized private path leaked in {entry.FullName}.");
        }

        if (entry.FullName.EndsWith(".json", StringComparison.OrdinalIgnoreCase))
        {
            using var document = JsonDocument.Parse(text);
            AssertJsonHasNoPrivateValues(
                document.RootElement,
                privateValues,
                entry.FullName);
        }
        else if (entry.FullName.EndsWith(".jsonl", StringComparison.OrdinalIgnoreCase))
        {
            foreach (var line in text.Split(
                         ['\r', '\n'],
                         StringSplitOptions.RemoveEmptyEntries))
            {
                using var document = JsonDocument.Parse(line);
                AssertJsonHasNoPrivateValues(
                    document.RootElement,
                    privateValues,
                    entry.FullName);
            }
        }
    }
}

static void TestDiagnosticsTimelineAndAnomalies()
{
    using var context = TestContext.Create();
    File.WriteAllLines(
        context.Paths.SidecarLogPath,
        [
            """{"timestamp":"2026-08-06T12:00:00.000Z","level":"info","event":"runtime.started","message":"Latest diagnostic session started."}""",
            """{"timestamp":"2026-08-06T12:00:00.000Z","level":"info","event":"session.health.ready","message":"Host and guest are authenticated through [2001:db8::5]:43120.","data":{"role":"Host","peerConnected":true,"peerAddress":"25.0.0.25","sessionFingerprint":"222222222222"}}""",
            """{"timestamp":"2026-08-06T12:00:01.000Z","level":"info","event":"mission.timeline.active","message":"Mission phase became Active.","data":{"role":"Host","missionEpoch":7,"revision":2}}""",
            """{"timestamp":"2026-08-06T12:00:01.050Z","level":"info","event":"bridge.user-marker","message":"[USER_MARKER] tester marked visible desync.","data":{"role":"Host","markerId":1,"markerCorrelationId":281474993487873}}""",
            """{"timestamp":"2026-08-06T12:00:01.100Z","level":"info","event":"player.divergence.event","message":"[PLAYER_DIVERGENCE][EVENT] state=first-divergence","data":{"role":"Guest","maxErrorMeters":1.75,"sampleErrorMeters":0.5,"rotationErrorDegrees":40,"actionFreshnessMs":800,"actionActive":1}}""",
            """{"timestamp":"2026-08-06T12:00:01.250Z","level":"info","event":"entity.divergence.event","message":"[ENTITY_DIVERGENCE][EVENT] state=first-divergence","data":{"role":"Host","maxErrorMeters":2.5,"missing":1,"divergent":1}}""",
            """{"timestamp":"2026-08-06T12:00:01.300Z","level":"info","event":"diagnostics.problem-snapshot","message":"[PROBLEM_SNAPSHOT] sampled cover and lasso state.","data":{"role":"Host","markerCorrelationId":281474993487873,"localCover":1,"localLasso":1}}""",
            """{"timestamp":"2026-08-06T12:00:01.400Z","level":"info","event":"combat.lifecycle","message":"Guest melee attack entered blocking state.","data":{"role":"Guest","actionId":9}}""",
            """{"timestamp":"2026-08-06T12:00:01.550Z","level":"info","event":"lasso.lifecycle","message":"Guest lasso rope attached and victim tied.","data":{"role":"Guest","actionId":10}}""",
            """{"timestamp":"2026-08-06T12:00:01.700Z","level":"info","event":"mount.lifecycle","message":"Host horse dismount committed.","data":{"role":"Host","actionId":11}}""",
            """{"timestamp":"2026-08-06T12:00:04.700Z","level":"info","event":"diagnostics.streaming","message":"Aggregated message flow.","data":{"role":"Host","messageFlow":[{"direction":"network-to-bridge","messageType":"PlayerState","observed":50,"delivered":47,"dropped":3,"coalesced":2,"lastObservedAgeMs":120,"averageGapMs":45,"p95GapMs":650,"maximumGapMs":4200}]}}""",
            """{"timestamp":"2026-08-06T12:00:04.800Z","level":"warning","event":"diagnostics.transport-gap","message":"Guest transport gap after reconnect from 25.0.0.25.","data":{"role":"Guest","gapMs":3300,"jitterMs":80}}""",
            """{"timestamp":"2026-08-06T12:00:05.000Z","level":"info","event":"diagnostics.transport-recovered","message":"Guest transport flow recovered.","data":{"role":"Guest","gapMs":40}}"""
        ],
        new UTF8Encoding(false));
    File.WriteAllText(
        context.Paths.SidecarLogPath + ".3",
        """{"timestamp":"2026-08-06T11:00:00Z","level":"info","event":"runtime.started","message":"Third-newest diagnostic session started."}""" +
        Environment.NewLine +
        """{"timestamp":"2026-08-06T11:00:01Z","level":"info","event":"session.handshake.begin","message":"TOO_OLD diagnostic session.","data":{"role":"Host","sessionFingerprint":"000000000000"}}""" +
        Environment.NewLine,
        new UTF8Encoding(false));
    File.WriteAllText(
        context.Paths.SidecarLogPath + ".2",
        """{"timestamp":"2026-08-06T11:59:59Z","level":"info","event":"runtime.started","message":"Second-newest diagnostic session started."}""" +
        Environment.NewLine +
        """{"timestamp":"2026-08-06T11:59:59Z","level":"info","event":"session.handshake.begin","message":"Rotated authentication.","data":{"role":"Host","sessionFingerprint":"111111111111"}}""" +
        Environment.NewLine +
        """{"timestamp":"2026-08-06T11:59:59.050Z","level":"info","event":"player.divergence.event","message":"[PLAYER_DIVERGENCE][EVENT] state=first-divergence","data":{"role":"Guest","maxErrorMeters":9.0}}""" +
        Environment.NewLine,
        new UTF8Encoding(false));
    File.WriteAllText(
        context.Paths.SidecarLogPath + ".1",
        """{"timestamp":"2026-08-06T11:59:59.500Z","level":"info","event":"session.health.rotated","message":"Newest rotated sidecar segment.","data":{"role":"Host"}}""" +
        Environment.NewLine,
        new UTF8Encoding(false));
    var bridgeLog = Path.Combine(context.Paths.LogDirectory, "bridge.log");
    File.WriteAllText(
        bridgeLog + ".3",
        "2026-08-06T11:59:58.100Z [CoopStoryBridge] [SESSION_HEALTH] rotated=3" +
        Environment.NewLine,
        new UTF8Encoding(false));
    File.WriteAllText(
        bridgeLog + ".2",
        "2026-08-06T11:59:59.100Z [CoopStoryBridge] [MISSION_TIMELINE] rotated=2" +
        Environment.NewLine,
        new UTF8Encoding(false));
    File.WriteAllText(
        bridgeLog + ".1",
        "2026-08-06T11:59:59.600Z [CoopStoryBridge] [ANIMGRAPH_TRAVERSAL] rotated=1" +
        Environment.NewLine,
        new UTF8Encoding(false));

    var destination = Path.Combine(context.Root, "timeline.zip");
    var diagnostics = new DiagnosticsService(
        context.Paths,
        context.Policy,
        context.Logger);
    diagnostics.Export(destination, context.Settings, context.Package);

    using var archive = ZipFile.OpenRead(destination);
    Assert(
        archive.GetEntry("TIMELINE.md") is not null &&
         archive.GetEntry("TIMELINE.json") is not null &&
         archive.GetEntry("ANOMALIES.md") is not null &&
         archive.GetEntry("ANOMALIES.json") is not null &&
         archive.GetEntry("MARKER_WINDOWS.md") is not null &&
         archive.GetEntry("MARKER_WINDOWS.json") is not null,
        "Correlated diagnostics reports were not written to the export ZIP.");
    foreach (var obsoleteRotatedEntry in new[]
             {
                 "logs/sidecar.jsonl.3",
                 "logs/sidecar.jsonl.2",
                 "logs/sidecar.jsonl.1",
                 "logs/bridge.log.3",
                 "logs/bridge.log.2",
                 "logs/bridge.log.1"
             })
    {
        Assert(
            archive.GetEntry(obsoleteRotatedEntry) is null,
            $"Obsolete rotated diagnostic segment {obsoleteRotatedEntry} was exported.");
    }
    Assert(
        archive.GetEntry("DIAGNOSTICS_SESSIONS.json") is not null,
        "Diagnostic session-window manifest was not exported.");
    var combinedSidecar = ReadZipText(archive, "logs/sidecar.jsonl");
    Assert(
        !combinedSidecar.Contains("TOO_OLD", StringComparison.Ordinal) &&
        combinedSidecar.Contains(
            "Second-newest diagnostic session started.",
            StringComparison.Ordinal) &&
        combinedSidecar.Contains(
            "Latest diagnostic session started.",
            StringComparison.Ordinal),
        "Raw diagnostic logs were not limited to the last two sessions.");

    using (var timeline = ReadJsonEntry(archive, "TIMELINE.json"))
    {
        var root = timeline.RootElement;
        Assert(
            root.GetProperty("latestSessionFingerprint").GetString() ==
            "222222222222",
            "Timeline did not select the newest session fingerprint.");
        var events = root.GetProperty("events").EnumerateArray().ToArray();
        Assert(events.Length >= 8, "Timeline omitted categorized events.");
        Assert(
            events.Any(item =>
                item.GetProperty("sessionFingerprint").GetString() ==
                "111111111111") &&
            events.Any(item =>
                item.GetProperty("sessionFingerprint").GetString() ==
                "222222222222") &&
            !events.Any(item =>
                item.GetProperty("sessionFingerprint").GetString() ==
                "000000000000"),
            "Timeline did not preserve both rotated session boundaries.");
        Assert(
            events.First().GetProperty("timestampUtc").GetDateTimeOffset() >=
            DateTimeOffset.Parse("2026-08-06T11:59:59Z"),
            "The two selected diagnostic sessions were not analyzed oldest-to-newest.");
        Assert(
            events.Any(item => item.GetProperty("role").GetString() == "Host") &&
            events.Any(item => item.GetProperty("role").GetString() == "Guest"),
            "Timeline did not identify both Host and Guest event roles.");
        Assert(
            events.SelectMany(item => item.GetProperty("categories").EnumerateArray())
                .Any(category => category.GetString() == "MISSION_TIMELINE") &&
            events.SelectMany(item => item.GetProperty("categories").EnumerateArray())
                .Any(category => category.GetString() == "COMBAT_LIFECYCLE") &&
            events.SelectMany(item => item.GetProperty("categories").EnumerateArray())
                .Any(category => category.GetString() == "LASSO_LIFECYCLE") &&
            events.SelectMany(item => item.GetProperty("categories").EnumerateArray())
                .Any(category => category.GetString() == "MOUNT_LIFECYCLE") &&
            events.SelectMany(item => item.GetProperty("categories").EnumerateArray())
                .Any(category => category.GetString() == "TRANSPORT_GAP") &&
             events.SelectMany(item => item.GetProperty("categories").EnumerateArray())
                .Any(category => category.GetString() == "USER_MARKER") &&
            events.SelectMany(item => item.GetProperty("categories").EnumerateArray())
                .Any(category => category.GetString() == "PROBLEM_SNAPSHOT"),
            "Timeline omitted mission/combat/lasso/mount lifecycle categories.");
        var streamingEvent = events.Single(item =>
            item.GetProperty("eventName").GetString() ==
            "diagnostics.streaming");
        Assert(
            streamingEvent.GetProperty("transportBudgetExceeded").GetBoolean() &&
            streamingEvent.GetProperty("metrics").EnumerateObject().Any(metric =>
                metric.Name.EndsWith(
                    ".maximumGapMs",
                    StringComparison.Ordinal) &&
                metric.Value.GetDouble() == 4200),
            "Parser did not descend into data.messageFlow[] or apply its budget.");
        var timestamps = events
            .Select(item => item.GetProperty("timestampUtc"))
            .Where(static item => item.ValueKind == JsonValueKind.String)
            .Select(static item => DateTimeOffset.Parse(item.GetString()!))
            .ToArray();
        Assert(
            timestamps.SequenceEqual(timestamps.Order()),
            "Timeline was not chronological.");
    }

    using (var anomalies = ReadJsonEntry(archive, "ANOMALIES.json"))
    {
        var root = anomalies.RootElement;
        Assert(
            root.GetProperty("latestSessionFingerprint").GetString() ==
            "222222222222",
            "Anomaly report did not select the newest session fingerprint.");
        Assert(
            root.GetProperty("firstDivergence")
                .GetProperty("eventName").GetString() == "player.divergence.event" &&
            root.GetProperty("firstDivergence")
                .GetProperty("sessionFingerprint").GetString() ==
                "222222222222" &&
            root.GetProperty("firstDivergence")
                .GetProperty("metrics")
                .EnumerateObject()
                .Any(metric =>
                    metric.Name.EndsWith(
                        "maxErrorMeters",
                        StringComparison.OrdinalIgnoreCase) &&
                    metric.Value.GetDouble() == 1.75),
            "First divergence mixed the older and newest fingerprint sessions.");
        var statistics = root.GetProperty("statistics");
        Assert(
            statistics.GetProperty("playerDivergenceMeters")
                .GetProperty("max").GetDouble() == 1.75,
            "Player divergence maximum was not calculated.");
        Assert(
            statistics.GetProperty("playerRotationDegrees")
                .GetProperty("max").GetDouble() == 40 &&
            statistics.GetProperty("playerActionFreshnessMilliseconds")
                .GetProperty("max").GetDouble() == 800,
            "Meters, degrees and milliseconds were not kept in separate statistics.");
        Assert(
            statistics.GetProperty("entityDivergenceMeters")
                .GetProperty("p95").GetDouble() == 2.5,
            "Entity divergence percentile was not calculated.");
        Assert(
            statistics.GetProperty("transportGapMilliseconds")
                .GetProperty("max").GetDouble() >= 4200,
            "Transport gap statistics did not consume messageFlow percentiles/maxima.");
        Assert(
            root.GetProperty("anomalies").EnumerateArray().Any(item =>
                item.GetProperty("kind").GetString() == "USER_MARKER"),
            "F9 USER_MARKER was not promoted into anomaly correlation.");
        Assert(
            root.GetProperty("anomalies").EnumerateArray().Any(item =>
                item.GetProperty("eventData").GetProperty("eventName").GetString() ==
                "diagnostics.transport-recovered"),
            "Transport recovery was not retained beside the transport gap anomaly.");
        var correlated = root.GetProperty("anomalies")
            .EnumerateArray()
            .First(item =>
                item.GetProperty("eventData")
                    .GetProperty("eventName").GetString() == "player.divergence.event")
            .GetProperty("correlatedEventIds")
            .GetArrayLength();
        Assert(
            correlated >= 3,
            "Player divergence was not chronologically correlated with nearby mission/entity events.");
    }

    using (var markerWindows = ReadJsonEntry(archive, "MARKER_WINDOWS.json"))
    {
        var root = markerWindows.RootElement;
        Assert(
            root.GetProperty("markerCount").GetInt32() == 1 &&
            root.GetProperty("windowBeforeMs").GetInt32() == 10_000 &&
            root.GetProperty("windowAfterMs").GetInt32() == 15_000,
            "F7 marker windows did not use the expected bounded context.");
        var marker = root.GetProperty("markers").EnumerateArray().Single();
        Assert(
            marker.GetProperty("correlationId").GetString() ==
                "281474993487873" &&
            marker.GetProperty("events").EnumerateArray().Any(item =>
                item.GetProperty("categories").EnumerateArray().Any(category =>
                    category.GetString() == "PROBLEM_SNAPSHOT")) &&
            marker.GetProperty("events").GetArrayLength() >= 8,
            "Marker report did not retain the diagnostic burst and nearby lifecycle events.");
    }

    var timelineMarkdown = ReadZipText(archive, "TIMELINE.md");
    var anomaliesMarkdown = ReadZipText(archive, "ANOMALIES.md");
    var markerMarkdown = ReadZipText(archive, "MARKER_WINDOWS.md");
    Assert(
        timelineMarkdown.Contains("Host", StringComparison.Ordinal) &&
        timelineMarkdown.Contains("Guest", StringComparison.Ordinal) &&
        anomaliesMarkdown.Contains("First divergence", StringComparison.Ordinal) &&
        anomaliesMarkdown.Contains("P95", StringComparison.Ordinal) &&
        markerMarkdown.Contains("281474993487873", StringComparison.Ordinal),
        "Human-readable timeline/anomaly reports are incomplete.");

    foreach (var entry in archive.Entries)
    {
        using var reader = new StreamReader(entry.Open(), Encoding.UTF8);
        var text = reader.ReadToEnd();
        Assert(
            !text.Contains("25.0.0.25", StringComparison.Ordinal) &&
            !text.Contains("2001:db8::5", StringComparison.OrdinalIgnoreCase) &&
            !text.Contains(context.Settings.HostAddress, StringComparison.Ordinal),
            $"Network address leaked in {entry.FullName}.");
    }
}

static void TestDiagnosticsQuietSessionNoFalsePositives()
{
    using var context = TestContext.Create();
    File.WriteAllLines(
        context.Paths.SidecarLogPath,
        [
            """{"timestamp":"2026-08-06T10:00:00Z","level":"info","event":"runtime.summary","message":"[PUPPET_PLAYER] summary error=0 distance=0.2 offset=0.1","data":{"role":"Guest"}}""",
            """{"timestamp":"2026-08-06T10:00:01Z","level":"info","event":"player.divergence.summary","message":"[PLAYER_DIVERGENCE] available=1","data":{"role":"Guest","positionErrorMeters":1.0,"rotationErrorDegrees":20,"actionFreshnessMs":500,"actionActive":0}}""",
            """{"timestamp":"2026-08-06T10:00:02Z","level":"info","event":"entity.divergence.summary","message":"[ENTITY_DIVERGENCE] desired=4 live=4 missing=0 divergent=0","data":{"role":"Guest","worstPositionErrorMeters":0.4,"missing":0,"divergent":0}}""",
            """{"timestamp":"2026-08-06T10:00:03Z","level":"info","event":"network.control.delivered","message":"Delivered EquipmentState to the authenticated peer.","data":{"control":"EquipmentState","direction":"bridge-to-network","delivered":true,"sessionFingerprint":"abc123def456"}}""",
            """{"timestamp":"2026-08-06T10:10:00Z","level":"info","event":"diagnostics.streaming","message":"Quiet streaming summary with no unavailable messages.","data":{"role":"Guest","networkToBridgeUnavailable":0,"networkToBridgeBacklog":0,"messageFlow":[{"direction":"network-to-bridge","messageType":"PlayerState","observed":100,"delivered":100,"dropped":0,"coalesced":0,"lastObservedAgeMs":100,"averageGapMs":50,"p95GapMs":100,"maximumGapMs":200}]}}"""
        ],
        new UTF8Encoding(false));

    var destination = Path.Combine(context.Root, "quiet-diagnostics.zip");
    var diagnostics = new DiagnosticsService(
        context.Paths,
        context.Policy,
        context.Logger);
    diagnostics.Export(destination, context.Settings, context.Package);

    using var archive = ZipFile.OpenRead(destination);
    using (var anomalies = ReadJsonEntry(archive, "ANOMALIES.json"))
    {
        var root = anomalies.RootElement;
        Assert(
            root.GetProperty("firstDivergence").ValueKind == JsonValueKind.Null &&
            root.GetProperty("firstPlayerDivergence").ValueKind == JsonValueKind.Null &&
            root.GetProperty("firstEntityDivergence").ValueKind == JsonValueKind.Null,
            "Zero or below-budget summaries created a false divergence.");
        Assert(
            root.GetProperty("totalDetectedAnomalies").GetInt32() == 0,
            "Quiet summaries created a false anomaly.");
        Assert(
            !root.GetProperty("statistics")
                .GetProperty("transportGapMilliseconds")
                .GetProperty("available").GetBoolean(),
            "A quiet messageFlow summary created false transport-gap statistics.");
    }
    using (var timeline = ReadJsonEntry(archive, "TIMELINE.json"))
    {
        var streaming = timeline.RootElement.GetProperty("events")
            .EnumerateArray()
            .Single(item =>
                item.GetProperty("eventName").GetString() ==
                "diagnostics.streaming");
        Assert(
            !streaming.GetProperty("categories")
                .EnumerateArray()
                .Any(category => category.GetString() == "TRANSPORT_GAP") &&
            !streaming.GetProperty("transportBudgetExceeded").GetBoolean(),
            "Normal streaming/backlog/unavailable=0 was classified as a transport gap.");
        Assert(
            streaming.GetProperty("sessionFingerprint").GetString() ==
            "abc123def456",
            "A session fingerprint from routine control traffic was not propagated.");
        Assert(
            !timeline.RootElement.GetProperty("events")
                .EnumerateArray()
                .Any(item => item.GetProperty("eventName").GetString() ==
                    "network.control.delivered"),
            "Routine controlled EquipmentState traffic polluted SESSION_HEALTH.");
        Assert(
            streaming.GetProperty("metrics").EnumerateObject().Any(metric =>
                metric.Name.EndsWith(
                    ".maximumGapMs",
                    StringComparison.Ordinal) &&
                metric.Value.GetDouble() == 200),
            "Nested data.messageFlow[] was not parsed in the quiet-session fixture.");
    }
}

static void TestDiagnosticsObservedRole()
{
    using var context = TestContext.Create();
    File.WriteAllLines(
        context.Paths.SidecarLogPath,
        [
            """{"timestamp":"2026-07-28T12:00:00Z","event":"session.in-game-selected","data":{"role":"Host"}}""",
            """{"timestamp":"2026-07-28T12:01:00Z","event":"unrelated","data":{"role":"Host"}}""",
            """{"timestamp":"2026-07-28T12:02:00Z","event":"session.in-game-selected","data":{"role":"Guest"}}"""
        ],
        new UTF8Encoding(false));
    var diagnostics = new DiagnosticsService(
        context.Paths,
        context.Policy,
        context.Logger);
    var destination = Path.Combine(context.Root, "observed-role.zip");
    diagnostics.Export(
        destination,
        context.Settings with { Role = LauncherRole.Host },
        context.Package);

    using var archive = ZipFile.OpenRead(destination);
    using var summary = ReadJsonEntry(archive, "summary.json");
    Assert(
        summary.RootElement.GetProperty("role").GetString() == "Guest",
        "Summary used the stale launcher role.");
    Assert(
        summary.RootElement.GetProperty("roleSource").GetString() ==
        "sidecar.session.in-game-selected",
        "Summary did not explain the observed role source.");
    using var settings = ReadJsonEntry(archive, "settings-redacted.json");
    Assert(
        settings.RootElement
            .GetProperty("settings")
            .GetProperty("role")
            .GetString() == "Guest",
        "Redacted settings used the stale launcher role.");
}

static void TestDiagnosticsReplacement()
{
    using var context = TestContext.Create();
    var diagnostics = new DiagnosticsService(
        context.Paths,
        context.Policy,
        context.Logger);
    var destination = Path.Combine(context.Root, "replace-me.zip");
    diagnostics.Export(destination, context.Settings, context.Package);
    context.Logger.Info("replacement.marker", "second export");
    diagnostics.Export(destination, context.Settings, context.Package);

    using var archive = ZipFile.OpenRead(destination);
    Assert(
        archive.GetEntry("summary.json") is not null &&
        archive.GetEntry("logs/launcher.jsonl") is not null,
        "Replacement diagnostics archive is incomplete.");
    Assert(
        !Directory.EnumerateFiles(
                context.Root,
                ".replace-me.zip.*.tmp",
                SearchOption.TopDirectoryOnly)
            .Any(),
        "Temporary diagnostics archive remained after replacement.");
}

static void TestEscapedWindowsPathRedaction()
{
    const string privatePath =
        @"C:\Users\Example User\Résumé\RDR2 Coop\logs";
    var redactor = new SecretRedactor([privatePath]);
    var jsonEscaped = JsonEncodedText.Encode(privatePath).ToString();
    var unicodeEscaped = jsonEscaped.Replace(
        "\\\\",
        "\\u005C",
        StringComparison.Ordinal);
    var source = JsonSerializer.Serialize(new { literal = privatePath }) +
                 Environment.NewLine +
                 unicodeEscaped;
    var redacted = redactor.Redact(source);

    Assert(
        !redacted.Contains(privatePath, StringComparison.OrdinalIgnoreCase),
        "Literal Windows path was not redacted.");
    Assert(
        !redacted.Contains(
            jsonEscaped,
            StringComparison.OrdinalIgnoreCase),
        "JSON-escaped Windows path was not redacted.");
    Assert(
        !redacted.Contains(
            unicodeEscaped,
            StringComparison.OrdinalIgnoreCase),
        "Unicode-escaped Windows separators were not redacted.");
}

static JsonDocument ReadJsonEntry(
    ZipArchive archive,
    string entryName)
{
    var entry = archive.GetEntry(entryName)
        ?? throw new InvalidOperationException(
            $"Missing diagnostics entry {entryName}.");
    using var reader = new StreamReader(entry.Open(), Encoding.UTF8);
    return JsonDocument.Parse(reader.ReadToEnd());
}

static string ReadZipText(ZipArchive archive, string entryName)
{
    var entry = archive.GetEntry(entryName)
        ?? throw new InvalidOperationException(
            $"Missing diagnostics entry {entryName}.");
    using var reader = new StreamReader(entry.Open(), Encoding.UTF8);
    return reader.ReadToEnd();
}

static void AssertJsonHasNoPrivateValues(
    JsonElement element,
    IReadOnlyList<string> privateValues,
    string source)
{
    switch (element.ValueKind)
    {
        case JsonValueKind.Object:
            foreach (var property in element.EnumerateObject())
            {
                AssertJsonHasNoPrivateValues(
                    property.Value,
                    privateValues,
                    source);
            }
            break;
        case JsonValueKind.Array:
            foreach (var item in element.EnumerateArray())
            {
                AssertJsonHasNoPrivateValues(item, privateValues, source);
            }
            break;
        case JsonValueKind.String:
            var value = NormalizePath(element.GetString() ?? string.Empty);
            foreach (var privateValue in privateValues)
            {
                Assert(
                    !value.Contains(
                        NormalizePath(privateValue),
                        StringComparison.OrdinalIgnoreCase),
                    $"Parsed JSON string leaked a private path in {source}.");
            }
            break;
    }
}

static string NormalizePath(string value) =>
    value
        .Replace("\\\\", "\\", StringComparison.Ordinal)
        .Replace('/', '\\')
        .TrimEnd('\\');

static string ReadSharedText(string path)
{
    using var stream = new FileStream(
        path,
        FileMode.Open,
        FileAccess.Read,
        FileShare.ReadWrite | FileShare.Delete);
    using var reader = new StreamReader(stream, Encoding.UTF8);
    return reader.ReadToEnd();
}

static void Assert(bool condition, string message)
{
    if (!condition)
    {
        throw new InvalidOperationException(message);
    }
}

static void ExpectThrows<T>(Action action)
    where T : Exception
{
    try
    {
        action();
    }
    catch (T)
    {
        return;
    }

    throw new InvalidOperationException($"Expected exception {typeof(T).Name}.");
}

sealed class TestContext : IDisposable
{
    private TestContext(
        string root,
        LauncherPaths paths,
        LauncherLogger logger,
        LauncherPolicy policy,
        PackageLayout package,
        string gameRoot,
        string gameExe,
        string runtimeBin,
        LauncherSettings settings,
        InstallationService installation)
    {
        Root = root;
        Paths = paths;
        Logger = logger;
        Policy = policy;
        Package = package;
        GameRoot = gameRoot;
        GameExe = gameExe;
        RuntimeBin = runtimeBin;
        Settings = settings;
        Installation = installation;
        Request = new InstallRequest(settings, package);
    }

    public string Root { get; }

    public LauncherPaths Paths { get; }

    public LauncherLogger Logger { get; }

    public LauncherPolicy Policy { get; }

    public PackageLayout Package { get; }

    public string GameRoot { get; }

    public string GameExe { get; }

    public string RuntimeBin { get; }

    public LauncherSettings Settings { get; }

    public InstallationService Installation { get; }

    public InstallRequest Request { get; }

    public static TestContext Create()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "CoopStoryLauncherSelfTest",
            Guid.NewGuid().ToString("N"));
        var packageRoot = Path.Combine(root, "package");
        var config = Path.Combine(packageRoot, "config");
        var sidecar = Path.Combine(packageRoot, "sidecar");
        var gameRoot = Path.Combine(root, "game");
        var runtimeRoot = Path.Combine(root, "runtime");
        var runtimeBin = Path.Combine(runtimeRoot, "bin");
        Directory.CreateDirectory(config);
        Directory.CreateDirectory(sidecar);
        Directory.CreateDirectory(gameRoot);
        Directory.CreateDirectory(runtimeBin);

        var gameExe = Path.Combine(gameRoot, "RDR2.exe");
        var bridge = Path.Combine(packageRoot, "CoopStoryBridge.asi");
        var configTemplate = Path.Combine(config, "coopstory.example.json");
        var sidecarExe = Path.Combine(sidecar, "CoopStory.Sidecar.exe");
        var scriptHook = Path.Combine(runtimeBin, "ScriptHookRDR2.dll");
        var dinput = Path.Combine(runtimeBin, "dinput8.dll");
        File.WriteAllBytes(gameExe, Encoding.UTF8.GetBytes("supported fake RDR2"));
        File.WriteAllBytes(bridge, Encoding.UTF8.GetBytes("bridge payload"));
        File.WriteAllText(configTemplate, "{}");
        File.WriteAllBytes(sidecarExe, Encoding.UTF8.GetBytes("sidecar payload"));
        File.WriteAllBytes(scriptHook, Encoding.UTF8.GetBytes("script hook runtime"));
        File.WriteAllBytes(dinput, Encoding.UTF8.GetBytes("dinput runtime"));
        File.WriteAllText(Path.Combine(runtimeBin, "NativeTrainer.asi"), "ignored");

        var paths = LauncherPaths.CreateUnder(Path.Combine(root, "state"));
        paths.EnsureDirectories();
        var logger = new LauncherLogger(paths.LauncherLogPath);
        var policy = new LauncherPolicy
        {
            GameSha256 = Hashing.FileSha256(gameExe),
            ScriptHookSha256 = Hashing.FileSha256(scriptHook),
            DinputSha256 = Hashing.FileSha256(dinput),
            SupportedGameVersion = "self-test"
        };
        var package = new PackageLayout(
            packageRoot,
            bridge,
            configTemplate,
            sidecar,
            sidecarExe);
        var settings = new LauncherSettings
        {
            GameExePath = gameExe,
            ScriptHookFolder = runtimeRoot,
            Role = LauncherRole.Host,
            HostAddress = "192.168.50.10",
            SessionToken = SessionCodeService.Generate()
        };
        var installation = new InstallationService(
            paths,
            policy,
            logger,
            () => false);
        return new TestContext(
            root,
            paths,
            logger,
            policy,
            package,
            gameRoot,
            gameExe,
            runtimeBin,
            settings,
            installation);
    }

    public void Dispose()
    {
        Logger.Dispose();
        if (Directory.Exists(Root))
        {
            Directory.Delete(Root, recursive: true);
        }
    }
}
