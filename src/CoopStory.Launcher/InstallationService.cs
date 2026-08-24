using System.Diagnostics;
using System.Text.Json;

namespace CoopStory.Launcher;

public sealed class InstallationService
{
    private static readonly string[] ForbiddenPackageFileNames =
    [
        "ScriptHookRDR2.dll",
        "dinput8.dll",
        "NativeTrainer.asi"
    ];

    private static readonly string[] KnownModDirectories =
    [
        "lml",
        "RampageFiles",
        "RedM",
        "ModManager"
    ];

    private readonly LauncherPaths _paths;
    private readonly LauncherPolicy _policy;
    private readonly LauncherLogger _logger;
    private readonly Func<bool> _isGameOrSidecarRunning;
    private readonly Action<InstallManifest>? _preparedManifestSavedHook;

    public InstallationService(
        LauncherPaths paths,
        LauncherPolicy policy,
        LauncherLogger logger,
        Func<bool>? isGameOrSidecarRunning = null,
        Action<InstallManifest>? preparedManifestSavedHook = null)
    {
        _paths = paths;
        _policy = policy;
        _logger = logger;
        _isGameOrSidecarRunning = isGameOrSidecarRunning ?? DefaultProcessCheck;
        _preparedManifestSavedHook = preparedManifestSavedHook;
        _paths.EnsureDirectories();
    }

    public bool HasManifest => File.Exists(_paths.InstallManifestPath);

    public VerificationReport Verify(InstallRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        try
        {
            if (HasManifest)
            {
                var manifest = ValidateInstalled(request.Package);
                var messages = new List<string>
                {
                    "The mod is installed and its manifest is valid.",
                    $"RDR2: {manifest.GameRoot}",
                    $"Game build: {_policy.SupportedGameVersion}",
                    $"Installation state: {manifest.Phase}"
                };
                _logger.Info("verify.installed_ok", string.Join(" | ", messages));
                return new VerificationReport(
                    true,
                    true,
                    messages,
                    request.Package.Root,
                    manifest.GameRoot,
                    null);
            }

            var plan = CreateInstallPlan(request);
            var reportMessages = new List<string>
            {
                "Verification completed successfully; installation is allowed.",
                $"RDR2.exe has a supported hash ({_policy.SupportedGameVersion}).",
                "ScriptHookRDR2.dll and dinput8.dll have the expected hashes.",
                $"Role: {request.Settings.Role}.",
                "No other .asi files or known mod loaders were detected."
            };
            if (plan.Runtime.TrainerWasPresent)
            {
                reportMessages.Add(
                    "NativeTrainer.asi was found in the Script Hook archive but will not be copied.");
            }

            _logger.Info("verify.ready", string.Join(" | ", reportMessages));
            return new VerificationReport(
                true,
                false,
                reportMessages,
                request.Package.Root,
                plan.GameRoot,
                plan.Runtime.Root);
        }
        catch (Exception exception)
        {
            _logger.Warning("verify.rejected", exception.Message);
            return new VerificationReport(
                false,
                HasManifest,
                [exception.Message],
                request.Package.Root,
                null,
                null);
        }
    }

    public InstallManifest Install(InstallRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (HasManifest)
        {
            throw new LauncherException(
                "An installation manifest already exists. Use 'Uninstall' first.");
        }

        var plan = CreateInstallPlan(request);
        var installId = Guid.NewGuid();
        var tag = installId.ToString("N");
        var entries = plan.Files.Select(file => new InstalledFileRecord
        {
            RelativePath = file.RelativePath,
            Sha256 = file.Sha256,
            Length = file.Length,
            Owned = file.Owned,
            InstallStageRelativePath =
                $".coopstory-{tag}-install-{SafeName(file.RelativePath)}.tmp",
            UninstallStageRelativePath =
                $".coopstory-{tag}-uninstall-{SafeName(file.RelativePath)}.tmp"
        }).ToArray();
        var manifest = new InstallManifest
        {
            InstallId = installId,
            MachineId = GetOrCreateMachineId(),
            CreatedAtUtc = DateTimeOffset.UtcNow,
            Phase = InstallPhase.Prepared,
            GameRoot = plan.GameRoot,
            GameRootFingerprint = FingerprintPath(plan.GameRoot),
            GameExecutableSha256 = plan.GameHash,
            PackageContentSha256 = plan.PackageHash,
            Files = entries
        };

        var committedTargets = new List<(string Path, string Hash)>();
        var stagingPaths = new List<string>();
        try
        {
            // The durable ownership journal exists before the first byte is
            // staged in the game directory. An interrupted install can
            // therefore always be recovered through Uninstall().
            SaveManifest(manifest);
            _preparedManifestSavedHook?.Invoke(manifest);

            for (var index = 0; index < plan.Files.Count; index++)
            {
                var file = plan.Files[index];
                if (!file.Owned)
                {
                    continue;
                }

                var staging = SafeGamePath(
                    plan.GameRoot,
                    entries[index].InstallStageRelativePath);
                if (File.Exists(staging))
                {
                    throw new LauncherException(
                        $"A staging file already exists: {Path.GetFileName(staging)}");
                }

                stagingPaths.Add(staging);
                if (file.Bytes is not null)
                {
                    WriteNewDurable(staging, file.Bytes);
                }
                else
                {
                    CopyNewDurable(
                        file.SourcePath
                            ?? throw new LauncherException("An installation file source is missing."),
                        staging);
                }

                if (!Hashing.FileSha256(staging).Equals(
                        file.Sha256,
                        StringComparison.OrdinalIgnoreCase))
                {
                    throw new LauncherException(
                        $"Staging validation failed for {file.RelativePath}.");
                }

            }

            for (var index = 0; index < entries.Length; index++)
            {
                var entry = entries[index];
                if (!entry.Owned)
                {
                    continue;
                }

                var target = SafeGamePath(plan.GameRoot, entry.RelativePath);
                var staging = SafeGamePath(
                    plan.GameRoot,
                    entry.InstallStageRelativePath);
                if (File.Exists(target))
                {
                    throw new LauncherException(
                        $"A target file appeared during installation: {entry.RelativePath}");
                }

                File.Move(staging, target);
                committedTargets.Add((target, entry.Sha256));
            }

            foreach (var entry in entries)
            {
                var target = SafeGamePath(plan.GameRoot, entry.RelativePath);
                if (!File.Exists(target) ||
                    !Hashing.FileSha256(target).Equals(
                        entry.Sha256,
                        StringComparison.OrdinalIgnoreCase))
                {
                    throw new LauncherException(
                        $"Final file validation failed: {entry.RelativePath}");
                }
            }

            manifest = manifest with { Phase = InstallPhase.Committed };
            SaveManifest(manifest);
            try
            {
                _logger.Info(
                    "install.completed",
                    $"Installed {entries.Count(static entry => entry.Owned)} " +
                    $"owned files in {plan.GameRoot}; installId={installId:D}.");
            }
            catch
            {
                // A logging failure after the durable Committed transition
                // cannot turn a successful install into a rollback.
            }
            return manifest;
        }
        catch (Exception exception)
        {
            var currentManifest = TryReadManifestRelaxed();
            if (currentManifest is not null &&
                currentManifest.InstallId == installId &&
                currentManifest.Phase == InstallPhase.Committed &&
                ManifestTargetsMatch(currentManifest))
            {
                // SaveManifest(Committed) is the commit point. If a later
                // operation reported failure, preserve the committed install.
                return currentManifest;
            }

            var rollbackComplete = true;
            foreach (var (path, hash) in committedTargets.AsEnumerable().Reverse())
            {
                rollbackComplete &= DeleteOnlyIfHashMatches(path, hash);
            }

            foreach (var staging in stagingPaths)
            {
                if (File.Exists(staging))
                {
                    try
                    {
                        File.Delete(staging);
                    }
                    catch (Exception)
                    {
                        rollbackComplete = false;
                    }
                }
            }

            if (rollbackComplete)
            {
                rollbackComplete = TryDeleteOwnPreparedManifest(installId);
            }

            try
            {
                _logger.Error("install.failed", exception);
            }
            catch
            {
                // Preserve the original installation failure.
            }
            throw new LauncherException(
                (rollbackComplete
                    ? $"Installation failed and was rolled back: {exception.Message}"
                    : $"Installation failed. The manifest was retained for safe " +
                      $"recovery through 'Uninstall': {exception.Message}") +
                AccessRecoveryHint(exception),
                exception);
        }
    }

    private InstallManifest? TryReadManifestRelaxed()
    {
        try
        {
            if (!File.Exists(_paths.InstallManifestPath))
            {
                return null;
            }

            var manifest = JsonSerializer.Deserialize<InstallManifest>(
                File.ReadAllBytes(_paths.InstallManifestPath),
                JsonSupport.Options);
            return manifest is { SchemaVersion: 1 } ? manifest : null;
        }
        catch (Exception)
        {
            return null;
        }
    }

    private bool TryDeleteOwnPreparedManifest(Guid expectedInstallId)
    {
        if (!File.Exists(_paths.InstallManifestPath))
        {
            return true;
        }

        try
        {
            // Re-read immediately before deletion. The per-user process mutex
            // prevents a second launcher from replacing this journal.
            var current = JsonSerializer.Deserialize<InstallManifest>(
                File.ReadAllBytes(_paths.InstallManifestPath),
                JsonSupport.Options);
            if (current is null ||
                current.SchemaVersion != 1 ||
                current.InstallId != expectedInstallId ||
                current.Phase != InstallPhase.Prepared)
            {
                return false;
            }

            File.Delete(_paths.InstallManifestPath);
            return !File.Exists(_paths.InstallManifestPath);
        }
        catch (Exception)
        {
            return false;
        }
    }

    private static bool ManifestTargetsMatch(InstallManifest manifest)
    {
        try
        {
            foreach (var entry in manifest.Files)
            {
                var target = SafeGamePath(manifest.GameRoot, entry.RelativePath);
                if (!File.Exists(target) ||
                    !Hashing.FileSha256(target).Equals(
                        entry.Sha256,
                        StringComparison.OrdinalIgnoreCase))
                {
                    return false;
                }
            }

            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

    public void Uninstall()
    {
        EnsureProcessesStopped();
        var manifest = ReadManifest();
        ValidateManifestIdentity(manifest);
        PrevalidateOwnedFiles(manifest);

        var uninstalling = manifest with { Phase = InstallPhase.Uninstalling };
        SaveManifest(uninstalling);
        var moved = new List<(string Temporary, string Target)>();
        try
        {
            foreach (var entry in uninstalling.Files.Where(static item => item.Owned))
            {
                var target = SafeGamePath(uninstalling.GameRoot, entry.RelativePath);
                var temporary = SafeGamePath(
                    uninstalling.GameRoot,
                    entry.UninstallStageRelativePath);

                if (File.Exists(temporary))
                {
                    EnsureHash(temporary, entry.Sha256, entry.RelativePath);
                    continue;
                }

                if (!File.Exists(target))
                {
                    continue;
                }

                EnsureHash(target, entry.Sha256, entry.RelativePath);
                File.Move(target, temporary);
                moved.Add((temporary, target));
            }
        }
        catch (Exception exception)
        {
            foreach (var (temporary, target) in moved.AsEnumerable().Reverse())
            {
                if (File.Exists(temporary) && !File.Exists(target))
                {
                    try
                    {
                        File.Move(temporary, target);
                    }
                    catch (Exception)
                    {
                        // The manifest remains in Uninstalling state for recovery.
                    }
                }
            }

            _logger.Error("uninstall.move_failed", exception);
            throw new LauncherException(
                "Not all mod files could be hidden safely. " +
                "The manifest was retained; try again after closing the game." +
                AccessRecoveryHint(exception),
                exception);
        }

        foreach (var entry in uninstalling.Files.Where(static item => item.Owned))
        {
            var temporary = SafeGamePath(
                uninstalling.GameRoot,
                entry.UninstallStageRelativePath);
            var oldInstallStage = SafeGamePath(
                uninstalling.GameRoot,
                entry.InstallStageRelativePath);
            DeleteOnlyIfHashMatchesOrMissing(
                temporary,
                entry.Sha256,
                entry.RelativePath);
            // This unique path is owned by the durable manifest and is never
            // loader-visible. It may contain only a partial write after power
            // loss, so content-hash validation is intentionally not required.
            if (File.Exists(oldInstallStage))
            {
                File.Delete(oldInstallStage);
            }
        }

        File.Delete(_paths.InstallManifestPath);
        _logger.Info(
            "uninstall.completed",
            $"Removed only files owned by installation {manifest.InstallId:D}. " +
            "Script Hook files that existed before installation were preserved.");
    }

    public InstallManifest ValidateInstalled(PackageLayout? package = null)
    {
        var manifest = ReadManifest();
        ValidateManifestIdentity(manifest);
        if (manifest.Phase != InstallPhase.Committed)
        {
            throw new LauncherException(
                $"The installation is incomplete (state {manifest.Phase}). " +
                "Use 'Uninstall' and then install again.");
        }

        if (package is not null)
        {
            ValidatePackage(package);
            var packageHash = CalculatePackageHash(package);
            if (!packageHash.Equals(
                    manifest.PackageContentSha256,
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new LauncherException(
                    "The bridge/sidecar package content changed after installation. " +
                    "Uninstall the mod and use one complete package version.");
            }
        }

        EnsureNoForeignMods(manifest.GameRoot, allowOwnedBridge: true);
        foreach (var entry in manifest.Files)
        {
            var target = SafeGamePath(manifest.GameRoot, entry.RelativePath);
            if (!File.Exists(target))
            {
                throw new LauncherException(
                    $"An installed file is missing: {entry.RelativePath}");
            }

            EnsureHash(target, entry.Sha256, entry.RelativePath);
        }

        var gameExe = Path.Combine(manifest.GameRoot, "RDR2.exe");
        if (!File.Exists(gameExe) ||
            !Hashing.FileSha256(gameExe).Equals(
                _policy.GameSha256,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new LauncherException(
                "RDR2.exe changed after installation or has an unsupported build.");
        }

        return manifest;
    }

    public bool IsPackageUpdateAvailable(PackageLayout package)
    {
        ArgumentNullException.ThrowIfNull(package);
        if (!HasManifest)
        {
            return false;
        }

        var manifest = ValidateInstalled(package: null);
        ValidatePackage(package);
        return !CalculatePackageHash(package).Equals(
            manifest.PackageContentSha256,
            StringComparison.OrdinalIgnoreCase);
    }

    public InstallManifest UpdateToPackage(InstallRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        EnsureProcessesStopped();
        var installed = ValidateInstalled(package: null);
        SidecarConfiguration.ValidateSettings(request.Settings);
        ValidatePackage(request.Package);

        var gameExe = Path.GetFullPath(request.Settings.GameExePath);
        var gameRoot = Path.GetDirectoryName(gameExe)
            ?? throw new LauncherException("RDR2.exe has no parent directory.");
        if (!gameRoot.Equals(
                installed.GameRoot,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new LauncherException(
                "An update must target the same RDR2 directory as the current installation.");
        }

        if (!File.Exists(gameExe) ||
            !Path.GetFileName(gameExe).Equals(
                "RDR2.exe",
                StringComparison.OrdinalIgnoreCase) ||
            !Hashing.FileSha256(gameExe).Equals(
                _policy.GameSha256,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new LauncherException(
                "Update stopped: RDR2.exe has an unsupported build.");
        }

        if (PathsOverlap(gameRoot, request.Package.Root))
        {
            throw new LauncherException(
                "The launcher package and game directory cannot be nested inside each other.");
        }

        var runtime = PackageLocator.LocateRuntime(
            request.Settings.ScriptHookFolder);
        if (PathsOverlap(gameRoot, runtime.Root))
        {
            throw new LauncherException(
                "The Script Hook folder must be outside the game directory.");
        }

        EnsureHash(
            runtime.ScriptHookPath,
            _policy.ScriptHookSha256,
            "ScriptHookRDR2.dll");
        EnsureHash(runtime.DinputPath, _policy.DinputSha256, "dinput8.dll");

        var incomingHash = CalculatePackageHash(request.Package);
        if (incomingHash.Equals(
                installed.PackageContentSha256,
                StringComparison.OrdinalIgnoreCase))
        {
            return ValidateInstalled(request.Package);
        }

        _logger.Info(
            "update.started",
            $"Updating installation {installed.InstallId:D} to the new package content.");
        Uninstall();
        try
        {
            var updated = Install(request);
            _logger.Info(
                "update.completed",
                $"Installed the new package; installId={updated.InstallId:D}.");
            return updated;
        }
        catch (Exception exception)
        {
            _logger.Error("update.failed_safe_uninstalled", exception);
            throw new LauncherException(
                "The previous build was removed safely, but installation of the new " +
                "build failed. The game was left without an active mod. Correct the " +
                $"reported settings and try again: {exception.Message}",
                exception);
        }
    }

    private InstallPlan CreateInstallPlan(InstallRequest request)
    {
        EnsureProcessesStopped();
        SidecarConfiguration.ValidateSettings(request.Settings);
        ValidatePackage(request.Package);

        var gameExe = Path.GetFullPath(request.Settings.GameExePath);
        if (!File.Exists(gameExe) ||
            !Path.GetFileName(gameExe).Equals(
                "RDR2.exe",
                StringComparison.OrdinalIgnoreCase))
        {
            throw new LauncherException(
                "Select the exact RDR2.exe file with the 'Browse' button.");
        }

        if (PackageLocator.IsReparsePoint(gameExe))
        {
            throw new LauncherException("RDR2.exe cannot be a symbolic link.");
        }

        var gameRoot = Path.GetDirectoryName(gameExe)
            ?? throw new LauncherException("RDR2.exe has no parent directory.");
        if (PackageLocator.IsReparsePoint(gameRoot))
        {
            throw new LauncherException("The game directory cannot be a symbolic link.");
        }

        if (PathsOverlap(gameRoot, request.Package.Root))
        {
            throw new LauncherException(
                "The launcher package and game directory cannot be nested inside each other.");
        }

        var gameHash = Hashing.FileSha256(gameExe);
        if (!gameHash.Equals(_policy.GameSha256, StringComparison.OrdinalIgnoreCase))
        {
            throw new LauncherException(
                $"Unsupported RDR2.exe. The launcher requires build " +
                $"{_policy.SupportedGameVersion}.");
        }

        EnsureNoForeignMods(gameRoot, allowOwnedBridge: false);
        var runtime = PackageLocator.LocateRuntime(request.Settings.ScriptHookFolder);
        if (PathsOverlap(gameRoot, runtime.Root))
        {
            throw new LauncherException(
                "Select a separately extracted Script Hook folder; do not select the game directory.");
        }

        EnsureHash(runtime.ScriptHookPath, _policy.ScriptHookSha256, "ScriptHookRDR2.dll");
        EnsureHash(runtime.DinputPath, _policy.DinputSha256, "dinput8.dll");

        var files = new List<PlannedFile>
        {
            PlannedFile.FromSource("CoopStoryBridge.asi", request.Package.BridgePath, owned: true),
            PlanRuntime(gameRoot, "ScriptHookRDR2.dll", runtime.ScriptHookPath),
            PlanRuntime(gameRoot, "dinput8.dll", runtime.DinputPath)
        };

        var obsoleteConfig = SafeGamePath(gameRoot, "CoopStory.config.json");
        if (File.Exists(obsoleteConfig))
        {
            throw new LauncherException(
                "CoopStory.config.json from an older installer remains in the game directory. " +
                "Run that installer's uninstaller before using the new launcher.");
        }

        foreach (var file in files.Where(static item => item.Owned))
        {
            var target = SafeGamePath(gameRoot, file.RelativePath);
            if (File.Exists(target))
            {
                throw new LauncherException(
                    $"File {file.RelativePath} already exists without a launcher manifest. " +
                    "The launcher refuses to overwrite it.");
            }
        }

        return new InstallPlan(
            gameRoot,
            gameHash,
            runtime,
            files,
            CalculatePackageHash(request.Package));
    }

    private PlannedFile PlanRuntime(
        string gameRoot,
        string relativePath,
        string sourcePath)
    {
        var sourceHash = Hashing.FileSha256(sourcePath);
        var target = SafeGamePath(gameRoot, relativePath);
        if (!File.Exists(target))
        {
            return PlannedFile.FromSource(relativePath, sourcePath, owned: true);
        }

        if (PackageLocator.IsReparsePoint(target))
        {
            throw new LauncherException(
                $"{relativePath} in the game directory is a symbolic link. The operation was stopped.");
        }

        var targetHash = Hashing.FileSha256(target);
        if (!targetHash.Equals(sourceHash, StringComparison.OrdinalIgnoreCase))
        {
            throw new LauncherException(
                $"{relativePath} already exists but has a different hash. " +
                "The launcher will not overwrite a foreign runtime.");
        }

        return PlannedFile.FromSource(relativePath, sourcePath, owned: false);
    }

    private void ValidatePackage(PackageLayout package)
    {
        if (!Directory.Exists(package.Root) ||
            PackageLocator.IsReparsePoint(package.Root) ||
            !File.Exists(package.BridgePath) ||
            !File.Exists(package.ConfigTemplatePath) ||
            !File.Exists(package.SidecarExePath))
        {
            throw new LauncherException("The mod package is incomplete.");
        }

        var count = 0;
        long bytes = 0;
        var pending = new Queue<string>();
        pending.Enqueue(package.Root);
        while (pending.Count > 0)
        {
            var directory = pending.Dequeue();
            foreach (var path in Directory.EnumerateFileSystemEntries(
                         directory,
                         "*",
                         SearchOption.TopDirectoryOnly))
            {
                if (PackageLocator.IsReparsePoint(path))
                {
                    throw new LauncherException(
                        "The mod package contains a forbidden symbolic link.");
                }

                if (Directory.Exists(path))
                {
                    pending.Enqueue(path);
                    continue;
                }

                count++;
                bytes += new FileInfo(path).Length;
                if (count > 5000 || bytes > 4L * 1024 * 1024 * 1024)
                {
                    throw new LauncherException(
                        "The package exceeds the safe size limit.");
                }

                if (ForbiddenPackageFileNames.Contains(
                        Path.GetFileName(path),
                        StringComparer.OrdinalIgnoreCase))
                {
                    throw new LauncherException(
                        $"The mod package cannot contain {Path.GetFileName(path)}. " +
                        "Script Hook must remain a separate user-supplied prerequisite.");
                }
            }
        }
    }

    private static string CalculatePackageHash(PackageLayout package)
    {
        var selected = Directory
            .EnumerateFiles(package.SidecarDirectory, "*", SearchOption.AllDirectories)
            .Append(package.BridgePath)
            .Append(package.ConfigTemplatePath)
            .Select(path => new
            {
                Relative = Path.GetRelativePath(package.Root, path).Replace('\\', '/'),
                Hash = Hashing.FileSha256(path),
                Length = new FileInfo(path).Length
            })
            .OrderBy(static item => item.Relative, StringComparer.OrdinalIgnoreCase)
            .Select(static item => $"{item.Relative}|{item.Length}|{item.Hash}");
        return Hashing.TextSha256(string.Join("\n", selected));
    }

    private void EnsureNoForeignMods(string gameRoot, bool allowOwnedBridge)
    {
        var allowed = allowOwnedBridge
            ? new HashSet<string>(StringComparer.OrdinalIgnoreCase)
            {
                "CoopStoryBridge.asi"
            }
            : [];
        var foreignAsi = Directory
            .EnumerateFiles(gameRoot, "*.asi", SearchOption.TopDirectoryOnly)
            .Select(Path.GetFileName)
            .Where(name => name is not null && !allowed.Contains(name))
            .Cast<string>()
            .ToArray();
        if (foreignAsi.Length > 0)
        {
            throw new LauncherException(
                "Remove or move other .asi mods before testing: " +
                string.Join(", ", foreignAsi));
        }

        var foundDirectories = KnownModDirectories
            .Where(name => Directory.Exists(Path.Combine(gameRoot, name)))
            .ToArray();
        if (foundDirectories.Length > 0)
        {
            throw new LauncherException(
                "Directories from other loaders/mods were detected: " +
                string.Join(", ", foundDirectories) +
                ". The test requires a clean game directory.");
        }
    }

    private InstallManifest ReadManifest()
    {
        if (!File.Exists(_paths.InstallManifestPath))
        {
            throw new LauncherException(
                "No manifest was found. The launcher will not guess what to remove.");
        }

        try
        {
            var manifest = JsonSerializer.Deserialize<InstallManifest>(
                File.ReadAllBytes(_paths.InstallManifestPath),
                JsonSupport.Options);
            if (manifest is null || manifest.SchemaVersion != 1)
            {
                throw new LauncherException(
                    "The installation manifest has an unsupported version.");
            }

            return manifest;
        }
        catch (JsonException exception)
        {
            throw new LauncherException("The installation manifest is corrupted.", exception);
        }
    }

    private void SaveManifest(InstallManifest manifest) =>
        AtomicFile.WriteJson(_paths.InstallManifestPath, manifest, JsonSupport.Options);

    private void ValidateManifestIdentity(InstallManifest manifest)
    {
        if (manifest.InstallId == Guid.Empty ||
            manifest.Files.Count is < 1 or > 8 ||
            !manifest.MachineId.Equals(GetOrCreateMachineId()))
        {
            throw new LauncherException(
                "The manifest does not belong to this machine or has an invalid structure.");
        }

        var root = Path.GetFullPath(manifest.GameRoot).TrimEnd('\\', '/');
        if (!Directory.Exists(root) ||
            !FingerprintPath(root).Equals(
                manifest.GameRootFingerprint,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new LauncherException(
                "The manifest points to a different or nonexistent game directory.");
        }

        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var allowed = new HashSet<string>(
            ["CoopStoryBridge.asi", "ScriptHookRDR2.dll", "dinput8.dll"],
            StringComparer.OrdinalIgnoreCase);
        foreach (var entry in manifest.Files)
        {
            if (!allowed.Contains(entry.RelativePath) ||
                !seen.Add(entry.RelativePath) ||
                entry.Length < 0 ||
                entry.Sha256.Length != 64 ||
                entry.Sha256.Any(static character => !Uri.IsHexDigit(character)) ||
                entry.InstallStageRelativePath.Contains(Path.DirectorySeparatorChar) ||
                entry.UninstallStageRelativePath.Contains(Path.DirectorySeparatorChar))
            {
                throw new LauncherException("The manifest contains a forbidden file entry.");
            }

            _ = Convert.FromHexString(entry.Sha256);
            _ = SafeGamePath(root, entry.RelativePath);
            _ = SafeGamePath(root, entry.InstallStageRelativePath);
            _ = SafeGamePath(root, entry.UninstallStageRelativePath);
        }
    }

    private void PrevalidateOwnedFiles(InstallManifest manifest)
    {
        foreach (var entry in manifest.Files.Where(static item => item.Owned))
        {
            var target = SafeGamePath(manifest.GameRoot, entry.RelativePath);
            var uninstallStage = SafeGamePath(
                manifest.GameRoot,
                entry.UninstallStageRelativePath);
            foreach (var candidate in new[] { target, uninstallStage })
            {
                if (File.Exists(candidate))
                {
                    EnsureHash(candidate, entry.Sha256, entry.RelativePath);
                }
            }
        }
    }

    private Guid GetOrCreateMachineId()
    {
        if (File.Exists(_paths.MachineIdPath))
        {
            var text = File.ReadAllText(_paths.MachineIdPath).Trim();
            if (Guid.TryParseExact(text, "D", out var existing) &&
                existing != Guid.Empty)
            {
                return existing;
            }

            throw new LauncherException("The local machine identifier is corrupted.");
        }

        var created = Guid.NewGuid();
        AtomicFile.WriteText(_paths.MachineIdPath, created.ToString("D"));
        return created;
    }

    private void EnsureProcessesStopped()
    {
        if (_isGameOrSidecarRunning())
        {
            throw new LauncherException(
                "RDR2 or CoopStory.Sidecar is running. Close the game and test before changing files.");
        }
    }

    private static bool DefaultProcessCheck() =>
        IsProcessRunning("RDR2") || IsProcessRunning("CoopStory.Sidecar");

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

    private static bool PathsOverlap(string first, string second)
    {
        var left = Path.GetFullPath(first).TrimEnd('\\', '/');
        var right = Path.GetFullPath(second).TrimEnd('\\', '/');
        if (left.Equals(right, StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }

        return left.StartsWith(
                   right + Path.DirectorySeparatorChar,
                   StringComparison.OrdinalIgnoreCase) ||
               right.StartsWith(
                   left + Path.DirectorySeparatorChar,
                   StringComparison.OrdinalIgnoreCase);
    }

    private static string FingerprintPath(string path) =>
        Hashing.TextSha256(
            Path.GetFullPath(path).TrimEnd('\\', '/').ToLowerInvariant());

    private static string SafeGamePath(string gameRoot, string relativePath)
    {
        if (Path.IsPathRooted(relativePath) ||
            relativePath.Contains('/') ||
            relativePath.Contains('\\'))
        {
            throw new LauncherException("Forbidden relative file path.");
        }

        var root = Path.GetFullPath(gameRoot).TrimEnd('\\', '/');
        var target = Path.GetFullPath(Path.Combine(root, relativePath));
        var prefix = root + Path.DirectorySeparatorChar;
        if (!target.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
        {
            throw new LauncherException("The resolved file is outside the game directory.");
        }

        return target;
    }

    private static string SafeName(string relativePath) =>
        new(relativePath
            .Select(character => char.IsLetterOrDigit(character) ? character : '-')
            .ToArray());

    private static void EnsureHash(
        string path,
        string expected,
        string displayName)
    {
        var actual = Hashing.FileSha256(path);
        if (!actual.Equals(expected, StringComparison.OrdinalIgnoreCase))
        {
            throw new LauncherException(
                $"{displayName} has a different hash than expected. The operation was stopped.");
        }
    }

    private static string AccessRecoveryHint(Exception exception)
    {
        const int accessDeniedHResult =
            unchecked((int)0x80070005);
        for (Exception? current = exception;
             current is not null;
             current = current.InnerException)
        {
            if (current is UnauthorizedAccessException ||
                current.HResult == accessDeniedHResult)
            {
                return " The game directory does not grant write permission. Close RDR2 and " +
                       "the launcher, run the launcher as administrator only for install/uninstall, " +
                       "and run it normally for regular testing.";
            }
        }

        return string.Empty;
    }

    private static void CopyNewDurable(string source, string destination)
    {
        using var input = new FileStream(
            source,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            128 * 1024,
            FileOptions.SequentialScan);
        using var output = new FileStream(
            destination,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.None,
            128 * 1024,
            FileOptions.WriteThrough);
        input.CopyTo(output);
        output.Flush(flushToDisk: true);
    }

    private static void WriteNewDurable(string destination, byte[] bytes)
    {
        using var output = new FileStream(
            destination,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.None,
            16 * 1024,
            FileOptions.WriteThrough);
        output.Write(bytes);
        output.Flush(flushToDisk: true);
    }

    private static bool DeleteOnlyIfHashMatches(string path, string expectedHash)
    {
        if (!File.Exists(path))
        {
            return true;
        }

        try
        {
            if (!Hashing.FileSha256(path).Equals(
                    expectedHash,
                    StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }

            File.Delete(path);
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

    private static void DeleteOnlyIfHashMatchesOrMissing(
        string path,
        string expectedHash,
        string displayName)
    {
        if (!File.Exists(path))
        {
            return;
        }

        EnsureHash(path, expectedHash, displayName);
        File.Delete(path);
    }

    private sealed record InstallPlan(
        string GameRoot,
        string GameHash,
        RuntimeLayout Runtime,
        IReadOnlyList<PlannedFile> Files,
        string PackageHash);

    private sealed record PlannedFile(
        string RelativePath,
        string? SourcePath,
        byte[]? Bytes,
        string Sha256,
        long Length,
        bool Owned)
    {
        public static PlannedFile FromSource(
            string relativePath,
            string sourcePath,
            bool owned)
        {
            var info = new FileInfo(sourcePath);
            return new PlannedFile(
                relativePath,
                sourcePath,
                null,
                Hashing.FileSha256(sourcePath),
                info.Length,
                owned);
        }

        public static PlannedFile FromBytes(string relativePath, byte[] bytes) =>
            new(
                relativePath,
                null,
                bytes,
                Hashing.BytesSha256(bytes),
                bytes.LongLength,
                true);
    }
}
