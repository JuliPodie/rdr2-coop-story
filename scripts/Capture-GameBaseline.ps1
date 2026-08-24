#Requires -Version 5.1

[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Low')]
param(
    [Parameter(Mandatory = $true)]
    [string]$GamePath,

    [string]$OutputPath,

    [ValidateRange(1, 4096)]
    [int]$MaxHashSizeMiB = 1024,

    [string[]]$CriticalFiles = @(
        'RDR2.exe',
        'PlayRDR2.exe',
        'bink2w64.dll',
        'amd_ags_x64.dll',
        'oo2core_5_win64.dll'
    )
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Test-PathWithin {
    param(
        [Parameter(Mandatory = $true)][string]$Parent,
        [Parameter(Mandatory = $true)][string]$Child
    )

    $parentFull = [IO.Path]::GetFullPath($Parent).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $childFull = [IO.Path]::GetFullPath($Child)
    return $childFull.StartsWith($parentFull, [StringComparison]::OrdinalIgnoreCase)
}

function Get-SafeChildPath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    if ([IO.Path]::IsPathRooted($RelativePath)) {
        throw 'CriticalFiles moze zawierac tylko sciezki wzgledne.'
    }

    $candidate = [IO.Path]::GetFullPath((Join-Path $Root $RelativePath))
    if (-not (Test-PathWithin -Parent $Root -Child $candidate)) {
        throw 'CriticalFiles zawiera sciezke wychodzaca poza katalog gry.'
    }
    return $candidate
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

$gameRoot = [IO.Path]::GetFullPath($GamePath).TrimEnd('\', '/')
$gameExe = Join-Path $gameRoot 'RDR2.exe'
if (-not (Test-Path -LiteralPath $gameRoot -PathType Container)) {
    throw 'GamePath nie istnieje lub nie jest katalogiem.'
}
$gameRootItem = Get-Item -LiteralPath $gameRoot -Force
if (($gameRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'GamePath nie moze byc reparse pointem.'
}
if (-not (Test-Path -LiteralPath $gameExe -PathType Leaf)) {
    throw 'GamePath nie zawiera RDR2.exe.'
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $workspaceRoot = Split-Path -Parent $PSScriptRoot
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
    $OutputPath = Join-Path $workspaceRoot ('artifacts\baselines\game-baseline-' + $stamp + '.json')
}

$outputFull = [IO.Path]::GetFullPath($OutputPath)
if (Test-PathWithin -Parent $gameRoot -Child $outputFull) {
    throw 'Baseline musi zostac zapisany poza katalogiem gry.'
}
if (Test-Path -LiteralPath $outputFull) {
    throw 'Plik baseline juz istnieje; skrypt odmawia nadpisania.'
}

$criticalLookup = @{}
foreach ($relativeCritical in $CriticalFiles) {
    if ([string]::IsNullOrWhiteSpace($relativeCritical)) {
        throw 'CriticalFiles nie moze zawierac pustych wartosci.'
    }
    $safeCriticalPath = Get-SafeChildPath -Root $gameRoot -RelativePath $relativeCritical
    $normalizedRelative = $relativeCritical.Replace('\', '/').TrimStart('/')
    $criticalLookup[$normalizedRelative.ToLowerInvariant()] = $safeCriticalPath
}

$maxHashBytes = [int64]$MaxHashSizeMiB * 1MB
$criticalResults = New-Object 'System.Collections.Generic.List[object]'
foreach ($key in ($criticalLookup.Keys | Sort-Object)) {
    $criticalPath = $criticalLookup[$key]
    $displayName = $key.Replace('\', '/')
    if (-not (Test-Path -LiteralPath $criticalPath -PathType Leaf)) {
        $criticalResults.Add([pscustomobject]@{
            RelativePath = $displayName
            Present = $false
            Length = $null
            LastWriteUtc = $null
            Sha256 = $null
            HashStatus = 'Missing'
        })
        continue
    }

    $file = Get-Item -LiteralPath $criticalPath -Force
    $hash = $null
    $hashStatus = 'SkippedTooLarge'
    if ($file.Length -le $maxHashBytes) {
        $hash = Get-FileSha256 -Path $file.FullName
        $hashStatus = 'Hashed'
    }
    $criticalResults.Add([pscustomobject]@{
        RelativePath = $displayName
        Present = $true
        Length = [int64]$file.Length
        LastWriteUtc = $file.LastWriteTimeUtc.ToString('o')
        Sha256 = $hash
        HashStatus = $hashStatus
    })
}

$topLevel = New-Object 'System.Collections.Generic.List[object]'
$topItems = @(Get-ChildItem -LiteralPath $gameRoot -Force | Sort-Object Name)
foreach ($item in $topItems) {
    $kind = if ($item.PSIsContainer) { 'Directory' } else { 'File' }
    $length = if ($item.PSIsContainer) { $null } else { [int64]$item.Length }
    $topLevel.Add([pscustomobject]@{
        Name = $item.Name
        Kind = $kind
        Length = $length
        LastWriteUtc = $item.LastWriteTimeUtc.ToString('o')
        ReparsePoint = (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
    })
}

$knownLoaderNames = @(
    'dinput8.dll',
    'ScriptHookRDR2.dll',
    'NativeTrainer.asi',
    'version.dll',
    'dxgi.dll',
    'vfs.asi',
    'ModManager.Core.dll'
)
$knownLoaderDirectories = @(
    'lml',
    'RampageFiles',
    'RedM'
)
$modIndicators = New-Object 'System.Collections.Generic.List[object]'
foreach ($item in $topItems) {
    $isIndicator = $false
    $reason = $null

    if (-not $item.PSIsContainer -and $item.Extension.Equals('.asi', [StringComparison]::OrdinalIgnoreCase)) {
        $isIndicator = $true
        $reason = 'ASI module'
    }
    elseif (-not $item.PSIsContainer -and
        @($knownLoaderNames | Where-Object { $_.Equals($item.Name, [StringComparison]::OrdinalIgnoreCase) }).Count -gt 0) {
        $isIndicator = $true
        $reason = 'Known loader or mod file'
    }
    elseif ($item.PSIsContainer -and
        @($knownLoaderDirectories | Where-Object { $_.Equals($item.Name, [StringComparison]::OrdinalIgnoreCase) }).Count -gt 0) {
        $isIndicator = $true
        $reason = 'Known mod directory'
    }

    if ($isIndicator) {
        $modIndicators.Add([pscustomobject]@{
            Name = $item.Name
            Kind = if ($item.PSIsContainer) { 'Directory' } else { 'File' }
            Reason = $reason
        })
    }
}

$gameVersionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($gameExe)
$baseline = [ordered]@{
    SchemaVersion = 1
    CapturedAtUtc = [DateTime]::UtcNow.ToString('o')
    GameRootFingerprint = (Get-StringSha256 -Value ($gameRoot.ToLowerInvariant()))
    Game = [ordered]@{
        ProductVersion = $gameVersionInfo.ProductVersion
        FileVersion = $gameVersionInfo.FileVersion
        ExecutableSha256 = (Get-FileSha256 -Path $gameExe)
    }
    HashPolicy = [ordered]@{
        Scope = 'Explicit critical files only'
        MaxFileSizeMiB = $MaxHashSizeMiB
        TopLevelFilesAreMetadataOnly = $true
    }
    CriticalFiles = @($criticalResults | ForEach-Object { $_ })
    TopLevelEntries = @($topLevel | ForEach-Object { $_ })
    ModIndicators = @($modIndicators | ForEach-Object { $_ })
}

$json = $baseline | ConvertTo-Json -Depth 8
$outputDirectory = Split-Path -Parent $outputFull

if ($PSCmdlet.ShouldProcess($outputFull, 'Zapis manifestu baseline gry')) {
    if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
        $null = New-Item -ItemType Directory -Path $outputDirectory
    }
    if (Test-Path -LiteralPath $outputFull) {
        throw 'Plik baseline pojawil sie w trakcie operacji; skrypt odmawia nadpisania.'
    }

    $utf8NoBom = New-Object Text.UTF8Encoding($false)
    $stream = [IO.File]::Open(
        $outputFull,
        [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write,
        [IO.FileShare]::None
    )
    try {
        $writer = New-Object IO.StreamWriter($stream, $utf8NoBom)
        try {
            $writer.Write($json)
        }
        finally {
            $writer.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
    Write-Output ('Baseline zapisany: ' + $outputFull)
}
else {
    Write-Output ('Dry-run: baseline objalby ' + $topLevel.Count + ' wpisow top-level i ' +
        $criticalResults.Count + ' plikow krytycznych.')
}
