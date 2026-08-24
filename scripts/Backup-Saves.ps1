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
                throw 'The source contains a reparse point; backup was stopped to avoid leaving the selected directory.'
            }

            if ($item.PSIsContainer) {
                $pending.Enqueue($item.FullName)
                continue
            }

            if (-not $item.FullName.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'A file outside the source directory was detected.'
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
                throw ('The source exceeds the limit of ' + $FileLimit + ' files.')
            }
            if ($totalBytes -gt $ByteLimit) {
                throw ('The source exceeds the limit of ' + $MaxTotalSizeMiB + ' MiB.')
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
    throw 'SourcePath does not exist or is not a directory.'
}
$sourceItem = Get-Item -LiteralPath $sourceRoot -Force
if (($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'SourcePath cannot be a reparse point.'
}

if (-not [IO.Path]::IsPathRooted($DestinationRoot)) {
    throw 'DestinationRoot must be an explicit absolute path.'
}
$destinationFull = [IO.Path]::GetFullPath($DestinationRoot).TrimEnd('\', '/')
if (Test-Path -LiteralPath $destinationFull) {
    $destinationItem = Get-Item -LiteralPath $destinationFull -Force
    if (-not $destinationItem.PSIsContainer) {
        throw 'DestinationRoot exists but is not a directory.'
    }
    if (($destinationItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'DestinationRoot cannot be a reparse point.'
    }
}
if ($destinationFull.Equals($sourceRoot, [StringComparison]::OrdinalIgnoreCase) -or
    (Test-PathWithin -Parent $sourceRoot -Child $destinationFull) -or
    (Test-PathWithin -Parent $destinationFull -Child $sourceRoot)) {
    throw 'The source and destination directories cannot overlap or contain one another.'
}

if ([string]::IsNullOrWhiteSpace($BackupName)) {
    $BackupName = 'RDR2-saves-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
}
if ($BackupName.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 -or
    $BackupName.Contains('\') -or $BackupName.Contains('/')) {
    throw 'BackupName must be a single safe directory name.'
}

$backupPath = [IO.Path]::GetFullPath((Join-Path $destinationFull $BackupName))
if (-not (Test-PathWithin -Parent $destinationFull -Child $backupPath)) {
    throw 'BackupName points outside DestinationRoot.'
}
if (Test-Path -LiteralPath $backupPath) {
    throw 'The backup directory already exists; the script refuses to overwrite it.'
}

$maxTotalBytes = [int64]$MaxTotalSizeMiB * 1MB
$inventory = Get-SafeFileInventory -Root $sourceRoot -FileLimit $MaxFiles -ByteLimit $maxTotalBytes
if ($inventory.Files.Count -eq 0) {
    throw 'No save files were found in SourcePath.'
}

if (-not $PSCmdlet.ShouldProcess($backupPath, ('Create a copy-only backup of ' + $inventory.Files.Count + ' files'))) {
    Write-Output ('Dry-run: files=' + $inventory.Files.Count + ', bytes=' + $inventory.TotalBytes +
        ', directory=' + $backupPath)
    return
}

if (-not (Test-Path -LiteralPath $destinationFull -PathType Container)) {
    $null = New-Item -ItemType Directory -Path $destinationFull
}
if (Test-Path -LiteralPath $backupPath) {
    throw 'The backup directory appeared during the operation; the script refuses to overwrite it.'
}
$null = New-Item -ItemType Directory -Path $backupPath

$manifestEntries = New-Object 'System.Collections.Generic.List[object]'
foreach ($file in $inventory.Files) {
    $relativeNative = $file.RelativePath.Replace('/', [IO.Path]::DirectorySeparatorChar)
    $targetPath = [IO.Path]::GetFullPath((Join-Path $backupPath $relativeNative))
    if (-not (Test-PathWithin -Parent $backupPath -Child $targetPath)) {
        throw 'An unsafe relative path was detected in the source.'
    }
    if (Test-Path -LiteralPath $targetPath) {
        throw ('The destination file already exists: ' + $file.RelativePath)
    }

    $targetDirectory = Split-Path -Parent $targetPath
    if (-not (Test-Path -LiteralPath $targetDirectory -PathType Container)) {
        $null = New-Item -ItemType Directory -Path $targetDirectory
    }

    $sourceHash = Get-FileSha256 -Path $file.SourcePath
    Copy-FileExclusive -Source $file.SourcePath -Destination $targetPath
    $targetHash = Get-FileSha256 -Path $targetPath
    if ($targetHash -ne $sourceHash) {
        throw ('Copy verification failed for: ' + $file.RelativePath)
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
    throw 'The backup manifest already exists; the script refuses to overwrite it.'
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

Write-Output ('Backup ready: ' + $backupPath)
Write-Output ('Files: ' + $manifestEntries.Count + '; bytes: ' + $inventory.TotalBytes +
    '; hash zestawu: ' + $manifest.ContentSetSha256)
