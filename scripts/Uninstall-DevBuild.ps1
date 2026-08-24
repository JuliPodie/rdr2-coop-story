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
    throw 'WorkspaceRoot nie istnieje lub nie jest katalogiem.'
}
$workspaceItem = Get-Item -LiteralPath $workspace -Force
if (($workspaceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'WorkspaceRoot nie moze byc reparse pointem.'
}

$gameRoot = [IO.Path]::GetFullPath($GamePath).TrimEnd('\', '/')
$gameExe = Join-Path $gameRoot 'RDR2.exe'
if (-not (Test-Path -LiteralPath $gameRoot -PathType Container) -or
    -not (Test-Path -LiteralPath $gameExe -PathType Leaf)) {
    throw 'GamePath nie wskazuje katalogu zawierajacego RDR2.exe.'
}
$gameRootItem = Get-Item -LiteralPath $gameRoot -Force
if (($gameRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'GamePath nie moze byc reparse pointem.'
}
if (@(Get-Process -Name RDR2 -ErrorAction SilentlyContinue).Count -gt 0) {
    throw 'RDR2 jest uruchomione. Zamknij gre przed odinstalowaniem.'
}

if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
    $ManifestPath = Join-Path $workspace 'artifacts\deploy\deployment-manifest.json'
}
$manifestFull = [IO.Path]::GetFullPath($ManifestPath)
$allowedManifestRoot = [IO.Path]::GetFullPath((Join-Path $workspace 'artifacts\deploy')).TrimEnd('\', '/')
if (-not (Test-PathWithin -Parent $allowedManifestRoot -Child $manifestFull)) {
    throw 'ManifestPath musi znajdowac sie w WorkspaceRoot\artifacts\deploy.'
}
if (Test-PathWithin -Parent $gameRoot -Child $manifestFull) {
    throw 'Manifest wdrozenia nie moze znajdowac sie w katalogu gry.'
}
if (-not (Test-Path -LiteralPath $manifestFull -PathType Leaf)) {
    throw 'Nie znaleziono manifestu wdrozenia; skrypt nie bedzie zgadywal, ktore pliki usunac.'
}

try {
    $manifest = Get-Content -LiteralPath $manifestFull -Raw | ConvertFrom-Json
}
catch {
    throw 'Manifest wdrozenia jest nieczytelny lub nie jest poprawnym JSON.'
}
if ($manifest.SchemaVersion -ne 1) {
    throw 'Nieobslugiwana wersja manifestu wdrozenia.'
}
$deploymentGuid = [Guid]::Empty
if (-not [Guid]::TryParse([string]$manifest.DeploymentId, [ref]$deploymentGuid)) {
    throw 'Manifest zawiera nieprawidlowy DeploymentId.'
}

$expectedRootFingerprint = Get-StringSha256 -Value ($gameRoot.ToLowerInvariant())
if ($manifest.GameRootFingerprint -ne $expectedRootFingerprint) {
    throw 'Manifest zostal utworzony dla innego katalogu gry.'
}

$allowedTargets = @('CoopStoryBridge.asi', 'CoopStory.config.json')
$manifestFiles = @($manifest.Files)
if ($manifestFiles.Count -lt 1 -or $manifestFiles.Count -gt $allowedTargets.Count) {
    throw 'Manifest zawiera nieprawidlowa liczbe plikow.'
}

$seen = @{}
$presentFiles = New-Object 'System.Collections.Generic.List[object]'
$missingFiles = New-Object 'System.Collections.Generic.List[string]'
foreach ($entry in $manifestFiles) {
    $relative = [string]$entry.RelativePath
    if (-not ($allowedTargets -contains $relative) -or
        [IO.Path]::IsPathRooted($relative) -or
        $relative.Contains('\') -or $relative.Contains('/')) {
        throw 'Manifest zawiera target spoza jawnej listy plikow projektu.'
    }
    if ($seen.ContainsKey($relative.ToLowerInvariant())) {
        throw 'Manifest zawiera zduplikowany target.'
    }
    $seen[$relative.ToLowerInvariant()] = $true

    $target = [IO.Path]::GetFullPath((Join-Path $gameRoot $relative))
    if (-not (Test-PathWithin -Parent $gameRoot -Child $target)) {
        throw 'Manifest zawiera target poza katalogiem gry.'
    }
    if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
        $missingFiles.Add($relative)
        continue
    }

    $actualHash = Get-FileSha256 -Path $target
    $manifestHash = [string]$entry.Sha256
    if ($manifestHash -notmatch '^[A-Fa-f0-9]{64}$') {
        throw 'Manifest zawiera nieprawidlowy hash pliku.'
    }
    $manifestHash = $manifestHash.ToUpperInvariant()
    $isMutable = ($entry.PSObject.Properties.Name -contains 'Mutable') -and ($entry.Mutable -eq $true)
    if ($actualHash -ne $manifestHash -and -not $isMutable) {
        throw ('Plik ' + $relative + ' zmienil sie od instalacji. Skrypt odmawia jego usuniecia.')
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
        throw 'BackupRoot istnieje, ale nie jest katalogiem.'
    }
    if (($backupRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'BackupRoot nie moze byc reparse pointem.'
    }
}
if ($backupRootFull.Equals($gameRoot, [StringComparison]::OrdinalIgnoreCase) -or
    (Test-PathWithin -Parent $gameRoot -Child $backupRootFull) -or
    (Test-PathWithin -Parent $backupRootFull -Child $gameRoot)) {
    throw 'BackupRoot nie moze pokrywac sie z katalogiem gry ani go zawierac.'
}
$backupName = 'uninstall-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-' +
    $deploymentGuid.ToString('N').Substring(0, 8)
$backupPath = [IO.Path]::GetFullPath((Join-Path $backupRootFull $backupName))
if (-not (Test-PathWithin -Parent $backupRootFull -Child $backupPath)) {
    throw 'Nie mozna bezpiecznie wyznaczyc katalogu backupu.'
}
if (Test-Path -LiteralPath $backupPath) {
    throw 'Katalog backupu juz istnieje; skrypt odmawia nadpisania.'
}

Write-Output 'Plan odinstalowania:'
foreach ($file in $presentFiles) {
    $suffix = if ($file.ModifiedSinceInstall) { ' (zmodyfikowany, backup aktualnej wersji)' } else { '' }
    Write-Output ('  backup + remove: ' + $file.RelativePath + $suffix)
}
foreach ($missing in $missingFiles) {
    Write-Output ('  juz nieobecny: ' + $missing)
}
Write-Output '  manifest: backup + remove z workspace'
Write-Output '  sidecar: bez zmian (nigdy nie byl kopiowany do katalogu gry)'
Write-Output '  ScriptHook/trainer: bez zmian'

if (-not $Apply) {
    Write-Output 'Dry-run zakonczony. Uzyj -Apply, aby wykonac dokladnie powyzsze operacje.'
    return
}
if (-not $PSCmdlet.ShouldProcess($gameRoot, 'Backup i usuniecie plikow zapisanych w manifescie wdrozenia')) {
    Write-Output 'Dry-run/WhatIf: nie zmieniono katalogu gry ani manifestu.'
    return
}

if (-not (Test-Path -LiteralPath $backupRootFull -PathType Container)) {
    $null = New-Item -ItemType Directory -Path $backupRootFull
}
if (Test-Path -LiteralPath $backupPath) {
    throw 'Katalog backupu pojawil sie w trakcie operacji; skrypt odmawia nadpisania.'
}
$null = New-Item -ItemType Directory -Path $backupPath

foreach ($file in $presentFiles) {
    $backupTarget = Join-Path $backupPath $file.RelativePath
    Copy-FileExclusive -Source $file.FullPath -Destination $backupTarget
    $backupHash = Get-FileSha256 -Path $backupTarget
    if ($backupHash -ne $file.Sha256) {
        throw ('Weryfikacja backupu nie powiodla sie dla: ' + $file.RelativePath)
    }
}

$manifestBackup = Join-Path $backupPath 'deployment-manifest.json'
Copy-FileExclusive -Source $manifestFull -Destination $manifestBackup
$manifestSourceHash = Get-FileSha256 -Path $manifestFull
$manifestBackupHash = Get-FileSha256 -Path $manifestBackup
if ($manifestSourceHash -ne $manifestBackupHash) {
    throw 'Weryfikacja backupu manifestu nie powiodla sie.'
}

# Every removal below is a single, validated file with a verified backup.
foreach ($file in $presentFiles) {
    Remove-Item -LiteralPath $file.FullPath -Force
}
Remove-Item -LiteralPath $manifestFull -Force

Write-Output ('Odinstalowanie zakonczone. Backup: ' + $backupPath)
