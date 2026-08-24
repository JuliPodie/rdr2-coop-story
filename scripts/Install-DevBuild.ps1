#Requires -Version 5.1

[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Low')]
param(
    [Parameter(Mandatory = $true)]
    [string]$GamePath,

    [string]$WorkspaceRoot,

    [string]$PackageRoot,

    [string]$ManifestPath,

    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string]$ExpectedGameSha256 = 'B56C9548F670654A9B73BF25DEF3CD73AF12E269F6E47DBA28A34079ADAF465E',

    [switch]$Apply
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($WorkspaceRoot)) {
    $WorkspaceRoot = Split-Path -Parent $PSScriptRoot
}

function Test-PathWithin {
    param(
        [Parameter(Mandatory = $true)][string]$Parent,
        [Parameter(Mandatory = $true)][string]$Child
    )

    $parentFull = [IO.Path]::GetFullPath($Parent).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $childFull = [IO.Path]::GetFullPath($Child)
    return $childFull.StartsWith($parentFull, [StringComparison]::OrdinalIgnoreCase)
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

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '')
    }
    finally {
        $stream.Dispose()
        $sha.Dispose()
    }
}

function Get-PropertyValue {
    param(
        [Parameter(Mandatory = $true)]$Object,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $property = $Object.PSObject.Properties |
        Where-Object { $_.Name.Equals($Name, [StringComparison]::OrdinalIgnoreCase) } |
        Select-Object -First 1
    if ($null -eq $property) {
        return $null
    }
    return $property.Value
}

function Copy-FileToStagingExclusive {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    $inputStream = [IO.File]::Open($Source, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
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

function Write-NewUtf8File {
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

function Get-SafeTreeItems {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [int]$MaxFiles = 5000,
        [int64]$MaxBytes = 4GB
    )

    $pending = New-Object 'System.Collections.Generic.Queue[string]'
    $items = New-Object 'System.Collections.Generic.List[object]'
    $pending.Enqueue($Root)
    $fileCount = 0
    [int64]$totalBytes = 0

    while ($pending.Count -gt 0) {
        $directory = $pending.Dequeue()
        foreach ($item in (Get-ChildItem -LiteralPath $directory -Force)) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw 'The dist package contains a reparse point; installation was stopped.'
            }
            $items.Add($item)
            if ($item.PSIsContainer) {
                $pending.Enqueue($item.FullName)
            }
            else {
                $fileCount++
                $totalBytes += [int64]$item.Length
                if ($fileCount -gt $MaxFiles -or $totalBytes -gt $MaxBytes) {
                    throw 'The dist package exceeds the safe file count or size limit.'
                }
            }
        }
    }

    return @($items | ForEach-Object { $_ })
}

$workspace = [IO.Path]::GetFullPath($WorkspaceRoot).TrimEnd('\', '/')
if (-not (Test-Path -LiteralPath $workspace -PathType Container)) {
    throw 'WorkspaceRoot does not exist or is not a directory.'
}
$workspaceItem = Get-Item -LiteralPath $workspace -Force
if (($workspaceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'WorkspaceRoot cannot be a reparse point.'
}

if ([string]::IsNullOrWhiteSpace($PackageRoot)) {
    $PackageRoot = Join-Path $workspace 'dist'
}
$package = [IO.Path]::GetFullPath($PackageRoot).TrimEnd('\', '/')
if (-not (Test-Path -LiteralPath $package -PathType Container)) {
    throw 'PackageRoot does not exist. Build and publish the dist directory first.'
}
$packageItem = Get-Item -LiteralPath $package -Force
if (($packageItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'PackageRoot cannot be a reparse point.'
}
if (-not (Test-PathWithin -Parent $workspace -Child $package)) {
    throw 'PackageRoot must be inside WorkspaceRoot.'
}

$gameRoot = [IO.Path]::GetFullPath($GamePath).TrimEnd('\', '/')
$gameExe = Join-Path $gameRoot 'RDR2.exe'
if (-not (Test-Path -LiteralPath $gameRoot -PathType Container) -or
    -not (Test-Path -LiteralPath $gameExe -PathType Leaf)) {
    throw 'GamePath does not point to a directory containing RDR2.exe.'
}
$gameRootItem = Get-Item -LiteralPath $gameRoot -Force
if (($gameRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'GamePath cannot be a reparse point.'
}
if ((Test-PathWithin -Parent $gameRoot -Child $workspace) -or
    (Test-PathWithin -Parent $workspace -Child $gameRoot) -or
    $gameRoot.Equals($workspace, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'WorkspaceRoot and GamePath cannot overlap or contain one another.'
}
if (@(Get-Process -Name RDR2 -ErrorAction SilentlyContinue).Count -gt 0) {
    throw 'RDR2 is running. Close the game before installation.'
}

$actualGameHash = Get-FileSha256 -Path $gameExe
if ($actualGameHash -ne $ExpectedGameSha256.ToUpperInvariant()) {
    throw 'The RDR2.exe hash does not match supported build 1.0.1491.50.'
}

if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
    $ManifestPath = Join-Path $workspace 'artifacts\deploy\deployment-manifest.json'
}
$manifestFull = [IO.Path]::GetFullPath($ManifestPath)
$allowedManifestRoot = [IO.Path]::GetFullPath((Join-Path $workspace 'artifacts\deploy')).TrimEnd('\', '/')
if (-not (Test-PathWithin -Parent $allowedManifestRoot -Child $manifestFull)) {
    throw 'ManifestPath must be inside WorkspaceRoot\artifacts\deploy.'
}
if (Test-PathWithin -Parent $gameRoot -Child $manifestFull) {
    throw 'The deployment manifest must never be stored in the game directory.'
}
if (Test-Path -LiteralPath $manifestFull) {
    throw 'The deployment manifest already exists. Run Uninstall-DevBuild.ps1 first.'
}

$bridgeSource = Join-Path $package 'CoopStoryBridge.asi'
$configSource = Join-Path $package 'config\coopstory.example.json'
$sidecarRoot = Join-Path $package 'sidecar'
$sidecarExe = Join-Path $sidecarRoot 'CoopStory.Sidecar.exe'
foreach ($requiredFile in @($bridgeSource, $configSource, $sidecarExe)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw 'The dist package does not contain all required bridge/config/sidecar artifacts.'
    }
    $item = Get-Item -LiteralPath $requiredFile -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'Dist artifacts cannot be reparse points.'
    }
}

$forbiddenRedistribution = @('ScriptHookRDR2.dll', 'dinput8.dll', 'NativeTrainer.asi')
$packageItems = @(Get-SafeTreeItems -Root $package)
if (@($packageItems | Where-Object {
    (-not $_.PSIsContainer) -and ($forbiddenRedistribution -contains $_.Name)
}).Count -gt 0) {
    throw 'The dist package contains a runtime/trainer file that the project cannot redistribute.'
}

try {
    $config = Get-Content -LiteralPath $configSource -Raw | ConvertFrom-Json
}
catch {
    throw 'coopstory.example.json is not a valid JSON document.'
}
$schemaVersion = Get-PropertyValue -Object $config -Name 'schemaVersion'
$role = [string](Get-PropertyValue -Object $config -Name 'role')
if ($schemaVersion -ne 1 -or @('Host', 'Guest') -notcontains $role) {
    throw 'The configuration must use schemaVersion=1 and role Host or Guest.'
}
$tcpPort = Get-PropertyValue -Object $config -Name 'tcpPort'
$udpPort = Get-PropertyValue -Object $config -Name 'udpPort'
if ($tcpPort -lt 1 -or $tcpPort -gt 65535 -or
    $udpPort -lt 1 -or $udpPort -gt 65535 -or
    $tcpPort -eq $udpPort) {
    throw 'The configuration contains invalid or identical TCP/UDP ports.'
}
$safety = Get-PropertyValue -Object $config -Name 'safety'
if ($null -eq $safety -or
    (Get-PropertyValue -Object $safety -Name 'storyModeOnly') -ne $true -or
    (Get-PropertyValue -Object $safety -Name 'refuseOnlineMode') -ne $true) {
    throw 'The configuration must enforce safety.storyModeOnly=true and safety.refuseOnlineMode=true.'
}

$bridgeTarget = Join-Path $gameRoot 'CoopStoryBridge.asi'
$configTarget = Join-Path $gameRoot 'CoopStory.config.json'
foreach ($target in @($bridgeTarget, $configTarget)) {
    if (-not (Test-PathWithin -Parent $gameRoot -Child $target)) {
        throw 'A target outside the game directory was detected.'
    }
    if (Test-Path -LiteralPath $target) {
        throw 'The destination project file already exists; the installer refuses to overwrite it.'
    }
}

$sidecarPrefix = $sidecarRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
$sidecarFiles = @($packageItems |
    Where-Object {
        (-not $_.PSIsContainer) -and
        $_.FullName.StartsWith($sidecarPrefix, [StringComparison]::OrdinalIgnoreCase)
    } |
    Sort-Object FullName)
if ($sidecarFiles.Count -eq 0 -or $sidecarFiles.Count -gt 1000) {
    throw 'The sidecar directory is empty or exceeds the safe limit of 1000 files.'
}
$sidecarLines = New-Object 'System.Collections.Generic.List[string]'
foreach ($file in $sidecarFiles) {
    $relative = $file.FullName.Substring($sidecarRoot.TrimEnd('\', '/').Length + 1).Replace('\', '/')
    $hash = Get-FileSha256 -Path $file.FullName
    $sidecarLines.Add($relative + '|' + $file.Length + '|' + $hash)
}
$sidecarSetHash = Get-StringSha256 -Value (($sidecarLines | Sort-Object) -join "`n")

$bridgeHash = Get-FileSha256 -Path $bridgeSource
$configHash = Get-FileSha256 -Path $configSource
$manifest = [ordered]@{
    SchemaVersion = 1
    DeploymentId = [Guid]::NewGuid().ToString('D')
    CreatedAtUtc = [DateTime]::UtcNow.ToString('o')
    GameRootFingerprint = (Get-StringSha256 -Value ($gameRoot.ToLowerInvariant()))
    GameExecutableSha256 = $actualGameHash
    Files = @(
        [ordered]@{
            RelativePath = 'CoopStoryBridge.asi'
            Sha256 = $bridgeHash
            Length = [int64](Get-Item -LiteralPath $bridgeSource).Length
            Mutable = $false
        },
        [ordered]@{
            RelativePath = 'CoopStory.config.json'
            Sha256 = $configHash
            Length = [int64](Get-Item -LiteralPath $configSource).Length
            Mutable = $true
        }
    )
    Sidecar = [ordered]@{
        RelativeToWorkspace = 'dist/sidecar'
        EntryPoint = 'CoopStory.Sidecar.exe'
        FileCount = $sidecarFiles.Count
        ContentSetSha256 = $sidecarSetHash
        InstalledIntoGame = $false
    }
    ScriptHookBundledOrInstalledByThisScript = $false
    OfflineOnlyBridgeGuardRequired = $true
}

Write-Output 'Plan wdrozenia:'
Write-Output '  game root: CoopStoryBridge.asi'
Write-Output '  game root: CoopStory.config.json'
Write-Output '  sidecar: remains in workspace\dist\sidecar'
Write-Output '  ScriptHook/trainer: unchanged'

if (-not $Apply) {
    Write-Output 'Dry run complete. Use -Apply to perform exactly the operations shown above.'
    return
}
if (-not $PSCmdlet.ShouldProcess($gameRoot, 'Install two RDR2 Coop Story project files')) {
    Write-Output 'Dry-run/WhatIf: the game directory was not changed.'
    return
}

$manifestDirectory = Split-Path -Parent $manifestFull
if (-not (Test-Path -LiteralPath $manifestDirectory -PathType Container)) {
    $null = New-Item -ItemType Directory -Path $manifestDirectory
}
if (Test-Path -LiteralPath $manifestFull) {
    throw 'The manifest appeared during the operation; the installer refuses to overwrite it.'
}

$deploymentTag = ([string]$manifest.DeploymentId).Replace('-', '')
$bridgeStaging = Join-Path $gameRoot ('.coopstory-' + $deploymentTag + '-bridge.tmp')
$configStaging = Join-Path $gameRoot ('.coopstory-' + $deploymentTag + '-config.tmp')
$manifestContent = $manifest | ConvertTo-Json -Depth 8
$manifestWrittenHash = $null
$bridgeCommitted = $false
$configCommitted = $false

try {
    # Staging files do not use a loader-recognized extension. Both are copied,
    # flushed to disk and verified before either final target becomes visible.
    Copy-FileToStagingExclusive -Source $bridgeSource -Destination $bridgeStaging
    if ((Get-FileSha256 -Path $bridgeStaging) -ne $bridgeHash) {
        throw 'Bridge staging verification failed.'
    }

    Copy-FileToStagingExclusive -Source $configSource -Destination $configStaging
    if ((Get-FileSha256 -Path $configStaging) -ne $configHash) {
        throw 'Configuration staging verification failed.'
    }

    # The manifest is durable before the first atomic rename. If the process is
    # interrupted between renames, Uninstall-DevBuild can identify the complete
    # final file by its expected hash.
    Write-NewUtf8File -Path $manifestFull -Content $manifestContent
    $manifestWrittenHash = Get-FileSha256 -Path $manifestFull

    if ((Test-Path -LiteralPath $bridgeTarget) -or
        (Test-Path -LiteralPath $configTarget)) {
        throw 'A destination file appeared during staging; the installer refuses to overwrite it.'
    }

    [IO.File]::Move($bridgeStaging, $bridgeTarget)
    $bridgeCommitted = $true
    [IO.File]::Move($configStaging, $configTarget)
    $configCommitted = $true

    if ((Get-FileSha256 -Path $bridgeTarget) -ne $bridgeHash -or
        (Get-FileSha256 -Path $configTarget) -ne $configHash) {
        throw 'Final project file verification failed.'
    }
}
catch {
    # Roll back only paths created by this deployment and only final files that
    # still have the exact verified content we committed.
    $rollbackComplete = $true
    if ($configCommitted -and (Test-Path -LiteralPath $configTarget -PathType Leaf)) {
        if ((Get-FileSha256 -Path $configTarget) -eq $configHash) {
            Remove-Item -LiteralPath $configTarget -Force
        }
        else {
            $rollbackComplete = $false
        }
    }
    if ($bridgeCommitted -and (Test-Path -LiteralPath $bridgeTarget -PathType Leaf)) {
        if ((Get-FileSha256 -Path $bridgeTarget) -eq $bridgeHash) {
            Remove-Item -LiteralPath $bridgeTarget -Force
        }
        else {
            $rollbackComplete = $false
        }
    }
    foreach ($ownedStagingPath in @($bridgeStaging, $configStaging)) {
        if (Test-Path -LiteralPath $ownedStagingPath) {
            Remove-Item -LiteralPath $ownedStagingPath -Force
        }
    }
    if ($rollbackComplete -and
        $null -ne $manifestWrittenHash -and
        (Test-Path -LiteralPath $manifestFull -PathType Leaf) -and
        (Get-FileSha256 -Path $manifestFull) -eq $manifestWrittenHash) {
        Remove-Item -LiteralPath $manifestFull -Force
    }
    throw
}

$runtimePresent = (Test-Path -LiteralPath (Join-Path $gameRoot 'ScriptHookRDR2.dll') -PathType Leaf) -and
    (Test-Path -LiteralPath (Join-Path $gameRoot 'dinput8.dll') -PathType Leaf)

Write-Output ('Deployment complete. Manifest: ' + $manifestFull)
if (-not $runtimePresent) {
    Write-Warning 'The Script Hook runtime is not present. This script intentionally does not install or redistribute it.'
}
