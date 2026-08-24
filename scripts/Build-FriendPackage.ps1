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
$readmeSource = Join-Path $workspace 'packaging\PRZECZYTAJ_MNIE.txt'
$v29TestSource = Join-Path $workspace 'docs\TEST_V29_ANIMSCENE_METAPED.md'
$v30TestSource = Join-Path $workspace 'docs\TEST_V30_ANIMGRAPH.md'
$v31TestSource = Join-Path $workspace 'docs\TEST_V31_ANIMSCENE_HYBRID.md'
$batSource = Join-Path $workspace 'packaging\URUCHOM_COOP.bat'

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
            throw 'Bridge ASI jest zbyt maly.'
        }
        $reader = New-Object IO.BinaryReader($stream)
        try {
            if ($reader.ReadUInt16() -ne 0x5A4D) {
                throw 'Bridge ASI nie ma sygnatury MZ.'
            }
            $stream.Position = 0x3C
            $peOffset = $reader.ReadInt32()
            if ($peOffset -lt 0x40 -or ($peOffset + 26) -gt $stream.Length) {
                throw 'Bridge ASI ma nieprawidlowy naglowek PE.'
            }
            $stream.Position = $peOffset
            if ($reader.ReadUInt32() -ne 0x00004550 -or
                $reader.ReadUInt16() -ne 0x8664) {
                throw 'Bridge ASI nie jest obrazem PE x64.'
            }
            $stream.Position = $peOffset + 22
            if (($reader.ReadUInt16() -band 0x2000) -eq 0) {
                throw 'Bridge ASI nie ma charakterystyki DLL.'
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
            'Bridge ASI nie ma jednoznacznego markera aktywnych native bindings. ' +
            'Zbuduj jawny preset bridge-asi-vs2022 lub bridge-asi-vs2026; ' +
            'paczka nie moze zawierac bezpiecznego, lecz nieaktywnego stuba.')
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
            'Bridge ASI jest starszy niz kod lub konfiguracja builda. ' +
            'Wykonaj nowy warning-clean build private-validation.')
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
                    'Paczka zawiera prywatna sciezke builda w pliku: ' +
                    $file.FullName)
            }
        }
    }
}

if (-not (Test-Path -LiteralPath $workspace -PathType Container)) {
    throw 'WorkspaceRoot nie istnieje.'
}
if (-not (Test-PathWithin -Parent $workspace -Child $bridge) -or
    -not (Test-Path -LiteralPath $bridge -PathType Leaf)) {
    throw 'BridgePath musi wskazywac plik wewnatrz workspace.'
}
if (-not ([IO.Path]::GetFileName($bridge)).Equals(
    'CoopStoryBridge.asi',
    [StringComparison]::Ordinal)) {
    throw 'BridgePath musi wskazywac CoopStoryBridge.asi.'
}
Assert-PeX64Dll -Path $bridge
Assert-ActiveBridgeCapability -Path $bridge
Assert-BridgeFreshness -Workspace $workspace -Bridge $bridge

foreach ($required in @(
    $launcherProject,
    $sidecarProject,
    $configSource,
    $readmeSource,
    $v29TestSource,
    $v30TestSource,
    $v31TestSource,
    $batSource
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw ('Brak wymaganego pliku: ' + $required)
    }
}

$dotnet = Get-Command dotnet.exe -ErrorAction SilentlyContinue
if ($null -eq $dotnet) {
    throw 'Nie znaleziono dotnet.exe.'
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
        throw 'Managed build nie powiodl sie.'
    }
}

$releaseRoot = Join-Path $workspace 'artifacts\releases'
if (-not (Test-Path -LiteralPath $releaseRoot -PathType Container)) {
    $null = [IO.Directory]::CreateDirectory($releaseRoot)
}
$releaseRootItem = Get-Item -LiteralPath $releaseRoot -Force
if (($releaseRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'artifacts\releases nie moze byc reparse pointem.'
}

$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
$packageName = 'RDR2-CoopStory-CutsceneMissionOwnershipV31.10-' + $stamp
$packageRoot = Join-Path $releaseRoot $packageName
$zipPath = Join-Path $releaseRoot ($packageName + '.zip')
if ((Test-Path -LiteralPath $packageRoot) -or
    (Test-Path -LiteralPath $zipPath)) {
    throw 'Docelowa paczka juz istnieje; niczego nie nadpisano.'
}
$null = [IO.Directory]::CreateDirectory($packageRoot)

$publishCommon = @(
    '-c', 'Release',
    '--no-restore',
    '--self-contained', 'false',
    '-p:DebugSymbols=false',
    '-p:DebugType=None',
    '-p:ContinuousIntegrationBuild=true',
    $pathMapProperty,
    '--nologo'
)

& $dotnet.Source publish $launcherProject @publishCommon -o $packageRoot
if ($LASTEXITCODE -ne 0) {
    throw 'Publikacja launchera nie powiodla sie.'
}

$sidecarRoot = Join-Path $packageRoot 'sidecar'
$null = [IO.Directory]::CreateDirectory($sidecarRoot)
& $dotnet.Source publish $sidecarProject @publishCommon -o $sidecarRoot
if ($LASTEXITCODE -ne 0) {
    throw 'Publikacja sidecara nie powiodla sie.'
}

# Project-reference PDB files can be copied from an earlier diagnostic build
# even when the current publish explicitly disables debug symbols. They are
# not runtime dependencies and must never make the deterministic friend-package
# allowlist depend on the state of bin/obj from a previous developer build.
foreach ($debugArtifact in @(
    Get-ChildItem -LiteralPath $packageRoot -Recurse -Force -File -Filter '*.pdb'
)) {
    if (-not (Test-PathWithin -Parent $packageRoot -Child $debugArtifact.FullName)) {
        throw 'Wykryto artefakt debugowania poza katalogiem staging.'
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
    (Join-Path $packageRoot 'PRZECZYTAJ_MNIE.txt'),
    $false)
[IO.File]::Copy(
    $v29TestSource,
    (Join-Path $packageRoot 'TEST_V29_ANIMSCENE_METAPED.md'),
    $false)
[IO.File]::Copy(
    $v30TestSource,
    (Join-Path $packageRoot 'TEST_V30_ANIMGRAPH.md'),
    $false)
[IO.File]::Copy(
    $v31TestSource,
    (Join-Path $packageRoot 'TEST_V31_ANIMSCENE_HYBRID.md'),
    $false)
[IO.File]::Copy(
    $batSource,
    (Join-Path $packageRoot 'URUCHOM_COOP.bat'),
    $false)

$requiredPackageFiles = @(
    'CoopStory.Launcher.exe',
    'CoopStoryBridge.asi',
    'config\coopstory.example.json',
    'sidecar\CoopStory.Sidecar.exe',
    'PRZECZYTAJ_MNIE.txt',
    'TEST_V29_ANIMSCENE_METAPED.md',
    'TEST_V30_ANIMGRAPH.md',
    'TEST_V31_ANIMSCENE_HYBRID.md',
    'URUCHOM_COOP.bat'
)
foreach ($relativePath in $requiredPackageFiles) {
    if (-not (Test-Path -LiteralPath (
        Join-Path $packageRoot $relativePath) -PathType Leaf)) {
        throw ('Paczka nie zawiera wymaganego pliku: ' + $relativePath)
    }
}

# The publish is framework-dependent and intentionally tiny. An exact allowlist
# prevents a renamed ScriptHook, trainer, loader or SDK archive from slipping
# into the friend package under an otherwise harmless extension.
$allowedPackagePaths = @(
    'CoopStory.Launcher.deps.json',
    'CoopStory.Launcher.dll',
    'CoopStory.Launcher.exe',
    'CoopStory.Launcher.runtimeconfig.json',
    'CoopStory.Protocol.dll',
    'CoopStoryBridge.asi',
    'PRZECZYTAJ_MNIE.txt',
    'TEST_V29_ANIMSCENE_METAPED.md',
    'TEST_V30_ANIMGRAPH.md',
    'TEST_V31_ANIMSCENE_HYBRID.md',
    'URUCHOM_COOP.bat',
    'config\coopstory.example.json',
    'sidecar\CoopStory.Protocol.dll',
    'sidecar\CoopStory.Sidecar.deps.json',
    'sidecar\CoopStory.Sidecar.dll',
    'sidecar\CoopStory.Sidecar.exe',
    'sidecar\CoopStory.Sidecar.runtimeconfig.json',
    'sidecar\sidecar.config.example.json'
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
    throw ('Paczka zawiera plik spoza allowlisty: ' +
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
    throw ('Paczka zawiera zabroniony plik third-party: ' +
        $forbidden[0].FullName)
}

$buildInfo = [ordered]@{
    package = $packageName
    createdUtc = [DateTime]::UtcNow.ToString('o')
    protocol = 20
    engineVersion = '31.10'
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
    exactAnimSceneDialogueAudioReplication = '31.10-host-only-seven-handler-capture-plus-guest-create-bind-all-required-before-load-verify-commit-and-retained-fallback-cast'
    animSceneDefinitionTransport = '20.0-canonical-sha256-128-resource-playback-create-flags-and-up-to-48-stable-ped-horse-object-role-bindings'
    animSceneDefinitionHandshake = '20.0-single-critical-fifo-noninvasive-host-story-scene-preload-cast-binding-and-stage-diagnostics'
    animSceneRuntimeCaptureEnabled = $true
    animSceneNativeCreateEnabled = $true
    animSceneHandlerInspector = '31.10-launcher-opt-in-exact-rva-prologue-validation-host-only-inline-capture-guest-native-create-only-and-full-rollback'
    animSceneStoryVmProbeDefaultEnabled = $false
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
    runtime = 'framework-dependent net10.0-windows x64'
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
    throw 'Nie udalo sie utworzyc ZIP.'
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
        throw ('ZIP zawiera niebezpieczna sciezke: ' + $unsafeZipEntry[0])
    }
    $duplicateZipEntry = @($zipFilePaths |
        Group-Object |
        Where-Object Count -gt 1 |
        Select-Object -First 1)
    if ($duplicateZipEntry.Count -ne 0) {
        throw ('ZIP zawiera powtorzony plik: ' + $duplicateZipEntry[0].Name)
    }
    $zipDifference = @(Compare-Object `
        -ReferenceObject ($expectedZipPaths | Sort-Object) `
        -DifferenceObject ($zipFilePaths | Sort-Object))
    if ($zipDifference.Count -ne 0) {
        throw ('Zawartosc gotowego ZIP-a nie zgadza sie z allowlista: ' +
            $zipDifference[0].InputObject)
    }
}
finally {
    $zipArchive.Dispose()
}

# Dopiero po poprawnym utworzeniu nowej paczki porządkujemy poprzednie wydania.
# Dzięki temu błąd kompilacji albo kompresji nie usuwa ostatniego działającego ZIP-a.
$oldRoot = Join-Path $releaseRoot 'OLD'
if (-not (Test-Path -LiteralPath $oldRoot -PathType Container)) {
    $null = [IO.Directory]::CreateDirectory($oldRoot)
}
$oldRootItem = Get-Item -LiteralPath $oldRoot -Force
if (($oldRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
    -not (Test-PathWithin -Parent $releaseRoot -Child $oldRoot)) {
    throw 'artifacts\releases\OLD nie jest bezpiecznym katalogiem archiwum.'
}

foreach ($previousZip in @(
    Get-ChildItem -LiteralPath $releaseRoot -Force -File -Filter '*.zip' |
        Where-Object { $_.FullName -ne $zipPath }
)) {
    if (-not (Test-PathWithin -Parent $releaseRoot -Child $previousZip.FullName)) {
        throw 'Wykryto ZIP poza katalogiem releases.'
    }
    $archiveTarget = Join-Path $oldRoot $previousZip.Name
    if (Test-Path -LiteralPath $archiveTarget) {
        throw ('Archiwum OLD zawiera już plik: ' + $previousZip.Name)
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
        throw ('Odmowa usunięcia niebezpiecznego katalogu release: ' +
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
        # Windows nie pozwala usunąć DLL launchera, z którego nadal działa
        # poprzednia wersja. Nowy ZIP jest już w tym miejscu kompletny i
        # zweryfikowany, więc pozostawiamy wyłącznie zablokowany katalog do
        # następnego porządkowania po zamknięciu starego procesu.
        Write-Warning (
            'Pominięto używany katalog starego release; zostanie usunięty ' +
            'po zamknięciu launchera przy następnym buildzie: ' +
            $previousDirectory.FullName)
    }
}

$zipSha256 = Get-Sha256 -Path $zipPath
if (-not (Test-PathWithin -Parent $releaseRoot -Child $packageRoot) -or
    (Get-Item -LiteralPath $packageRoot -Force).Attributes -band
        [IO.FileAttributes]::ReparsePoint) {
    throw 'Odmowa usuniecia niebezpiecznego katalogu staging release.'
}
# Użytkownik testuje wyłącznie gotowy ZIP. Po ponownym otwarciu i sprawdzeniu
# allowlisty katalog staging nie jest już potrzebny; releases ma zawierać tylko
# bieżący ZIP oraz archiwum OLD ze starszymi ZIP-ami.
Remove-Item -LiteralPath $packageRoot -Recurse -Force

Write-Output ('PACKAGE_ZIP=' + $zipPath)
Write-Output ('PACKAGE_ZIP_SHA256=' + $zipSha256)
Write-Output 'SCRIPT_HOOK_BUNDLED=false'
