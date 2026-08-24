#Requires -Version 5.1

[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Low')]
param(
    [string]$SourcePath = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Rockstar Games\Red Dead Redemption 2\Profiles'),

    [Parameter(Mandatory = $true)]
    [string]$DestinationRoot,

    [string]$BackupName,

    [ValidateRange(1, 100000)]
    [int]$MaxFiles = 10000,

    [ValidateRange(1, 102400)]
    [int]$MaxTotalSizeMiB = 2048
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

function Get-SafeFileInventory {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][int]$FileLimit,
        [Parameter(Mandatory = $true)][int64]$ByteLimit
    )

    $rootPrefix = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $pending = New-Object 'System.Collections.Generic.Queue[string]'
    $files = New-Object 'System.Collections.Generic.List[object]'
    $pending.Enqueue($Root)
    [int64]$totalBytes = 0

    while ($pending.Count -gt 0) {
        $directory = $pending.Dequeue()
        foreach ($item in (Get-ChildItem -LiteralPath $directory -Force)) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw 'Zrodlo zawiera reparse point; backup zostal zatrzymany, aby nie wyjsc poza wskazany katalog.'
            }

            if ($item.PSIsContainer) {
                $pending.Enqueue($item.FullName)
                continue
            }

            if (-not $item.FullName.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Wykryto plik poza katalogiem zrodlowym.'
            }

            $relative = $item.FullName.Substring($rootPrefix.Length).Replace('\', '/')
            $totalBytes += [int64]$item.Length
            $files.Add([pscustomobject]@{
                SourcePath = $item.FullName
                RelativePath = $relative
                Length = [int64]$item.Length
                LastWriteUtc = $item.LastWriteTimeUtc
            })

            if ($files.Count -gt $FileLimit) {
                throw ('Zrodlo przekracza limit ' + $FileLimit + ' plikow.')
            }
            if ($totalBytes -gt $ByteLimit) {
                throw ('Zrodlo przekracza limit ' + $MaxTotalSizeMiB + ' MiB.')
            }
        }
    }

    return [pscustomobject]@{
        Files = @($files | Sort-Object RelativePath)
        TotalBytes = $totalBytes
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
    [IO.File]::SetLastWriteTimeUtc($Destination, [IO.File]::GetLastWriteTimeUtc($Source))
}

$sourceRoot = [IO.Path]::GetFullPath($SourcePath).TrimEnd('\', '/')
if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) {
    throw 'SourcePath nie istnieje lub nie jest katalogiem.'
}
$sourceItem = Get-Item -LiteralPath $sourceRoot -Force
if (($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'SourcePath nie moze byc reparse pointem.'
}

if (-not [IO.Path]::IsPathRooted($DestinationRoot)) {
    throw 'DestinationRoot musi byc jawna, bezwzgledna sciezka.'
}
$destinationFull = [IO.Path]::GetFullPath($DestinationRoot).TrimEnd('\', '/')
if (Test-Path -LiteralPath $destinationFull) {
    $destinationItem = Get-Item -LiteralPath $destinationFull -Force
    if (-not $destinationItem.PSIsContainer) {
        throw 'DestinationRoot istnieje, ale nie jest katalogiem.'
    }
    if (($destinationItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'DestinationRoot nie moze byc reparse pointem.'
    }
}
if ($destinationFull.Equals($sourceRoot, [StringComparison]::OrdinalIgnoreCase) -or
    (Test-PathWithin -Parent $sourceRoot -Child $destinationFull) -or
    (Test-PathWithin -Parent $destinationFull -Child $sourceRoot)) {
    throw 'Zrodlo i katalog docelowy nie moga sie pokrywac ani zawierac nawzajem.'
}

if ([string]::IsNullOrWhiteSpace($BackupName)) {
    $BackupName = 'RDR2-saves-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
}
if ($BackupName.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 -or
    $BackupName.Contains('\') -or $BackupName.Contains('/')) {
    throw 'BackupName musi byc pojedyncza, bezpieczna nazwa katalogu.'
}

$backupPath = [IO.Path]::GetFullPath((Join-Path $destinationFull $BackupName))
if (-not (Test-PathWithin -Parent $destinationFull -Child $backupPath)) {
    throw 'BackupName wskazuje poza DestinationRoot.'
}
if (Test-Path -LiteralPath $backupPath) {
    throw 'Katalog backupu juz istnieje; skrypt odmawia nadpisania.'
}

$maxTotalBytes = [int64]$MaxTotalSizeMiB * 1MB
$inventory = Get-SafeFileInventory -Root $sourceRoot -FileLimit $MaxFiles -ByteLimit $maxTotalBytes
if ($inventory.Files.Count -eq 0) {
    throw 'W SourcePath nie znaleziono zadnych plikow save.'
}

if (-not $PSCmdlet.ShouldProcess($backupPath, ('Utworzenie copy-only backupu ' + $inventory.Files.Count + ' plikow'))) {
    Write-Output ('Dry-run: pliki=' + $inventory.Files.Count + ', bajty=' + $inventory.TotalBytes +
        ', katalog=' + $backupPath)
    return
}

if (-not (Test-Path -LiteralPath $destinationFull -PathType Container)) {
    $null = New-Item -ItemType Directory -Path $destinationFull
}
if (Test-Path -LiteralPath $backupPath) {
    throw 'Katalog backupu pojawil sie w trakcie operacji; skrypt odmawia nadpisania.'
}
$null = New-Item -ItemType Directory -Path $backupPath

$manifestEntries = New-Object 'System.Collections.Generic.List[object]'
foreach ($file in $inventory.Files) {
    $relativeNative = $file.RelativePath.Replace('/', [IO.Path]::DirectorySeparatorChar)
    $targetPath = [IO.Path]::GetFullPath((Join-Path $backupPath $relativeNative))
    if (-not (Test-PathWithin -Parent $backupPath -Child $targetPath)) {
        throw 'Wykryto niebezpieczna sciezke wzgledna w zrodle.'
    }
    if (Test-Path -LiteralPath $targetPath) {
        throw ('Plik docelowy juz istnieje: ' + $file.RelativePath)
    }

    $targetDirectory = Split-Path -Parent $targetPath
    if (-not (Test-Path -LiteralPath $targetDirectory -PathType Container)) {
        $null = New-Item -ItemType Directory -Path $targetDirectory
    }

    $sourceHash = Get-FileSha256 -Path $file.SourcePath
    Copy-FileExclusive -Source $file.SourcePath -Destination $targetPath
    $targetHash = Get-FileSha256 -Path $targetPath
    if ($targetHash -ne $sourceHash) {
        throw ('Weryfikacja kopii nie powiodla sie dla: ' + $file.RelativePath)
    }

    $manifestEntries.Add([pscustomobject]@{
        RelativePath = $file.RelativePath
        Length = $file.Length
        LastWriteUtc = $file.LastWriteUtc.ToString('o')
        Sha256 = $sourceHash
    })
}

$contentSet = ($manifestEntries |
    Sort-Object RelativePath |
    ForEach-Object { $_.RelativePath + '|' + $_.Length + '|' + $_.Sha256 }) -join "`n"
$manifest = [ordered]@{
    SchemaVersion = 1
    CreatedAtUtc = [DateTime]::UtcNow.ToString('o')
    SourceFingerprint = (Get-StringSha256 -Value ($sourceRoot.ToLowerInvariant()))
    FileCount = $manifestEntries.Count
    TotalBytes = [int64]$inventory.TotalBytes
    ContentSetSha256 = (Get-StringSha256 -Value $contentSet)
    Files = @($manifestEntries | ForEach-Object { $_ })
}

$manifestPath = Join-Path $backupPath 'backup-manifest.json'
if (Test-Path -LiteralPath $manifestPath) {
    throw 'Manifest backupu juz istnieje; skrypt odmawia nadpisania.'
}
$manifestJson = $manifest | ConvertTo-Json -Depth 7
$utf8NoBom = New-Object Text.UTF8Encoding($false)
$stream = [IO.File]::Open(
    $manifestPath,
    [IO.FileMode]::CreateNew,
    [IO.FileAccess]::Write,
    [IO.FileShare]::None
)
try {
    $writer = New-Object IO.StreamWriter($stream, $utf8NoBom)
    try {
        $writer.Write($manifestJson)
    }
    finally {
        $writer.Dispose()
    }
}
finally {
    $stream.Dispose()
}

Write-Output ('Backup gotowy: ' + $backupPath)
Write-Output ('Pliki: ' + $manifestEntries.Count + '; bajty: ' + $inventory.TotalBytes +
    '; hash zestawu: ' + $manifest.ContentSetSha256)
