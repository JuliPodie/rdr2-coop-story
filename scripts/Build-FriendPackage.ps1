#Requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BridgePath,

    [string]$WorkspaceRoot,

    [switch]$SkipManagedBuild
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($WorkspaceRoot)) {
    $WorkspaceRoot = Split-Path -Parent $PSScriptRoot
}

$workspace = [IO.Path]::GetFullPath($WorkspaceRoot).TrimEnd('\', '/')
$bridge = [IO.Path]::GetFullPath($BridgePath)
$launcherProject = Join-Path $workspace 'src\CoopStory.Launcher\CoopStory.Launcher.csproj'
$sidecarProject = Join-Path $workspace 'src\CoopStory.Sidecar\CoopStory.Sidecar.csproj'
$configSource = Join-Path $workspace 'src\CoopStory.Sidecar\sidecar.config.example.json'
$readmeSource = Join-Path $workspace 'packaging\README.txt'
$testGuideSource = Join-Path $workspace 'docs\TESTING.md'
$batSource = Join-Path $workspace 'packaging\START_COOP.bat'

function Test-PathWithin {
    param(
        [Parameter(Mandatory = $true)][string]$Parent,
        [Parameter(Mandatory = $true)][string]$Child
    )

    $parentFull = [IO.Path]::GetFullPath($Parent).TrimEnd('\', '/') +
        [IO.Path]::DirectorySeparatorChar
    $childFull = [IO.Path]::GetFullPath($Child)
    return $childFull.StartsWith(
        $parentFull,
        [StringComparison]::OrdinalIgnoreCase)
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::Open(
        $Path,
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    try {
        return ([BitConverter]::ToString(
            $sha.ComputeHash($stream))).Replace('-', '')
    }
    finally {
        $stream.Dispose()
        $sha.Dispose()
    }
}

function Write-Utf8CreateNew {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Content
    )

    $encoding = New-Object Text.UTF8Encoding($false)
    $bytes = $encoding.GetBytes($Content)
    $stream = [IO.File]::Open(
        $Path,
        [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write,
        [IO.FileShare]::None)
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    }
    finally {
        $stream.Dispose()
    }
}

function Assert-PeX64Dll {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = [IO.File]::OpenRead($Path)
    try {
        if ($stream.Length -lt 512) {
            throw 'Bridge ASI is too small.'
        }
        $reader = New-Object IO.BinaryReader($stream)
        try {
            if ($reader.ReadUInt16() -ne 0x5A4D) {
                throw 'Bridge ASI does not have an MZ signature.'
            }
            $stream.Position = 0x3C
            $peOffset = $reader.ReadInt32()
            if ($peOffset -lt 0x40 -or ($peOffset + 26) -gt $stream.Length) {
                throw 'Bridge ASI has an invalid PE header.'
            }
            $stream.Position = $peOffset
            if ($reader.ReadUInt32() -ne 0x00004550 -or
                $reader.ReadUInt16() -ne 0x8664) {
                throw 'Bridge ASI is not an x64 PE image.'
            }
            $stream.Position = $peOffset + 22
            if (($reader.ReadUInt16() -band 0x2000) -eq 0) {
                throw 'Bridge ASI does not have the DLL characteristic.'
            }
        }
        finally {
            $reader.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

function Assert-ActiveBridgeCapability {
    param([Parameter(Mandatory = $true)][string]$Path)

    $enabledMarker = 'COOPSTORY_NATIVE_BINDINGS_ENABLED_V1'
    $disabledMarker = 'COOPSTORY_NATIVE_BINDINGS_DISABLED_V1'
    $bytes = [IO.File]::ReadAllBytes($Path)
    $binaryText = [Text.Encoding]::ASCII.GetString($bytes)
    $hasEnabledMarker = $binaryText.IndexOf(
        $enabledMarker,
        [StringComparison]::Ordinal) -ge 0
    $hasDisabledMarker = $binaryText.IndexOf(
        $disabledMarker,
        [StringComparison]::Ordinal) -ge 0
    if (-not $hasEnabledMarker -or $hasDisabledMarker) {
        throw (
            'Bridge ASI does not contain an unambiguous active-native-bindings marker. ' +
            'Build the explicit bridge-asi-vs2022 or bridge-asi-vs2026 preset; ' +
            'the package cannot contain a safe but inactive stub.')
    }
}

function Assert-BridgeFreshness {
    param(
        [Parameter(Mandatory = $true)][string]$Workspace,
        [Parameter(Mandatory = $true)][string]$Bridge
    )

    $sourceRoot = Join-Path $Workspace 'src\CoopStory.Bridge'
    $inputs = @(
        Get-ChildItem -LiteralPath $sourceRoot -Recurse -Force -File |
            Where-Object {
                @('.cpp', '.c', '.hpp', '.h', '.txt') -contains
                    $_.Extension.ToLowerInvariant()
            }
    )
    foreach ($rootInput in @('CMakeLists.txt', 'CMakePresets.json')) {
        $candidate = Join-Path $Workspace $rootInput
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $inputs += Get-Item -LiteralPath $candidate -Force
        }
    }

    $newest = $inputs |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1
    $bridgeItem = Get-Item -LiteralPath $Bridge -Force
    if ($null -eq $newest -or
        $bridgeItem.LastWriteTimeUtc -lt $newest.LastWriteTimeUtc) {
        throw (
            'Bridge ASI is older than the source or build configuration. ' +
            'Create a new warning-clean private-validation build.')
    }
}

function Assert-NoPrivateBuildPaths {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string[]]$PrivatePaths
    )

    $needles = New-Object 'System.Collections.Generic.List[string]'
    foreach ($privatePath in $PrivatePaths) {
        if ([string]::IsNullOrWhiteSpace($privatePath)) {
            continue
        }
        $needles.Add($privatePath)
        $needles.Add($privatePath.Replace('\', '/'))
    }

    foreach ($file in Get-ChildItem -LiteralPath $Root -Recurse -Force -File) {
        $bytes = [IO.File]::ReadAllBytes($file.FullName)
        $ascii = [Text.Encoding]::ASCII.GetString($bytes)
        $unicode = [Text.Encoding]::Unicode.GetString($bytes)
        foreach ($needle in $needles) {
            if ($ascii.IndexOf(
                    $needle,
                    [StringComparison]::OrdinalIgnoreCase) -ge 0 -or
                $unicode.IndexOf(
                    $needle,
                    [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                throw (
                    'The package contains a private build path in file: ' +
                    $file.FullName)
            }
        }
    }
}

if (-not (Test-Path -LiteralPath $workspace -PathType Container)) {
    throw 'WorkspaceRoot does not exist.'
}
if (-not (Test-PathWithin -Parent $workspace -Child $bridge) -or
    -not (Test-Path -LiteralPath $bridge -PathType Leaf)) {
    throw 'BridgePath must point to a file inside the workspace.'
}
if (-not ([IO.Path]::GetFileName($bridge)).Equals(
    'CoopStoryBridge.asi',
    [StringComparison]::Ordinal)) {
    throw 'BridgePath must point to CoopStoryBridge.asi.'
}
Assert-PeX64Dll -Path $bridge
Assert-ActiveBridgeCapability -Path $bridge
Assert-BridgeFreshness -Workspace $workspace -Bridge $bridge

foreach ($required in @(
    $launcherProject,
    $sidecarProject,
    $configSource,
    $readmeSource,
    $testGuideSource,
    $batSource
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw ('Required file is missing: ' + $required)
    }
}

$dotnet = Get-Command dotnet.exe -ErrorAction SilentlyContinue
if ($null -eq $dotnet) {
    throw 'dotnet.exe was not found.'
}

$pathMapProperty = '-p:PathMap=' + $workspace + '=/_/src'
if (-not $SkipManagedBuild) {
    & $dotnet.Source build (Join-Path $workspace 'CoopStory.slnx') `
        -c Release -t:Rebuild `
        -p:DebugSymbols=false `
        -p:DebugType=None `
        -p:ContinuousIntegrationBuild=true `
        $pathMapProperty `
        --nologo
    if ($LASTEXITCODE -ne 0) {
        throw 'Managed build failed.'
    }
}

$releaseRoot = Join-Path $workspace 'artifacts\releases'
if (-not (Test-Path -LiteralPath $releaseRoot -PathType Container)) {
    $null = [IO.Directory]::CreateDirectory($releaseRoot)
}
$releaseRootItem = Get-Item -LiteralPath $releaseRoot -Force
if (($releaseRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'artifacts\releases cannot be a reparse point.'
}

$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
$packageName = 'RDR2-CoopStory-Tester-Protocol32-' + $stamp
$packageRoot = Join-Path $releaseRoot $packageName
$zipPath = Join-Path $releaseRoot ($packageName + '.zip')
if ((Test-Path -LiteralPath $packageRoot) -or
    (Test-Path -LiteralPath $zipPath)) {
    throw 'The target package already exists; nothing was overwritten.'
}
$null = [IO.Directory]::CreateDirectory($packageRoot)

$publishCommon = @(
    '-c', 'Release',
    '-r', 'win-x64',
    '--self-contained', 'true',
    '-p:DebugSymbols=false',
    '-p:DebugType=None',
    '-p:ContinuousIntegrationBuild=true',
    $pathMapProperty,
    '--nologo'
)

& $dotnet.Source publish $launcherProject @publishCommon -o $packageRoot
if ($LASTEXITCODE -ne 0) {
    throw 'Launcher publication failed.'
}
if (-not (Test-Path -LiteralPath (
    Join-Path $packageRoot 'CoopStory.Launcher.exe') -PathType Leaf)) {
    throw 'Self-contained launcher publication did not produce CoopStory.Launcher.exe.'
}

$sidecarRoot = Join-Path $packageRoot 'sidecar'
$null = [IO.Directory]::CreateDirectory($sidecarRoot)
& $dotnet.Source publish $sidecarProject @publishCommon -o $sidecarRoot
if ($LASTEXITCODE -ne 0) {
    throw 'Sidecar publication failed.'
}

# Project-reference PDB files can be copied from an earlier diagnostic build
# even when the current publish explicitly disables debug symbols. They are
# not runtime dependencies and must never make the deterministic friend-package
# allowlist depend on the state of bin/obj from a previous developer build.
foreach ($debugArtifact in @(
    Get-ChildItem -LiteralPath $packageRoot -Recurse -Force -File -Filter '*.pdb'
)) {
    if (-not (Test-PathWithin -Parent $packageRoot -Child $debugArtifact.FullName)) {
        throw 'A debug artifact was detected outside the staging directory.'
    }
    Remove-Item -LiteralPath $debugArtifact.FullName -Force
}

$configRoot = Join-Path $packageRoot 'config'
$null = [IO.Directory]::CreateDirectory($configRoot)
[IO.File]::Copy(
    $bridge,
    (Join-Path $packageRoot 'CoopStoryBridge.asi'),
    $false)
[IO.File]::Copy(
    $configSource,
    (Join-Path $configRoot 'coopstory.example.json'),
    $false)
[IO.File]::Copy(
    $readmeSource,
    (Join-Path $packageRoot 'README.txt'),
    $false)
[IO.File]::Copy(
    $testGuideSource,
    (Join-Path $packageRoot 'TESTING.md'),
    $false)
[IO.File]::Copy(
    $batSource,
    (Join-Path $packageRoot 'START_COOP.bat'),
    $false)

$requiredPackageFiles = @(
    'CoopStory.Launcher.exe',
    'CoopStoryBridge.asi',
    'config\coopstory.example.json',
    'sidecar\CoopStory.Sidecar.exe',
    'README.txt',
    'TESTING.md',
    'START_COOP.bat'
)
foreach ($relativePath in $requiredPackageFiles) {
    if (-not (Test-Path -LiteralPath (
        Join-Path $packageRoot $relativePath) -PathType Leaf)) {
        throw ('The package does not contain required file: ' + $relativePath)
    }
}

# Capture the exact output from the two known self-contained publishes before
# project documentation and bridge files are added. The archive check below
# uses this list, while the forbidden-file check still rejects third-party
# loaders, SDKs, trainers, source and debug artifacts.
$publishedRuntimePaths = @(Get-ChildItem `
    -LiteralPath $packageRoot `
    -Recurse `
    -Force `
    -File | ForEach-Object {
        $_.FullName.Substring($packageRoot.Length + 1)
    })
if ($publishedRuntimePaths.Count -eq 0) {
    throw 'Self-contained publication produced no runtime files.'
}

$allowedPackagePaths = @(
    ($publishedRuntimePaths + @(
    'CoopStory.Launcher.deps.json',
    'CoopStory.Launcher.dll',
    'CoopStory.Launcher.exe',
    'CoopStory.Launcher.runtimeconfig.json',
    'CoopStory.Protocol.dll',
    'CoopStoryBridge.asi',
    'README.txt',
    'TESTING.md',
    'START_COOP.bat',
    'config\coopstory.example.json',
    'sidecar\CoopStory.Protocol.dll',
    'sidecar\CoopStory.Sidecar.deps.json',
    'sidecar\CoopStory.Sidecar.dll',
    'sidecar\CoopStory.Sidecar.exe',
    'sidecar\CoopStory.Sidecar.runtimeconfig.json',
    'sidecar\sidecar.config.example.json'
    ) | Sort-Object -Unique)
)
$unexpectedPackageFiles = @(Get-ChildItem `
    -LiteralPath $packageRoot `
    -Recurse `
    -Force `
    -File | Where-Object {
        $relativePath = $_.FullName.Substring($packageRoot.Length + 1)
        $allowedPackagePaths -notcontains $relativePath
    })
if ($unexpectedPackageFiles.Count -gt 0) {
    throw ('The package contains a file outside the allowlist: ' +
        $unexpectedPackageFiles[0].FullName)
}

$allFiles = @(Get-ChildItem -LiteralPath $packageRoot -Recurse -Force -File)
$forbiddenNames = @(
    'ScriptHookRDR2.dll',
    'dinput8.dll',
    'NativeTrainer.asi',
    'natives.h',
    'script.h'
)
$forbiddenExtensions = @(
    '.h',
    '.hpp',
    '.c',
    '.cpp',
    '.inc',
    '.inl',
    '.def',
    '.lib',
    '.vcxproj',
    '.sln',
    '.otf',
    '.zip',
    '.7z',
    '.rar',
    '.pdf'
)
$forbidden = @($allFiles | Where-Object {
    $relativePath = $_.FullName.Substring($packageRoot.Length + 1)
    $thirdPartyRuntimeOrSdkPath =
        $relativePath -match '(^|[\\/])(ScriptHookRDR2([_. -]SDK)?|SDK|NativeTrainer)([\\/]|$)'
    $suspiciousThirdPartyName =
        $_.Name -match '(?i)(scripthookrdr2|dinput8|nativetrainer)'
    $forbiddenNames -contains $_.Name -or
    $suspiciousThirdPartyName -or
    $forbiddenExtensions -contains $_.Extension -or
    $thirdPartyRuntimeOrSdkPath -or
    $_.Extension -eq '.ttf'
})
if ($forbidden.Count -gt 0) {
    throw ('The package contains a forbidden third-party file: ' +
        $forbidden[0].FullName)
}

$buildInfo = [ordered]@{
    package = $packageName
    createdUtc = [DateTime]::UtcNow.ToString('o')
    protocol = 32
    engineVersion = 'tester-protocol32'
    ambientEncounterCatalog = '32.0-50-reviewed-script-ids-five-host-owned-profiles-local-vanilla-loot-only'
    entityGraph = '31.9-priority-hysteresis-plus-retained-cinematic-cast-and-optional-released-animscene-object-lane'
    missionSync = '31.10-host-owned-mission-plus-live-resume-anchor-unowned-weather-guard-and-handle-pinned-private-scene-quarantine'
    missionSpectator = '31.0-versioned-animscene-definition-prepare-commit-or-smoothed-proxy-camera-safe-fallback'
    missionSkip = '31.10-two-player-cinematic-generation-scoped-persistent-consensus-plus-handle-pinned-authored-scene-quarantine'
    missionObjective = '29.5-shared-story-objective-panel-plus-yellow-anchor-not-exact-vanilla-mission-vm-text'
    missionReconnect = '31.0-generation-bound-atomic-peer-replay-confirmed-local-guest-reset-parent-first-world-rekeyed-definition-and-safe-fallback-resume-barrier'
    bridgePipeReconnect = '31.0-ready-generation-token-no-cross-pipe-retarget-and-full-frame-reset-before-host-replay-request'
    udpPeerBinding = '19.0-hmac-authenticated-sender-instance-id-plus-control-sequence-floor-before-endpoint-pinning'
    missionDamage = 'host-validated-script-owned-combat-target-with-guest-proxy-attribution'
    playerActionSync = '31.5-generation-bound-host-authority-causal-playerstate-pipe-proof-plus-context-isolated-explicit-mpul-dismount-variant'
    interactionAuthority = '31.9-peer-generation-bound-host-validation-plus-all-context-variant-talk-and-own-mount-melee-isolation'
    restraintState = '31.0-targetless-terminal-release-after-mapping-expiry-sustain-heartbeat-replayable-free-tombstone-and-deferred-spawn-apply'
    downedRevive = '31.4-physical-engine-observation-without-policy-feedback-latch-plus-stable-respawn-recovery'
    puppetNavigation = '13.1-sustained-error-marker-lock'
    ghostRecord = '13.1-marker-lock-traversal-replay'
    motionReplication = '30.0-rdr-native-heading-direction-aware-direct-root-state-sync'
    animGraphReader = 'native-locomotion-probe-movenetwork-fail-closed'
    animGraphMemoryReaderEnabled = $false
    animGraphVisualDriver = '30.0-eight-way-direction-turn-in-place-native-gait-ik-and-350ms-stall-watchdog'
    animGraphDirectionalLocomotion = '30.0-rdr-heading-zero-plus-y-forward-right-axis-projection-and-direction-refresh'
    animGraphWater = '30.0-in-water-swimming-underwater-semantics-plus-native-graph-observation'
    animGraphTraversalDriver = 'reliable-jump-climb-and-airborne-velocity-seed'
    physicalRootLeashMeters = 1.5
    stealthReplication = '30.2-rdr2-crouch-semantic-transition-plus-250ms-observation-watchdog-recovery'
    coverReplication = '30.2-native-acquire-crouched-fallback-state-ownership-and-bounded-reacquire-watchdogs'
    meleeReplication = '26.0-reliable-action-fsm-plus-persistent-semantic-ragdoll-and-victim-owned-knockdown-window'
    lassoReplication = '31.2-sender-confirmed-physical-task-no-receiver-begin-autothrow-and-authoritative-victim-only-restraint-fallback'
    remoteEquipment = '29.5-periodic-held-weapon-visual-reassertion-plus-lasso-utility-validation-bypass'
    exactMetaPedOutfitReplication = '29.0-ordered-complete-story-shop-component-set-model-gated-and-fingerprinted'
    exactAnimSceneDialogueAudioReplication = '30.0-experimental-host-cue-ready-with-companion-only-local-presentation-and-vanilla-fallback'
    animSceneDefinitionTransport = '20.0-canonical-sha256-128-resource-playback-create-flags-and-up-to-48-stable-ped-horse-object-role-bindings'
    animSceneDefinitionHandshake = '20.0-single-critical-fifo-noninvasive-host-story-scene-preload-cast-binding-and-stage-diagnostics'
    animSceneRuntimeCaptureEnabled = $true
    animSceneNativeCreateEnabled = $true
    animSceneHandlerInspector = '31.10-launcher-opt-in-exact-rva-prologue-validation-host-only-inline-capture-guest-native-create-only-and-full-rollback'
    animSceneStoryVmProbeDefaultEnabled = $true
    launcherSessionFlow = '29.6-launcher-owned-4-to-64-character-password-host-join-pbkdf2-colored-role-lobby-and-ping'
    launcherHostSave = '29.4-local-host-save-selector-moved-to-settings'
    inGameMenuUx = '30.1-f8-closed-two-column-f9-and-cross-peer-f7-transient-user-marker'
    diagnosticMarkers = '30.1-shared-correlation-command-immediate-sidecar-snapshot-15s-bridge-burst-and-10s-before-15s-after-export-window'
    projectilePresentation = '30.2-zero-damage-host-authoritative-shot-plus-empty-weapon-native-fire-graph-pulse-and-ammo-restore-lease'
    remoteMount = '31.5-fifty-millisecond-presentation-explicit-rider-id-mapping-local-player-exclusion-and-tagged-dismount-only'
    peerCrimeIsolation = 'short-lived-npc-ignore-and-clean-wanted-preservation'
    synchronizedPause = 'native-frontend-without-hard-freeze-fallback'
    gracefulSessionStop = '27.0-reusable-session-cycle-with-cinematic-vote-latch-camera-action-rope-and-world-cleanup'
    sessionLifecycle = 'host-join-stop-rehost-without-game-restart'
    guestPopulationMask = '29.6-invisible-host-pool-admission-filter-plus-sticky-population-and-recursive-attachment-mask'
    diagnosticsIndex = '31.4-last-two-runtime-sessions-shared-cutoff-for-raw-logs-index-timeline-anomalies-and-markers'
    diagnosticsTimeline = '30.1-timeline-anomaly-and-marker-window-json-plus-markdown-with-10s-before-and-15s-after-context'
    diagnosticsTransport = 'per-direction-per-message-observed-delivered-dropped-coalesced-gap-percentiles-and-recovery'
    diagnosticsRuntime = '30.1-one-hertz-baseline-plus-500ms-for-15s-after-marker-session-mission-player-entity-combat-lasso-cover-mount-metaped-and-animscene'
    diagnosticsCorrelation = '30.1-session-fingerprint-shared-cross-peer-marker-id-and-bounded-time-windows-with-action-and-entity-identifiers'
    diagnosticLogRetention = 'disk-rotation-current-plus-three-archived-but-export-consolidates-only-two-newest-runtime-sessions'
    nativeCrashGuard = '31.10-tick-stage-seh-isolation-bounded-replica-respawn-host-only-register-preserving-detour-and-zero-guest-story-vm-patching'
    supportedGameSha256 =
        'B56C9548F670654A9B73BF25DEF3CD73AF12E269F6E47DBA28A34079ADAF465E'
    bridgeSha256 = Get-Sha256 -Path (
        Join-Path $packageRoot 'CoopStoryBridge.asi')
    bridgeNativeBindings = $true
    runtime = 'self-contained net10.0-windows win-x64'
    scriptHookBundled = $false
    trainerBundled = $false
}
Write-Utf8CreateNew `
    -Path (Join-Path $packageRoot 'BUILD_INFO.json') `
    -Content (($buildInfo | ConvertTo-Json -Depth 4) + "`r`n")

Assert-NoPrivateBuildPaths `
    -Root $packageRoot `
    -PrivatePaths @(
        $workspace,
        [Environment]::GetFolderPath(
            [Environment+SpecialFolder]::UserProfile)
    )

$allFiles = @(Get-ChildItem -LiteralPath $packageRoot -Recurse -Force -File)
$hashLines = New-Object 'System.Collections.Generic.List[string]'
foreach ($file in ($allFiles | Sort-Object FullName)) {
    $relative = $file.FullName.Substring($packageRoot.Length + 1)
    $hashLines.Add((Get-Sha256 -Path $file.FullName) + '  ' + $relative)
}
Write-Utf8CreateNew `
    -Path (Join-Path $packageRoot 'SHA256SUMS.txt') `
    -Content (($hashLines -join "`r`n") + "`r`n")

Compress-Archive -Path (
    Join-Path $packageRoot '*') -DestinationPath $zipPath -CompressionLevel Optimal
if (-not (Test-Path -LiteralPath $zipPath -PathType Leaf)) {
    throw 'Failed to create the ZIP archive.'
}

# Re-open the actual archive rather than trusting only the staging directory.
# This catches duplicate, traversing or unexpected entries after compression.
$expectedZipPaths = @(
    $allowedPackagePaths + @('BUILD_INFO.json', 'SHA256SUMS.txt') |
        ForEach-Object { $_.Replace('\', '/') }
)
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipArchive = [IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    $zipFilePaths = @($zipArchive.Entries |
        Where-Object { -not $_.FullName.EndsWith('/') } |
        ForEach-Object { $_.FullName.Replace('\', '/') })
    $unsafeZipEntry = @($zipFilePaths | Where-Object {
        $_.StartsWith('/') -or
        $_.Contains('../') -or
        $_.Contains(':')
    } | Select-Object -First 1)
    if ($unsafeZipEntry.Count -ne 0) {
        throw ('The ZIP contains an unsafe path: ' + $unsafeZipEntry[0])
    }
    $duplicateZipEntry = @($zipFilePaths |
        Group-Object |
        Where-Object Count -gt 1 |
        Select-Object -First 1)
    if ($duplicateZipEntry.Count -ne 0) {
        throw ('The ZIP contains a duplicate file: ' + $duplicateZipEntry[0].Name)
    }
    $zipDifference = @(Compare-Object `
        -ReferenceObject ($expectedZipPaths | Sort-Object) `
        -DifferenceObject ($zipFilePaths | Sort-Object))
    if ($zipDifference.Count -ne 0) {
        throw ('The completed ZIP does not match the allowlist: ' +
            $zipDifference[0].InputObject)
    }
}
finally {
    $zipArchive.Dispose()
}

# Clean previous releases only after the new package has been created successfully.
# This ensures that a build or compression failure cannot remove the last working ZIP.
$oldRoot = Join-Path $releaseRoot 'OLD'
if (-not (Test-Path -LiteralPath $oldRoot -PathType Container)) {
    $null = [IO.Directory]::CreateDirectory($oldRoot)
}
$oldRootItem = Get-Item -LiteralPath $oldRoot -Force
if (($oldRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
    -not (Test-PathWithin -Parent $releaseRoot -Child $oldRoot)) {
    throw 'artifacts\releases\OLD is not a safe archive directory.'
}

foreach ($previousZip in @(
    Get-ChildItem -LiteralPath $releaseRoot -Force -File -Filter '*.zip' |
        Where-Object { $_.FullName -ne $zipPath }
)) {
    if (-not (Test-PathWithin -Parent $releaseRoot -Child $previousZip.FullName)) {
        throw 'A ZIP was detected outside the releases directory.'
    }
    $archiveTarget = Join-Path $oldRoot $previousZip.Name
    if (Test-Path -LiteralPath $archiveTarget) {
        throw ('The OLD archive already contains file: ' + $previousZip.Name)
    }
    Move-Item -LiteralPath $previousZip.FullName -Destination $archiveTarget
}

foreach ($previousDirectory in @(
    Get-ChildItem -LiteralPath $releaseRoot -Force -Directory |
        Where-Object {
            $_.FullName -ne $packageRoot -and
            $_.FullName -ne $oldRoot
        }
)) {
    if (-not (Test-PathWithin -Parent $releaseRoot -Child $previousDirectory.FullName) -or
        ($previousDirectory.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw ('Refusing to remove an unsafe release directory: ' +
            $previousDirectory.FullName)
    }
    try {
        Remove-Item -LiteralPath $previousDirectory.FullName -Recurse -Force
    }
    catch {
        $accessDenied =
            $_.Exception -is [UnauthorizedAccessException] -or
            $_.Exception.InnerException -is [UnauthorizedAccessException]
        $fileLocked =
            $_.Exception -is [IO.IOException] -or
            $_.Exception.InnerException -is [IO.IOException]
        if (-not $accessDenied -and -not $fileLocked) {
            throw
        }
        # Windows cannot remove a launcher DLL while the previous version is
        # still running from it. The new ZIP is already complete and verified,
        # so leave only the locked directory for the next cleanup after the old
        # process has closed.
        Write-Warning (
            'Skipped an in-use old release directory; it will be removed ' +
            'during the next build after the launcher closes: ' +
            $previousDirectory.FullName)
    }
}

$zipSha256 = Get-Sha256 -Path $zipPath
if (-not (Test-PathWithin -Parent $releaseRoot -Child $packageRoot) -or
    (Get-Item -LiteralPath $packageRoot -Force).Attributes -band
        [IO.FileAttributes]::ReparsePoint) {
    throw 'Refusing to remove an unsafe release staging directory.'
}
# The tester uses only the completed ZIP. After reopening and checking the
# allowlist, the staging directory is no longer needed; releases should contain
# only the current ZIP and the OLD archive of earlier ZIP files.
Remove-Item -LiteralPath $packageRoot -Recurse -Force

Write-Output ('PACKAGE_ZIP=' + $zipPath)
Write-Output ('PACKAGE_ZIP_SHA256=' + $zipSha256)
Write-Output 'SCRIPT_HOOK_BUNDLED=false'
