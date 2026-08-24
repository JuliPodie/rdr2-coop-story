#Requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Install', 'Launch', 'Uninstall')]
    [string]$Action,

    [string]$GamePath,

    [string]$GameExe = (Join-Path ${env:ProgramFiles(x86)} `
        'Steam\steamapps\common\Red Dead Redemption 2\RDR2.exe'),

    [switch]$Preview,

    [switch]$Yes
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$workspace = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\', '/')
if (-not [string]::IsNullOrWhiteSpace($GamePath)) {
    $gameExeFull = [IO.Path]::GetFullPath((Join-Path $GamePath 'RDR2.exe'))
}
else {
    $gameExeFull = [IO.Path]::GetFullPath($GameExe)
}
$gameRoot = Split-Path -Parent $gameExeFull
$expectedGameHash = 'B56C9548F670654A9B73BF25DEF3CD73AF12E269F6E47DBA28A34079ADAF465E'
$runtimeManifestPath = Join-Path $workspace 'artifacts\deploy\runtime-deployment-manifest.json'
$projectManifestPath = Join-Path $workspace 'artifacts\deploy\deployment-manifest.json'
$windowsPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'

$runtimeFiles = @(
    [pscustomobject]@{
        RelativePath = 'ScriptHookRDR2.dll'
        SourceRelativePath = 'bin\ScriptHookRDR2.dll'
        Sha256 = '3AC29FBE8C92B664E358F7D4F0AF2EC9F1CA674885975087EF76BD98BF972A4C'
    },
    [pscustomobject]@{
        RelativePath = 'dinput8.dll'
        SourceRelativePath = 'bin\dinput8.dll'
        Sha256 = '956FB3765572D00F6C08BCAE11E9856A00A68107464A87B6CCC6C1FFED46B88A'
    }
)

function Write-Step {
    param([Parameter(Mandatory = $true)][string]$Text)
    Write-Host ''
    Write-Host ('=== ' + $Text + ' ===') -ForegroundColor Cyan
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::Open(
        $Path,
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::Read
    )
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '')
    }
    finally {
        $stream.Dispose()
        $sha.Dispose()
    }
}

function Get-StringSha256 {
    param([Parameter(Mandatory = $true)][string]$Value)

    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes($Value)
        return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '')
    }
    finally {
        $sha.Dispose()
    }
}

function Copy-DurableExclusive {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    $inputStream = [IO.File]::Open(
        $Source,
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::Read
    )
    try {
        $outputStream = [IO.File]::Open(
            $Destination,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::None
        )
        try {
            $inputStream.CopyTo($outputStream)
            $outputStream.Flush($true)
        }
        finally {
            $outputStream.Dispose()
        }
    }
    finally {
        $inputStream.Dispose()
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
        [IO.FileShare]::None
    )
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    }
    finally {
        $stream.Dispose()
    }
}

function Assert-GameReady {
    param([switch]$RequireSupportedHash)

    if (-not (Test-Path -LiteralPath $gameExeFull -PathType Leaf)) {
        throw ('RDR2.exe was not found: ' + $gameExeFull)
    }
    if (-not ([IO.Path]::GetFileName($gameExeFull)).Equals(
        'RDR2.exe',
        [StringComparison]::OrdinalIgnoreCase)) {
        throw 'GameExe must point to an RDR2.exe file.'
    }
    if (@(Get-Process -Name RDR2 -ErrorAction SilentlyContinue).Count -gt 0) {
        throw 'RDR2 is running. Close the game and run this file again.'
    }
    if ($RequireSupportedHash) {
        $actualHash = Get-FileSha256 -Path $gameExeFull
        if ($actualHash -ne $expectedGameHash) {
            throw 'This RDR2.exe version is unsupported. Installation was stopped.'
        }
    }
}

function Assert-SidecarClosed {
    if (@(Get-Process -Name 'CoopStory.Sidecar' -ErrorAction SilentlyContinue).Count -gt 0) {
        throw 'The sidecar is running. Close its window and try again.'
    }
}

function Get-StaleInstallerTemps {
    $ownedNamePattern =
        '^\.coopstory-(?:[0-9a-f]{32}-(?:bridge|config)|runtime-[0-9a-f]{32})\.tmp$'
    $result = New-Object 'System.Collections.Generic.List[object]'
    foreach ($item in (Get-ChildItem -LiteralPath $gameRoot -Force -File)) {
        if ($item.Name -notmatch $ownedNamePattern) {
            continue
        }
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw ('Old staging is a reparse point; refusing to remove: ' +
                $item.Name)
        }
        if ((Split-Path -Parent $item.FullName) -ne $gameRoot) {
            throw 'Staging was detected outside the game directory.'
        }
        $result.Add($item)
    }
    return @($result | ForEach-Object { $_ })
}

function Remove-StaleInstallerTemps {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [object[]]$Files
    )

    $ownedNamePattern =
        '^\.coopstory-(?:[0-9a-f]{32}-(?:bridge|config)|runtime-[0-9a-f]{32})\.tmp$'
    foreach ($file in $Files) {
        if ($file.Name -notmatch $ownedNamePattern -or
            (Split-Path -Parent $file.FullName) -ne $gameRoot -or
            ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw 'The old staging list changed; refusing removal.'
        }
        if (Test-Path -LiteralPath $file.FullName -PathType Leaf) {
            Remove-Item -LiteralPath $file.FullName -Force
            Write-Host ('Removed inactive staging left by an interrupted operation: ' +
                $file.Name)
        }
    }
}

function Get-UnsafeGameResidue {
    $result = New-Object 'System.Collections.Generic.List[string]'
    foreach ($name in @(
        'CoopStory.config.json',
        'dinput8.dll',
        'ScriptHookRDR2.dll'
    )) {
        if (Test-Path -LiteralPath (Join-Path $gameRoot $name) -PathType Leaf) {
            $result.Add($name)
        }
    }
    foreach ($asi in (
        Get-ChildItem -LiteralPath $gameRoot -Force -File -Filter '*.asi')) {
        $result.Add($asi.Name)
    }
    return @(($result | Sort-Object -Unique) | ForEach-Object { $_ })
}

function Assert-DistReady {
    $required = @(
        (Join-Path $workspace 'dist\CoopStoryBridge.asi'),
        (Join-Path $workspace 'dist\config\coopstory.example.json'),
        (Join-Path $workspace 'dist\sidecar\CoopStory.Sidecar.exe')
    )
    foreach ($path in $required) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw ('A required built package file is missing: ' + $path)
        }
    }

    $forbidden = @('ScriptHookRDR2.dll', 'dinput8.dll', 'NativeTrainer.asi')
    $forbiddenInDist = @(Get-ChildItem -LiteralPath (Join-Path $workspace 'dist') -Recurse -Force |
        Where-Object { (-not $_.PSIsContainer) -and ($forbidden -contains $_.Name) })
    if ($forbiddenInDist.Count -gt 0) {
        throw 'The dist directory contains a runtime or trainer. Installation was stopped.'
    }

    $sidecar = Join-Path $workspace 'dist\sidecar\CoopStory.Sidecar.exe'
    $helpOutput = @(& $sidecar --help 2>&1)
    if ($LASTEXITCODE -ne 0 -or
        (($helpOutput -join "`n") -notmatch 'local-test')) {
        throw 'The sidecar does not start with the local .NET 10 runtime or does not support local-test.'
    }
}

function Assert-NoConflictingMods {
    $asiFiles = @(Get-ChildItem -LiteralPath $gameRoot -File -Filter '*.asi' -Force |
        Where-Object {
            -not $_.Name.Equals(
                'CoopStoryBridge.asi',
                [StringComparison]::OrdinalIgnoreCase)
        })
    if ($asiFiles.Count -gt 0) {
        throw ('Another ASI mod is present in the game directory: ' +
            (($asiFiles | ForEach-Object { $_.Name }) -join ', ') +
            '. Remove it before a controlled test.')
    }

    $knownConflicts = @(
        'NativeTrainer.asi',
        'Rampage.asi',
        'RampageFiles',
        'lml',
        'RedM',
        'vfs.asi',
        'ModManager.Core.dll'
    )
    $present = New-Object 'System.Collections.Generic.List[string]'
    foreach ($name in $knownConflicts) {
        if (Test-Path -LiteralPath (Join-Path $gameRoot $name)) {
            $present.Add($name)
        }
    }
    if ($present.Count -gt 0) {
        throw ('A conflicting mod/loader was detected: ' + ($present -join ', ') +
            '. The installer will not remove anything automatically.')
    }
}

function Resolve-RuntimeRoot {
    $exact = Join-Path $workspace 'ScriptHookRDR2_1.0.1491.17'
    if (Test-Path -LiteralPath $exact -PathType Container) {
        return $exact
    }
    throw 'The local extracted ScriptHookRDR2_1.0.1491.17 runtime is missing.'
}

function Invoke-PrerequisiteVerification {
    param([Parameter(Mandatory = $true)][string]$RuntimeRoot)

    if (-not (Test-Path -LiteralPath $windowsPowerShell -PathType Leaf)) {
        throw 'Windows PowerShell 5.1 was not found.'
    }
    $verifyScript = Join-Path $workspace 'scripts\Verify-Prerequisites.ps1'
    $sdkRoot = Join-Path $workspace 'ScriptHookRDR2_SDK_1.0.1207.73'
    $commandArguments = @(
        '-NoLogo',
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', $verifyScript,
        '-WorkspaceRoot', $workspace,
        '-GamePath', $gameRoot,
        '-SdkPath', $sdkRoot,
        '-RuntimePath', $RuntimeRoot
    )
    & $windowsPowerShell @commandArguments
    if ($LASTEXITCODE -ne 0) {
        throw 'Prerequisite verification failed. Review the FAIL entries above.'
    }
}

function Read-RuntimeReceipt {
    $result = @{}
    if (-not (Test-Path -LiteralPath $runtimeManifestPath -PathType Leaf)) {
        return $result
    }

    try {
        $manifest = Get-Content -LiteralPath $runtimeManifestPath -Raw |
            ConvertFrom-Json
    }
    catch {
        throw 'The runtime manifest is corrupted. The installer refuses to guess.'
    }
    if ($manifest.SchemaVersion -ne 1) {
        throw 'The runtime manifest has an unsupported version.'
    }
    $fingerprint = Get-StringSha256 -Value ($gameRoot.ToLowerInvariant())
    if ($manifest.GameRootFingerprint -ne $fingerprint) {
        throw 'The runtime manifest was created for a different game directory.'
    }

    $allowed = @{}
    foreach ($spec in $runtimeFiles) {
        $allowed[$spec.RelativePath.ToLowerInvariant()] = $spec
    }
    foreach ($entry in @($manifest.Files)) {
        $relative = [string]$entry.RelativePath
        $key = $relative.ToLowerInvariant()
        if (-not $allowed.ContainsKey($key) -or $result.ContainsKey($key)) {
            throw 'The runtime manifest contains an unsupported or duplicate file.'
        }
        if ([string]$entry.Sha256 -ne $allowed[$key].Sha256) {
            throw 'The runtime manifest contains an unexpected hash.'
        }
        if (-not ($entry.OwnedByEasyInstaller -is [bool])) {
            throw 'The runtime manifest does not contain valid ownership information.'
        }
        $result[$key] = [pscustomobject]@{
            RelativePath = $relative
            Sha256 = [string]$entry.Sha256
            Length = [int64]$entry.Length
            OwnedByEasyInstaller = [bool]$entry.OwnedByEasyInstaller
        }
    }
    return $result
}

function Get-RuntimePlan {
    param([Parameter(Mandatory = $true)][string]$RuntimeRoot)

    $receipt = Read-RuntimeReceipt
    $plans = New-Object 'System.Collections.Generic.List[object]'
    foreach ($spec in $runtimeFiles) {
        $source = Join-Path $RuntimeRoot $spec.SourceRelativePath
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw ('The runtime package does not contain: ' + $spec.SourceRelativePath)
        }
        $sourceHash = Get-FileSha256 -Path $source
        if ($sourceHash -ne $spec.Sha256) {
            throw ('The local runtime has an unexpected hash: ' + $spec.RelativePath)
        }

        $target = Join-Path $gameRoot $spec.RelativePath
        $targetPresent = Test-Path -LiteralPath $target -PathType Leaf
        if ($targetPresent -and (Get-FileSha256 -Path $target) -ne $spec.Sha256) {
            throw ('File ' + $spec.RelativePath +
                ' already exists but has a different hash. The installer will not overwrite it.')
        }
        $key = $spec.RelativePath.ToLowerInvariant()
        $previouslyOwned = $receipt.ContainsKey($key) -and
            $receipt[$key].OwnedByEasyInstaller
        $plans.Add([pscustomobject]@{
            RelativePath = $spec.RelativePath
            SourcePath = $source
            TargetPath = $target
            Sha256 = $spec.Sha256
            Length = [int64](Get-Item -LiteralPath $source).Length
            NeedsCopy = (-not $targetPresent)
            OwnedAfterInstall = ((-not $targetPresent) -or $previouslyOwned)
        })
    }
    return @($plans | ForEach-Object { $_ })
}

function Write-RuntimeReceipt {
    param([Parameter(Mandatory = $true)][hashtable]$Entries)

    $directory = Split-Path -Parent $runtimeManifestPath
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
        $null = New-Item -ItemType Directory -Path $directory
    }
    $files = @($Entries.Values | Sort-Object RelativePath | ForEach-Object {
        [ordered]@{
            RelativePath = $_.RelativePath
            Sha256 = $_.Sha256
            Length = [int64]$_.Length
            OwnedByEasyInstaller = [bool]$_.OwnedByEasyInstaller
        }
    })
    $manifest = [ordered]@{
        SchemaVersion = 1
        UpdatedAtUtc = [DateTime]::UtcNow.ToString('o')
        GameRootFingerprint = (Get-StringSha256 -Value ($gameRoot.ToLowerInvariant()))
        GameExecutableSha256 = $expectedGameHash
        SourcePackage = 'ScriptHookRDR2_1.0.1491.17'
        NativeTrainerInstalled = $false
        Files = $files
    }
    $temporary = Join-Path $directory (
        '.runtime-manifest-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    $replacementBackup = Join-Path $directory (
        '.runtime-manifest-backup-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    Write-Utf8CreateNew -Path $temporary -Content (
        $manifest | ConvertTo-Json -Depth 6)
    try {
        if (Test-Path -LiteralPath $runtimeManifestPath -PathType Leaf) {
            [IO.File]::Replace(
                $temporary,
                $runtimeManifestPath,
                $replacementBackup,
                $true)
            if (Test-Path -LiteralPath $replacementBackup -PathType Leaf) {
                Remove-Item -LiteralPath $replacementBackup -Force
            }
        }
        else {
            [IO.File]::Move($temporary, $runtimeManifestPath)
        }
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
        if (Test-Path -LiteralPath $replacementBackup) {
            Remove-Item -LiteralPath $replacementBackup -Force
        }
    }
}

function Install-Runtime {
    param([Parameter(Mandatory = $true)][object[]]$Plans)

    $receiptPreviouslyExisted =
        Test-Path -LiteralPath $runtimeManifestPath -PathType Leaf
    $originalEntries = Read-RuntimeReceipt
    $journalEntries = @{}
    foreach ($plan in $Plans) {
        $key = $plan.RelativePath.ToLowerInvariant()
        $journalEntries[$key] = [pscustomobject]@{
            RelativePath = $plan.RelativePath
            Sha256 = $plan.Sha256
            Length = $plan.Length
            OwnedByEasyInstaller = [bool]$plan.OwnedAfterInstall
        }
    }

    # Persist the complete ownership plan before the first final Move. After an
    # abrupt stop, a copied hash-matching DLL is still tracked by the receipt.
    Write-RuntimeReceipt -Entries $journalEntries
    $journalHash = Get-FileSha256 -Path $runtimeManifestPath
    $committedPlans = New-Object 'System.Collections.Generic.List[object]'
    try {
        foreach ($plan in $Plans) {
            if (-not $plan.NeedsCopy) {
                if ($plan.OwnedAfterInstall) {
                    Write-Host ('Runtime already installed by this installer: ' +
                        $plan.RelativePath)
                }
                else {
                    Write-Host ('An identical runtime was already present and remains third-party-owned: ' +
                        $plan.RelativePath)
                }
                continue
            }

            $tag = [Guid]::NewGuid().ToString('N')
            $staging = Join-Path $gameRoot (
                '.coopstory-runtime-' + $tag + '.tmp')
            try {
                Copy-DurableExclusive -Source $plan.SourcePath -Destination $staging
                if ((Get-FileSha256 -Path $staging) -ne $plan.Sha256) {
                    throw ('Staging verification failed: ' +
                        $plan.RelativePath)
                }
                if (Test-Path -LiteralPath $plan.TargetPath) {
                    throw ('The target appeared during staging: ' +
                        $plan.RelativePath)
                }
                [IO.File]::Move($staging, $plan.TargetPath)
                $committedPlans.Add($plan)
                if ((Get-FileSha256 -Path $plan.TargetPath) -ne $plan.Sha256) {
                    throw ('Final runtime verification failed: ' +
                        $plan.RelativePath)
                }
                Write-Host ('Copied official runtime: ' +
                    $plan.RelativePath) -ForegroundColor Green
            }
            finally {
                if (Test-Path -LiteralPath $staging) {
                    Remove-Item -LiteralPath $staging -Force
                }
            }
        }
    }
    catch {
        $rollbackComplete = $true
        foreach ($committedPlan in $committedPlans) {
            if (Test-Path -LiteralPath $committedPlan.TargetPath -PathType Leaf) {
                if ((Get-FileSha256 -Path $committedPlan.TargetPath) -eq
                    $committedPlan.Sha256) {
                    Remove-Item -LiteralPath $committedPlan.TargetPath -Force
                }
                else {
                    $rollbackComplete = $false
                }
            }
        }
        if ($rollbackComplete -and
            (Test-Path -LiteralPath $runtimeManifestPath -PathType Leaf) -and
            (Get-FileSha256 -Path $runtimeManifestPath) -eq $journalHash) {
            if ($receiptPreviouslyExisted) {
                Write-RuntimeReceipt -Entries $originalEntries
            }
            else {
                Remove-Item -LiteralPath $runtimeManifestPath -Force
            }
        }
        throw
    }
}

function Test-ConfigSafety {
    param([Parameter(Mandatory = $true)][string]$Path)

    try {
        $config = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    }
    catch {
        throw 'CoopStory.config.json is corrupted.'
    }
    if ($config.schemaVersion -ne 1 -or
        $config.safety.storyModeOnly -ne $true -or
        $config.safety.refuseOnlineMode -ne $true) {
        throw 'The configuration is missing mandatory Story Mode/RDO guards.'
    }
}

function Test-ProjectAlreadyInstalled {
    if (-not (Test-Path -LiteralPath $projectManifestPath -PathType Leaf)) {
        return $false
    }
    try {
        $manifest = Get-Content -LiteralPath $projectManifestPath -Raw |
            ConvertFrom-Json
    }
    catch {
        throw 'The mod manifest is corrupted. Use the safe uninstaller.'
    }
    $fingerprint = Get-StringSha256 -Value ($gameRoot.ToLowerInvariant())
    if ($manifest.SchemaVersion -ne 1 -or
        $manifest.GameRootFingerprint -ne $fingerprint) {
        throw 'The mod manifest does not match this game directory.'
    }

    $bridgeTarget = Join-Path $gameRoot 'CoopStoryBridge.asi'
    $configTarget = Join-Path $gameRoot 'CoopStory.config.json'
    if (-not (Test-Path -LiteralPath $bridgeTarget -PathType Leaf) -or
        -not (Test-Path -LiteralPath $configTarget -PathType Leaf)) {
        throw 'The manifest exists, but the mod installation is incomplete.'
    }
    $bridgeEntry = @($manifest.Files | Where-Object {
        $_.RelativePath -eq 'CoopStoryBridge.asi'
    }) | Select-Object -First 1
    if ($null -eq $bridgeEntry) {
        throw 'The mod manifest does not contain the bridge.'
    }
    $installedHash = Get-FileSha256 -Path $bridgeTarget
    $distHash = Get-FileSha256 -Path (
        Join-Path $workspace 'dist\CoopStoryBridge.asi')
    if ($installedHash -ne [string]$bridgeEntry.Sha256 -or
        $installedHash -ne $distHash) {
        throw 'The installed bridge is different. Use the uninstaller first.'
    }
    Test-ConfigSafety -Path $configTarget
    return $true
}

function Confirm-Exact {
    param(
        [Parameter(Mandatory = $true)][string]$Expected,
        [Parameter(Mandatory = $true)][string]$Prompt
    )

    if ($Yes) {
        return
    }
    $answer = Read-Host $Prompt
    if ($answer -cne $Expected) {
        throw 'Cancelled. The game directory was not changed.'
    }
}

function Install-Easy {
    Write-Step 'Checking the game and package'
    Assert-GameReady -RequireSupportedHash
    Assert-SidecarClosed
    Assert-DistReady
    Assert-NoConflictingMods
    $runtimeRoot = Resolve-RuntimeRoot
    Invoke-PrerequisiteVerification -RuntimeRoot $runtimeRoot
    $runtimePlans = @(Get-RuntimePlan -RuntimeRoot $runtimeRoot)
    $staleTemps = @(Get-StaleInstallerTemps)
    $projectInstalled = Test-ProjectAlreadyInstalled
    $runtimeNeedsCopy = @($runtimePlans | Where-Object { $_.NeedsCopy }).Count -gt 0

    if ($projectInstalled -and -not $runtimeNeedsCopy -and
        $staleTemps.Count -eq 0) {
        Write-Host ''
        Write-Host 'The mod and runtime are already installed correctly.' -ForegroundColor Green
        Write-Host 'Run now: 2_RUN_TEST.bat'
        return
    }

    Write-Step 'Safe installation dry run'
    if ($projectInstalled) {
        Write-Host '  project files: already installed; only the runtime will be repaired'
    }
    else {
        & (Join-Path $workspace 'scripts\Install-DevBuild.ps1') `
            -GamePath $gameRoot `
            -WorkspaceRoot $workspace
    }
    foreach ($plan in $runtimePlans) {
        if ($plan.NeedsCopy) {
            Write-Host ('  copy runtime: ' + $plan.RelativePath)
        }
        else {
            Write-Host ('  leave identical runtime: ' + $plan.RelativePath)
        }
    }
    Write-Host '  NativeTrainer.asi: NEVER COPIED'
    foreach ($temp in $staleTemps) {
        Write-Host ('  remove stale non-loadable staging: ' + $temp.Name)
    }

    if ($Preview) {
        Write-Host ''
        Write-Host 'Preview complete. The game directory was not changed.' -ForegroundColor Yellow
        return
    }

    Write-Host ''
    Write-Host 'This is an experimental smoke test, not a finished campaign co-op mod.' `
        -ForegroundColor Yellow
    Confirm-Exact -Expected 'INSTALL' `
        -Prompt 'To continue, type exactly INSTALL'

    Remove-StaleInstallerTemps -Files $staleTemps

    Write-Step 'Save backup and baseline'
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
    & (Join-Path $workspace 'scripts\Backup-Saves.ps1') `
        -DestinationRoot (Join-Path $workspace '_backups\saves-easy-installer') `
        -BackupName ('RDR2-saves-' + $stamp)
    & (Join-Path $workspace 'scripts\Capture-GameBaseline.ps1') `
        -GamePath $gameRoot `
        -OutputPath (Join-Path $workspace (
            'artifacts\baselines\easy-before-install-' + $stamp + '.json'))

    if (-not $projectInstalled) {
        Write-Step 'Installing the inactive bridge and configuration'
        & (Join-Path $workspace 'scripts\Install-DevBuild.ps1') `
            -GamePath $gameRoot `
            -WorkspaceRoot $workspace `
            -Apply
    }

    # The loader is the last commit. If an earlier project step fails, no new
    # executable loader is left in the game directory.
    Write-Step 'Installing the official runtime and loader as the final step'
    Install-Runtime -Plans $runtimePlans

    Write-Host ''
    Write-Host 'INSTALLATION COMPLETE.' -ForegroundColor Green
    Write-Host 'Next, run 2_RUN_TEST.bat and select Story Mode only.'
}

function Get-OwnedRuntimeForRemoval {
    $receipt = Read-RuntimeReceipt
    $owned = New-Object 'System.Collections.Generic.List[object]'
    foreach ($entry in $receipt.Values) {
        if (-not $entry.OwnedByEasyInstaller) {
            continue
        }
        $target = Join-Path $gameRoot $entry.RelativePath
        if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
            continue
        }
        if ((Get-FileSha256 -Path $target) -ne $entry.Sha256) {
            throw ('A runtime owned by this installer was modified: ' + $entry.RelativePath +
                '. The uninstaller refuses to remove it.')
        }
        $owned.Add([pscustomobject]@{
            RelativePath = $entry.RelativePath
            FullPath = $target
            Sha256 = $entry.Sha256
        })
    }
    return @($owned | ForEach-Object { $_ })
}

function Uninstall-Runtime {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [object[]]$OwnedFiles
    )

    if (-not (Test-Path -LiteralPath $runtimeManifestPath -PathType Leaf)) {
        return
    }
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
    $backup = Join-Path $workspace (
        'artifacts\uninstall-backups\runtime-' + $stamp)
    $null = New-Item -ItemType Directory -Path $backup

    foreach ($file in $OwnedFiles) {
        $target = Join-Path $backup $file.RelativePath
        Copy-DurableExclusive -Source $file.FullPath -Destination $target
        if ((Get-FileSha256 -Path $target) -ne $file.Sha256) {
            throw ('Runtime backup verification failed: ' +
                $file.RelativePath)
        }
    }
    $manifestBackup = Join-Path $backup 'runtime-deployment-manifest.json'
    Copy-DurableExclusive -Source $runtimeManifestPath -Destination $manifestBackup
    $manifestHash = Get-FileSha256 -Path $runtimeManifestPath
    if ((Get-FileSha256 -Path $manifestBackup) -ne $manifestHash) {
        throw 'Runtime manifest backup verification failed.'
    }

    $removalOrder = @($OwnedFiles | Sort-Object @{
        Expression = {
            if ($_.RelativePath -eq 'dinput8.dll') { 0 } else { 1 }
        }
    })
    foreach ($file in $removalOrder) {
        if ((Get-FileSha256 -Path $file.FullPath) -ne $file.Sha256) {
            throw ('The runtime changed during backup: ' + $file.RelativePath)
        }
        Remove-Item -LiteralPath $file.FullPath -Force
    }
    if ((Get-FileSha256 -Path $runtimeManifestPath) -ne $manifestHash) {
        throw 'The runtime manifest changed during the operation.'
    }
    Remove-Item -LiteralPath $runtimeManifestPath -Force
    Write-Host ('Removed runtime backup: ' + $backup)
}

function Uninstall-Easy {
    Write-Step 'Pre-uninstallation checks'
    Assert-GameReady
    Assert-SidecarClosed
    $ownedRuntime = @(Get-OwnedRuntimeForRemoval)
    $hasProject = Test-Path -LiteralPath $projectManifestPath -PathType Leaf
    $hasRuntimeReceipt = Test-Path -LiteralPath $runtimeManifestPath -PathType Leaf
    $staleTemps = @(Get-StaleInstallerTemps)
    if (-not $hasProject -and -not $hasRuntimeReceipt -and
        $staleTemps.Count -eq 0) {
        $untrackedResidue = @(Get-UnsafeGameResidue)
        if ($untrackedResidue.Count -gt 0) {
            Write-Host ('WARNING: no manifest exists, but these files remain: ' +
                ($untrackedResidue -join ', ')) -ForegroundColor Red
            throw 'The uninstaller will not guess what may be removed. DO NOT ENTER RDO.'
        }
        Write-Host 'No installation created by this installer was found.'
        return
    }

    foreach ($file in $ownedRuntime) {
        Write-Host ('  backup + remove owned runtime: ' + $file.RelativePath)
    }
    $receipt = Read-RuntimeReceipt
    foreach ($entry in $receipt.Values) {
        if (-not $entry.OwnedByEasyInstaller) {
            Write-Host ('  leave pre-existing runtime: ' + $entry.RelativePath)
        }
    }
    if ($hasProject) {
        & (Join-Path $workspace 'scripts\Uninstall-DevBuild.ps1') `
            -GamePath $gameRoot `
            -WorkspaceRoot $workspace
    }
    foreach ($temp in $staleTemps) {
        Write-Host ('  remove stale non-loadable staging: ' + $temp.Name)
    }

    if ($Preview) {
        Write-Host 'Preview complete. Nothing was removed.' -ForegroundColor Yellow
        return
    }
    Confirm-Exact -Expected 'UNINSTALL' `
        -Prompt 'To continue, type exactly UNINSTALL'

    # Disable the loader first. If a later project-file cleanup fails, an
    # inert ASI is safer than a live dinput8 loader.
    if ($hasRuntimeReceipt) {
        Uninstall-Runtime -OwnedFiles $ownedRuntime
    }
    if ($hasProject) {
        & (Join-Path $workspace 'scripts\Uninstall-DevBuild.ps1') `
            -GamePath $gameRoot `
            -WorkspaceRoot $workspace `
            -Apply
    }
    Remove-StaleInstallerTemps -Files $staleTemps

    Write-Host ''
    Write-Host 'UNINSTALLATION COMPLETE.' -ForegroundColor Green
    Write-Host 'Files from other mods and third-party runtimes were left untouched.'
    $remainingUnsafe = @(Get-UnsafeGameResidue)
    if ($remainingUnsafe.Count -gt 0) {
        Write-Host ''
        Write-Host ('WARNING: still present: ' +
            ($remainingUnsafe -join ', ')) `
            -ForegroundColor Red
        Write-Host 'DO NOT ENTER RDO until you remove them manually.' `
            -ForegroundColor Red
    }
}

function Launch-Easy {
    Write-Step 'Installation check'
    Assert-GameReady -RequireSupportedHash
    Assert-DistReady
    Assert-NoConflictingMods
    if (-not (Test-ProjectAlreadyInstalled)) {
        throw 'The mod is not installed. Run 1_INSTALL_MOD.bat first.'
    }
    foreach ($spec in $runtimeFiles) {
        $target = Join-Path $gameRoot $spec.RelativePath
        if (-not (Test-Path -LiteralPath $target -PathType Leaf) -or
            (Get-FileSha256 -Path $target) -ne $spec.Sha256) {
            throw ('A valid runtime file is missing: ' + $spec.RelativePath)
        }
    }
    if (@(Get-Process -Name 'CoopStory.Sidecar' -ErrorAction SilentlyContinue).Count -gt 0) {
        throw 'The sidecar is already running. Close its window and restart the launcher.'
    }

    $sidecar = Join-Path $workspace 'dist\sidecar\CoopStory.Sidecar.exe'
    $config = Join-Path $gameRoot 'CoopStory.config.json'
    Test-ConfigSafety -Path $config

    $launchDirectory = Join-Path $workspace 'artifacts\launch'
    if (-not (Test-Path -LiteralPath $launchDirectory -PathType Container)) {
        $null = New-Item -ItemType Directory -Path $launchDirectory
    }
    $launchId = [Guid]::NewGuid().ToString('N')
    $readyFile = Join-Path $launchDirectory ('local-test-' + $launchId + '.ready')
    $failureFile = Join-Path $launchDirectory ('local-test-' + $launchId + '.failed')
    $escapedSidecar = $sidecar.Replace("'", "''")
    $escapedConfig = $config.Replace("'", "''")
    $escapedReady = $readyFile.Replace("'", "''")
    $escapedFailure = $failureFile.Replace("'", "''")
    $command = @"
`$Host.UI.RawUI.WindowTitle = 'RDR2 Coop Story - LOCAL TEST'
Write-Host 'Do not close this window during the test.' -ForegroundColor Cyan
Write-Host 'Success: LOCAL_TEST_BRIDGE_ACTIVE and LOCAL_TEST_GUEST_STREAMING appear.'
& '$escapedSidecar' local-test --config '$escapedConfig' --ready-file '$escapedReady' --motion-profile puppet
`$sidecarExit = `$LASTEXITCODE
if (-not (Test-Path -LiteralPath '$escapedReady')) {
    [IO.File]::WriteAllText('$escapedFailure', [string]`$sidecarExit)
}
Write-Host ''
Write-Host 'The sidecar stopped. Read the error message above.' -ForegroundColor Yellow
Read-Host 'Press Enter to close this window'
exit `$sidecarExit
"@
    $encoded = [Convert]::ToBase64String(
        [Text.Encoding]::Unicode.GetBytes($command))
    $sidecarWindow = Start-Process -FilePath $windowsPowerShell `
        -ArgumentList (
            '-NoLogo -NoProfile -ExecutionPolicy Bypass -EncodedCommand ' +
            $encoded) `
        -WorkingDirectory (Split-Path -Parent $sidecar) `
        -PassThru

    $ready = $false
    $readyWait = [Diagnostics.Stopwatch]::StartNew()
    while ($readyWait.ElapsedMilliseconds -lt 8000) {
        if (Test-Path -LiteralPath $failureFile -PathType Leaf) {
            $failureCode = Get-Content -LiteralPath $failureFile -Raw
            throw ('Local-test ended before becoming ready. Exit code: ' +
                $failureCode)
        }
        if (Test-Path -LiteralPath $readyFile -PathType Leaf) {
            $sidecarProcesses = @(
                Get-Process -Name 'CoopStory.Sidecar' -ErrorAction SilentlyContinue)
            if ($sidecarProcesses.Count -gt 0) {
                $ready = $true
                break
            }
        }
        $sidecarWindow.Refresh()
        if ($sidecarWindow.HasExited) {
            throw 'The local-test window closed before reaching readiness.'
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $ready) {
        throw 'Local-test did not become ready within 8 seconds. Steam was not started.'
    }

    Write-Host 'Local-test ready. Opening RDR2 through Steam...'
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = 'steam://rungameid/1174180'
    $startInfo.UseShellExecute = $true
    try {
        $null = [Diagnostics.Process]::Start($startInfo)
    }
    catch {
        Write-Warning 'Steam could not be opened automatically. Click Play in Steam manually.'
    }

    Write-Host ''
    Write-Host 'In the game, select STORY MODE only.' -ForegroundColor Yellow
    Write-Host 'NEVER enter Red Dead Online while the mod is installed.'
    Write-Host 'Always run 3_UNINSTALL_MOD.bat before entering RDO.'
}

try {
    switch ($Action) {
        'Install' { Install-Easy }
        'Launch' { Launch-Easy }
        'Uninstall' { Uninstall-Easy }
    }
}
catch {
    Write-Host ''
    Write-Host ('ERROR: ' + $_.Exception.Message) -ForegroundColor Red
    if ($Action -eq 'Install' -and -not $Preview) {
        Write-Host 'DO NOT START THE GAME OR RDO.' -ForegroundColor Red
        Write-Host 'Keep this folder and run 3_UNINSTALL_MOD.bat.' `
            -ForegroundColor Red
    }
    elseif ($Action -eq 'Uninstall' -and -not $Preview) {
        Write-Host 'The loader may still be present. DO NOT ENTER RDO.' `
            -ForegroundColor Red
        Write-Host 'Keep this folder, close the game/sidecar, and retry uninstallation.' `
            -ForegroundColor Red
    }
    exit 1
}

exit 0
