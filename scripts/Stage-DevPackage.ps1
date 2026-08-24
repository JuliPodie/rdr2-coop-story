#Requires -Version 5.1

[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Low')]
param(
    [Parameter(Mandatory = $true)]
    [string]$BridgePath,

    [string]$WorkspaceRoot,

    [string]$ConfigPath,

    [string]$SidecarProjectPath,

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

    $parentFull = [IO.Path]::GetFullPath($Parent).TrimEnd('\', '/') +
        [IO.Path]::DirectorySeparatorChar
    $childFull = [IO.Path]::GetFullPath($Child)
    return $childFull.StartsWith($parentFull, [StringComparison]::OrdinalIgnoreCase)
}

function Get-WorkspacePath {
    param(
        [Parameter(Mandatory = $true)][string]$Workspace,
        [Parameter(Mandatory = $true)][string]$Value
    )

    if ([IO.Path]::IsPathRooted($Value)) {
        return [IO.Path]::GetFullPath($Value)
    }
    return [IO.Path]::GetFullPath((Join-Path $Workspace $Value))
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

function Copy-FileExclusive {
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
        }
        finally {
            $outputStream.Dispose()
        }
    }
    finally {
        $inputStream.Dispose()
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

function Assert-PeX64Dll {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = [IO.File]::Open(
        $Path,
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::Read
    )
    try {
        if ($stream.Length -lt 512) {
            throw 'Bridge ASI jest zbyt maly, aby byc poprawnym obrazem PE.'
        }
        $reader = New-Object IO.BinaryReader($stream)
        try {
            if ($reader.ReadUInt16() -ne 0x5A4D) {
                throw 'Bridge ASI nie ma sygnatury MZ.'
            }
            $stream.Position = 0x3C
            $peOffset = $reader.ReadInt32()
            if ($peOffset -lt 0x40 -or ($peOffset + 26) -gt $stream.Length) {
                throw 'Bridge ASI ma nieprawidlowy offset naglowka PE.'
            }
            $stream.Position = $peOffset
            if ($reader.ReadUInt32() -ne 0x00004550) {
                throw 'Bridge ASI nie ma sygnatury PE.'
            }
            $machine = $reader.ReadUInt16()
            if ($machine -ne 0x8664) {
                throw 'Bridge ASI nie jest binarium x64.'
            }
            $stream.Position = $peOffset + 20
            $optionalHeaderSize = $reader.ReadUInt16()
            $characteristics = $reader.ReadUInt16()
            if (($characteristics -band 0x2000) -eq 0) {
                throw 'Bridge ASI nie ma charakterystyki DLL.'
            }
            if ($optionalHeaderSize -lt 2) {
                throw 'Bridge ASI nie ma opcjonalnego naglowka PE.'
            }
            $optionalHeaderMagic = $reader.ReadUInt16()
            if ($optionalHeaderMagic -ne 0x020B) {
                throw 'Bridge ASI nie jest obrazem PE32+.'
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
                throw 'Staging zawiera reparse point; operacja zostala zatrzymana.'
            }
            $items.Add($item)
            if ($item.PSIsContainer) {
                $pending.Enqueue($item.FullName)
            }
            else {
                $fileCount++
                $totalBytes += [int64]$item.Length
                if ($fileCount -gt $MaxFiles -or $totalBytes -gt $MaxBytes) {
                    throw 'Staging przekracza bezpieczny limit liczby lub rozmiaru plikow.'
                }
            }
        }
    }

    return @($items | ForEach-Object { $_ })
}

$workspace = [IO.Path]::GetFullPath($WorkspaceRoot).TrimEnd('\', '/')
if (-not (Test-Path -LiteralPath $workspace -PathType Container)) {
    throw 'WorkspaceRoot nie istnieje lub nie jest katalogiem.'
}
$workspaceItem = Get-Item -LiteralPath $workspace -Force
if (($workspaceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'WorkspaceRoot nie moze byc reparse pointem.'
}

if (-not [IO.Path]::IsPathRooted($BridgePath)) {
    throw 'BridgePath musi byc jawna, bezwzgledna sciezka.'
}
$bridge = [IO.Path]::GetFullPath($BridgePath)
if (-not (Test-PathWithin -Parent $workspace -Child $bridge)) {
    throw 'BridgePath musi znajdowac sie wewnatrz WorkspaceRoot.'
}
if (-not (Test-Path -LiteralPath $bridge -PathType Leaf)) {
    throw 'BridgePath nie wskazuje istniejacego pliku.'
}
$bridgeItem = Get-Item -LiteralPath $bridge -Force
if (($bridgeItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'BridgePath nie moze byc reparse pointem.'
}
if (-not $bridgeItem.Name.Equals('CoopStoryBridge.asi', [StringComparison]::Ordinal)) {
    throw 'Bridge musi miec dokladna nazwe CoopStoryBridge.asi.'
}
Assert-PeX64Dll -Path $bridge

if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
    $ConfigPath = 'src\CoopStory.Sidecar\sidecar.config.example.json'
}
$config = Get-WorkspacePath -Workspace $workspace -Value $ConfigPath
if (-not (Test-PathWithin -Parent $workspace -Child $config) -or
    -not (Test-Path -LiteralPath $config -PathType Leaf)) {
    throw 'ConfigPath musi wskazywac istniejacy JSON wewnatrz WorkspaceRoot.'
}
$configItem = Get-Item -LiteralPath $config -Force
if (($configItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'ConfigPath nie moze byc reparse pointem.'
}
try {
    $configObject = Get-Content -LiteralPath $config -Raw | ConvertFrom-Json
}
catch {
    throw 'ConfigPath nie jest poprawnym dokumentem JSON.'
}
$safety = Get-PropertyValue -Object $configObject -Name 'safety'
if ((Get-PropertyValue -Object $configObject -Name 'schemaVersion') -ne 1 -or
    $null -eq $safety -or
    (Get-PropertyValue -Object $safety -Name 'storyModeOnly') -ne $true -or
    (Get-PropertyValue -Object $safety -Name 'refuseOnlineMode') -ne $true) {
    throw 'Config musi miec schemaVersion=1 i oba wymagane guardy Story Mode.'
}

if ([string]::IsNullOrWhiteSpace($SidecarProjectPath)) {
    $SidecarProjectPath = 'src\CoopStory.Sidecar\CoopStory.Sidecar.csproj'
}
$sidecarProject = Get-WorkspacePath -Workspace $workspace -Value $SidecarProjectPath
if (-not (Test-PathWithin -Parent $workspace -Child $sidecarProject) -or
    -not (Test-Path -LiteralPath $sidecarProject -PathType Leaf)) {
    throw 'SidecarProjectPath musi wskazywac istniejacy projekt wewnatrz WorkspaceRoot.'
}
$projectItem = Get-Item -LiteralPath $sidecarProject -Force
if (($projectItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'SidecarProjectPath nie moze byc reparse pointem.'
}
$assetsPath = Join-Path (Split-Path -Parent $sidecarProject) 'obj\project.assets.json'
if (-not (Test-Path -LiteralPath $assetsPath -PathType Leaf)) {
    throw 'Brak project.assets.json. Uruchom jawny restore poza tym skryptem; staging nigdy nie restore-uje.'
}

$bridgeSourceRoot = Join-Path $workspace 'src\CoopStory.Bridge'
if (-not (Test-Path -LiteralPath $bridgeSourceRoot -PathType Container)) {
    throw 'Nie znaleziono zrodel bridge do kontroli swiezosci.'
}
$bridgeSourceItems = @(Get-SafeTreeItems -Root $bridgeSourceRoot)
$buildInputs = @($bridgeSourceItems |
    Where-Object {
        (-not $_.PSIsContainer) -and
        (@('.cpp', '.c', '.hpp', '.h', '.txt') -contains $_.Extension.ToLowerInvariant())
    })
foreach ($rootBuildFile in @('CMakeLists.txt', 'CMakePresets.json')) {
    $candidate = Join-Path $workspace $rootBuildFile
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        $buildInputs += Get-Item -LiteralPath $candidate
    }
}
$newestInput = $buildInputs | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if ($null -eq $newestInput -or $bridgeItem.LastWriteTimeUtc -lt $newestInput.LastWriteTimeUtc) {
    throw 'CoopStoryBridge.asi jest starszy niz zrodla/build config. Wymagany jest nowy warning-clean build.'
}

$dotnet = Get-Command dotnet.exe -ErrorAction SilentlyContinue
if ($null -eq $dotnet) {
    throw 'Nie znaleziono dotnet.exe.'
}
$oldTelemetry = [Environment]::GetEnvironmentVariable('DOTNET_CLI_TELEMETRY_OPTOUT', 'Process')
$oldFirstTime = [Environment]::GetEnvironmentVariable('DOTNET_SKIP_FIRST_TIME_EXPERIENCE', 'Process')
$oldLanguage = [Environment]::GetEnvironmentVariable('DOTNET_CLI_UI_LANGUAGE', 'Process')
try {
    [Environment]::SetEnvironmentVariable('DOTNET_CLI_TELEMETRY_OPTOUT', '1', 'Process')
    [Environment]::SetEnvironmentVariable('DOTNET_SKIP_FIRST_TIME_EXPERIENCE', '1', 'Process')
    [Environment]::SetEnvironmentVariable('DOTNET_CLI_UI_LANGUAGE', 'en-US', 'Process')
    $dotnetVersion = (& $dotnet.Source --version 2>$null | Select-Object -First 1)
}
finally {
    [Environment]::SetEnvironmentVariable('DOTNET_CLI_TELEMETRY_OPTOUT', $oldTelemetry, 'Process')
    [Environment]::SetEnvironmentVariable('DOTNET_SKIP_FIRST_TIME_EXPERIENCE', $oldFirstTime, 'Process')
    [Environment]::SetEnvironmentVariable('DOTNET_CLI_UI_LANGUAGE', $oldLanguage, 'Process')
}
if ([string]::IsNullOrWhiteSpace($dotnetVersion) -or $dotnetVersion -notmatch '^10\.') {
    throw 'Staging wymaga .NET SDK 10.x.'
}

$dist = Join-Path $workspace 'dist'
if (Test-Path -LiteralPath $dist) {
    throw 'dist juz istnieje; Stage-DevPackage odmawia nadpisania lub laczenia katalogow.'
}

Write-Output 'Plan stagingu:'
Write-Output '  validate fresh PE32+ x64 DLL: CoopStoryBridge.asi'
Write-Output '  dotnet publish: Release, framework-dependent, --no-restore'
Write-Output '  copy config example with Story Mode/RDO guards'
Write-Output '  reject ScriptHookRDR2.dll, dinput8.dll and NativeTrainer.asi'
Write-Output '  create a new workspace\dist; never overwrite or delete'

if (-not $Apply) {
    Write-Output 'Dry-run zakonczony. Uzyj -Apply po warning-clean buildzie, aby utworzyc dist.'
    return
}
if (-not $PSCmdlet.ShouldProcess($dist, 'Utworzenie nowego framework-dependent dist')) {
    Write-Output 'Dry-run/WhatIf: nie utworzono stagingu ani dist.'
    return
}

$stagingRoot = Join-Path $workspace 'artifacts\staging'
if (-not (Test-Path -LiteralPath $stagingRoot -PathType Container)) {
    $null = [IO.Directory]::CreateDirectory($stagingRoot)
}
$stagingRootItem = Get-Item -LiteralPath $stagingRoot -Force
if (($stagingRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'artifacts\staging nie moze byc reparse pointem.'
}

$stageName = 'dist-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-' +
    [Guid]::NewGuid().ToString('N')
$stage = Join-Path $stagingRoot $stageName
if (Test-Path -LiteralPath $stage) {
    throw 'Unikalny katalog stagingu juz istnieje.'
}
$null = [IO.Directory]::CreateDirectory($stage)

$sidecarStage = Join-Path $stage 'sidecar'
$oldTelemetry = [Environment]::GetEnvironmentVariable('DOTNET_CLI_TELEMETRY_OPTOUT', 'Process')
$oldFirstTime = [Environment]::GetEnvironmentVariable('DOTNET_SKIP_FIRST_TIME_EXPERIENCE', 'Process')
$oldLanguage = [Environment]::GetEnvironmentVariable('DOTNET_CLI_UI_LANGUAGE', 'Process')
try {
    [Environment]::SetEnvironmentVariable('DOTNET_CLI_TELEMETRY_OPTOUT', '1', 'Process')
    [Environment]::SetEnvironmentVariable('DOTNET_SKIP_FIRST_TIME_EXPERIENCE', '1', 'Process')
    [Environment]::SetEnvironmentVariable('DOTNET_CLI_UI_LANGUAGE', 'en-US', 'Process')

    $publishOutput = New-Object 'System.Collections.Generic.List[string]'
    & $dotnet.Source publish $sidecarProject `
        -c Release `
        --no-restore `
        --self-contained false `
        -o $sidecarStage 2>&1 |
        ForEach-Object {
            $line = [string]$_
            $publishOutput.Add($line)
            Write-Output $line
        }
    $publishExitCode = $LASTEXITCODE
}
finally {
    [Environment]::SetEnvironmentVariable('DOTNET_CLI_TELEMETRY_OPTOUT', $oldTelemetry, 'Process')
    [Environment]::SetEnvironmentVariable('DOTNET_SKIP_FIRST_TIME_EXPERIENCE', $oldFirstTime, 'Process')
    [Environment]::SetEnvironmentVariable('DOTNET_CLI_UI_LANGUAGE', $oldLanguage, 'Process')
}

if ($publishExitCode -ne 0) {
    throw ('dotnet publish --no-restore zakonczyl sie kodem ' + $publishExitCode +
        '. Czesciowy staging pozostaje do inspekcji; skrypt niczego nie usuwa.')
}
$warningLines = @($publishOutput | Where-Object {
    $_ -match '(?i)(:\s*warning\s+[A-Z]{2,}\d+|\bwarning\s+[A-Z]{2,}\d+\s*:)'
})
if ($warningLines.Count -gt 0) {
    throw 'dotnet publish wyemitowal warning; dist nie zostanie utworzony.'
}

$bridgeStage = Join-Path $stage 'CoopStoryBridge.asi'
Copy-FileExclusive -Source $bridge -Destination $bridgeStage
$configDirectory = Join-Path $stage 'config'
$null = [IO.Directory]::CreateDirectory($configDirectory)
$configStage = Join-Path $configDirectory 'coopstory.example.json'
Copy-FileExclusive -Source $config -Destination $configStage

foreach ($required in @(
    $bridgeStage,
    $configStage,
    (Join-Path $sidecarStage 'CoopStory.Sidecar.exe'),
    (Join-Path $sidecarStage 'CoopStory.Sidecar.dll'),
    (Join-Path $sidecarStage 'CoopStory.Sidecar.deps.json'),
    (Join-Path $sidecarStage 'CoopStory.Sidecar.runtimeconfig.json')
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw 'Framework-dependent staging nie zawiera wszystkich wymaganych artefaktow.'
    }
}

$runtimeConfigPath = Join-Path $sidecarStage 'CoopStory.Sidecar.runtimeconfig.json'
try {
    $runtimeConfig = Get-Content -LiteralPath $runtimeConfigPath -Raw | ConvertFrom-Json
    $runtimeOptions = Get-PropertyValue -Object $runtimeConfig -Name 'runtimeOptions'
    $framework = Get-PropertyValue -Object $runtimeOptions -Name 'framework'
    $frameworkName = [string](Get-PropertyValue -Object $framework -Name 'name')
}
catch {
    throw 'Nie mozna potwierdzic framework-dependent runtimeconfig sidecara.'
}
if ($frameworkName -ne 'Microsoft.NETCore.App') {
    throw 'Sidecar publish nie deklaruje framework-dependent Microsoft.NETCore.App.'
}

$packageItems = @(Get-SafeTreeItems -Root $stage)
$forbiddenNames = @('ScriptHookRDR2.dll', 'dinput8.dll', 'NativeTrainer.asi')
$forbidden = @($packageItems | Where-Object {
    (-not $_.PSIsContainer) -and ($forbiddenNames -contains $_.Name)
})
if ($forbidden.Count -gt 0) {
    throw 'Staging zawiera zabroniony runtime ScriptHook/loader/trainer.'
}
$bundledRuntimeNames = @('hostfxr.dll', 'hostpolicy.dll', 'coreclr.dll')
if (@($packageItems | Where-Object {
    (-not $_.PSIsContainer) -and ($bundledRuntimeNames -contains $_.Name)
}).Count -gt 0) {
    throw 'Sidecar zawiera self-contained runtime; wymagany jest framework-dependent publish.'
}

$topLevelNames = @(Get-ChildItem -LiteralPath $stage -Force | ForEach-Object { $_.Name })
foreach ($requiredTopLevel in @('CoopStoryBridge.asi', 'config', 'sidecar')) {
    if ($topLevelNames -notcontains $requiredTopLevel) {
        throw 'Staging ma nieprawidlowa strukture top-level.'
    }
}
if ($topLevelNames.Count -ne 3) {
    throw 'Staging zawiera nieoczekiwane elementy top-level.'
}
if ((Get-FileSha256 -Path $bridgeStage) -ne (Get-FileSha256 -Path $bridge)) {
    throw 'Hash bridge w stagingu nie zgadza sie ze zrodlem.'
}
if ((Get-FileSha256 -Path $configStage) -ne (Get-FileSha256 -Path $config)) {
    throw 'Hash configu w stagingu nie zgadza sie ze zrodlem.'
}

if (Test-Path -LiteralPath $dist) {
    throw 'dist pojawil sie w trakcie stagingu; skrypt odmawia nadpisania.'
}

# Directory.Move on the same workspace volume creates dist atomically and
# fails if the destination appears. It neither merges nor deletes content.
[IO.Directory]::Move($stage, $dist)

Write-Output ('dist utworzony: ' + $dist)
Write-Output ('Bridge SHA-256: ' + (Get-FileSha256 -Path (Join-Path $dist 'CoopStoryBridge.asi')))
Write-Output 'ScriptHook/runtime/trainer: not bundled'
