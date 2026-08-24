#Requires -Version 5.1

[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [Parameter(Mandatory = $true)]
    [string]$GamePath,

    [string]$WorkspaceRoot,

    [string]$ManifestPath,

    [string]$BackupRoot,

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

function Copy-FileExclusive {
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
        }
        finally {
            $outputStream.Dispose()
        }
    }
    finally {
        $inputStream.Dispose()
    }
}

$workspace = [IO.Path]::GetFullPath($WorkspaceRoot).TrimEnd('\', '/')
if (-not (Test-Path -LiteralPath $workspace -PathType Container)) {
    throw 'WorkspaceRoot does not exist or is not a directory.'
}
$workspaceItem = Get-Item -LiteralPath $workspace -Force
if (($workspaceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'WorkspaceRoot cannot be a reparse point.'
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
if (@(Get-Process -Name RDR2 -ErrorAction SilentlyContinue).Count -gt 0) {
    throw 'RDR2 is running. Close the game before uninstalling.'
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
    throw 'The deployment manifest cannot be stored in the game directory.'
}
if (-not (Test-Path -LiteralPath $manifestFull -PathType Leaf)) {
    throw 'The deployment manifest was not found; the script will not guess which files to remove.'
}

try {
    $manifest = Get-Content -LiteralPath $manifestFull -Raw | ConvertFrom-Json
}
catch {
    throw 'The deployment manifest is unreadable or is not valid JSON.'
}
if ($manifest.SchemaVersion -ne 1) {
    throw 'Unsupported deployment manifest version.'
}
$deploymentGuid = [Guid]::Empty
if (-not [Guid]::TryParse([string]$manifest.DeploymentId, [ref]$deploymentGuid)) {
    throw 'The manifest contains an invalid DeploymentId.'
}

$expectedRootFingerprint = Get-StringSha256 -Value ($gameRoot.ToLowerInvariant())
if ($manifest.GameRootFingerprint -ne $expectedRootFingerprint) {
    throw 'The manifest was created for a different game directory.'
}

$allowedTargets = @('CoopStoryBridge.asi', 'CoopStory.config.json')
$manifestFiles = @($manifest.Files)
if ($manifestFiles.Count -lt 1 -or $manifestFiles.Count -gt $allowedTargets.Count) {
    throw 'The manifest contains an invalid file count.'
}

$seen = @{}
$presentFiles = New-Object 'System.Collections.Generic.List[object]'
$missingFiles = New-Object 'System.Collections.Generic.List[string]'
foreach ($entry in $manifestFiles) {
    $relative = [string]$entry.RelativePath
    if (-not ($allowedTargets -contains $relative) -or
        [IO.Path]::IsPathRooted($relative) -or
        $relative.Contains('\') -or $relative.Contains('/')) {
        throw 'The manifest contains a target outside the explicit project file list.'
    }
    if ($seen.ContainsKey($relative.ToLowerInvariant())) {
        throw 'The manifest contains a duplicate target.'
    }
    $seen[$relative.ToLowerInvariant()] = $true

    $target = [IO.Path]::GetFullPath((Join-Path $gameRoot $relative))
    if (-not (Test-PathWithin -Parent $gameRoot -Child $target)) {
        throw 'The manifest contains a target outside the game directory.'
    }
    if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
        $missingFiles.Add($relative)
        continue
    }

    $actualHash = Get-FileSha256 -Path $target
    $manifestHash = [string]$entry.Sha256
    if ($manifestHash -notmatch '^[A-Fa-f0-9]{64}$') {
        throw 'The manifest contains an invalid file hash.'
    }
    $manifestHash = $manifestHash.ToUpperInvariant()
    $isMutable = ($entry.PSObject.Properties.Name -contains 'Mutable') -and ($entry.Mutable -eq $true)
    if ($actualHash -ne $manifestHash -and -not $isMutable) {
        throw ('File ' + $relative + ' changed after installation. The script refuses to remove it.')
    }
    $presentFiles.Add([pscustomobject]@{
        RelativePath = $relative
        FullPath = $target
        Sha256 = $actualHash
        ModifiedSinceInstall = ($actualHash -ne $manifestHash)
    })
}

if ([string]::IsNullOrWhiteSpace($BackupRoot)) {
    $BackupRoot = Join-Path $workspace 'artifacts\uninstall-backups'
}
$backupRootFull = [IO.Path]::GetFullPath($BackupRoot).TrimEnd('\', '/')
if (Test-Path -LiteralPath $backupRootFull) {
    $backupRootItem = Get-Item -LiteralPath $backupRootFull -Force
    if (-not $backupRootItem.PSIsContainer) {
        throw 'BackupRoot exists but is not a directory.'
    }
    if (($backupRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'BackupRoot cannot be a reparse point.'
    }
}
if ($backupRootFull.Equals($gameRoot, [StringComparison]::OrdinalIgnoreCase) -or
    (Test-PathWithin -Parent $gameRoot -Child $backupRootFull) -or
    (Test-PathWithin -Parent $backupRootFull -Child $gameRoot)) {
    throw 'BackupRoot cannot overlap with or contain the game directory.'
}
$backupName = 'uninstall-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-' +
    $deploymentGuid.ToString('N').Substring(0, 8)
$backupPath = [IO.Path]::GetFullPath((Join-Path $backupRootFull $backupName))
if (-not (Test-PathWithin -Parent $backupRootFull -Child $backupPath)) {
    throw 'The backup directory cannot be determined safely.'
}
if (Test-Path -LiteralPath $backupPath) {
    throw 'The backup directory already exists; the script refuses to overwrite it.'
}

Write-Output 'Plan odinstalowania:'
foreach ($file in $presentFiles) {
    $suffix = if ($file.ModifiedSinceInstall) { ' (modified; backing up the current version)' } else { '' }
    Write-Output ('  backup + remove: ' + $file.RelativePath + $suffix)
}
foreach ($missing in $missingFiles) {
    Write-Output ('  already absent: ' + $missing)
}
Write-Output '  manifest: back up + remove from workspace'
Write-Output '  sidecar: unchanged (it was never copied into the game directory)'
Write-Output '  ScriptHook/trainer: unchanged'

if (-not $Apply) {
    Write-Output 'Dry run complete. Use -Apply to perform exactly the operations shown above.'
    return
}
if (-not $PSCmdlet.ShouldProcess($gameRoot, 'Back up and remove files recorded in the deployment manifest')) {
    Write-Output 'Dry-run/WhatIf: the game directory and manifest were not changed.'
    return
}

if (-not (Test-Path -LiteralPath $backupRootFull -PathType Container)) {
    $null = New-Item -ItemType Directory -Path $backupRootFull
}
if (Test-Path -LiteralPath $backupPath) {
    throw 'The backup directory appeared during the operation; the script refuses to overwrite it.'
}
$null = New-Item -ItemType Directory -Path $backupPath

foreach ($file in $presentFiles) {
    $backupTarget = Join-Path $backupPath $file.RelativePath
    Copy-FileExclusive -Source $file.FullPath -Destination $backupTarget
    $backupHash = Get-FileSha256 -Path $backupTarget
    if ($backupHash -ne $file.Sha256) {
        throw ('Backup verification failed for: ' + $file.RelativePath)
    }
}

$manifestBackup = Join-Path $backupPath 'deployment-manifest.json'
Copy-FileExclusive -Source $manifestFull -Destination $manifestBackup
$manifestSourceHash = Get-FileSha256 -Path $manifestFull
$manifestBackupHash = Get-FileSha256 -Path $manifestBackup
if ($manifestSourceHash -ne $manifestBackupHash) {
    throw 'Manifest backup verification failed.'
}

# Every removal below is a single, validated file with a verified backup.
foreach ($file in $presentFiles) {
    Remove-Item -LiteralPath $file.FullPath -Force
}
Remove-Item -LiteralPath $manifestFull -Force

Write-Output ('Uninstallation complete. Backup: ' + $backupPath)
