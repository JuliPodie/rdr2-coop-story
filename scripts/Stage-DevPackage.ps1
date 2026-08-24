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
            throw 'The bridge ASI is too small to be a valid PE image.'
        }
        $reader = New-Object IO.BinaryReader($stream)
        try {
            if ($reader.ReadUInt16() -ne 0x5A4D) {
                throw 'The bridge ASI has no MZ signature.'
            }
            $stream.Position = 0x3C
            $peOffset = $reader.ReadInt32()
            if ($peOffset -lt 0x40 -or ($peOffset + 26) -gt $stream.Length) {
                throw 'The bridge ASI has an invalid PE header offset.'
            }
            $stream.Position = $peOffset
            if ($reader.ReadUInt32() -ne 0x00004550) {
                throw 'The bridge ASI has no PE signature.'
            }
            $machine = $reader.ReadUInt16()
            if ($machine -ne 0x8664) {
                throw 'The bridge ASI is not an x64 binary.'
            }
            $stream.Position = $peOffset + 20
            $optionalHeaderSize = $reader.ReadUInt16()
            $characteristics = $reader.ReadUInt16()
            if (($characteristics -band 0x2000) -eq 0) {
                throw 'The bridge ASI does not have the DLL characteristic.'
            }
            if ($optionalHeaderSize -lt 2) {
                throw 'The bridge ASI has no optional PE header.'
            }
            $optionalHeaderMagic = $reader.ReadUInt16()
            if ($optionalHeaderMagic -ne 0x020B) {
                throw 'The bridge ASI is not a PE32+ image.'
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
                throw 'Staging contains a reparse point; the operation was stopped.'
            }
            $items.Add($item)
            if ($item.PSIsContainer) {
                $pending.Enqueue($item.FullName)
            }
            else {
                $fileCount++
                $totalBytes += [int64]$item.Length
                if ($fileCount -gt $MaxFiles -or $totalBytes -gt $MaxBytes) {
                    throw 'Staging exceeds the safe file count or size limit.'
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

if (-not [IO.Path]::IsPathRooted($BridgePath)) {
    throw 'BridgePath must be an explicit absolute path.'
}
$bridge = [IO.Path]::GetFullPath($BridgePath)
if (-not (Test-PathWithin -Parent $workspace -Child $bridge)) {
    throw 'BridgePath must be inside WorkspaceRoot.'
}
if (-not (Test-Path -LiteralPath $bridge -PathType Leaf)) {
    throw 'BridgePath does not point to an existing file.'
}
$bridgeItem = Get-Item -LiteralPath $bridge -Force
if (($bridgeItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'BridgePath cannot be a reparse point.'
}
if (-not $bridgeItem.Name.Equals('CoopStoryBridge.asi', [StringComparison]::Ordinal)) {
    throw 'The bridge must be named exactly CoopStoryBridge.asi.'
}
Assert-PeX64Dll -Path $bridge

if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
    $ConfigPath = 'src\CoopStory.Sidecar\sidecar.config.example.json'
}
$config = Get-WorkspacePath -Workspace $workspace -Value $ConfigPath
if (-not (Test-PathWithin -Parent $workspace -Child $config) -or
    -not (Test-Path -LiteralPath $config -PathType Leaf)) {
    throw 'ConfigPath must point to an existing JSON file inside WorkspaceRoot.'
}
$configItem = Get-Item -LiteralPath $config -Force
if (($configItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'ConfigPath cannot be a reparse point.'
}
try {
    $configObject = Get-Content -LiteralPath $config -Raw | ConvertFrom-Json
}
catch {
    throw 'ConfigPath is not a valid JSON document.'
}
$safety = Get-PropertyValue -Object $configObject -Name 'safety'
if ((Get-PropertyValue -Object $configObject -Name 'schemaVersion') -ne 1 -or
    $null -eq $safety -or
    (Get-PropertyValue -Object $safety -Name 'storyModeOnly') -ne $true -or
    (Get-PropertyValue -Object $safety -Name 'refuseOnlineMode') -ne $true) {
    throw 'Config must use schemaVersion=1 and both required Story Mode guards.'
}

if ([string]::IsNullOrWhiteSpace($SidecarProjectPath)) {
    $SidecarProjectPath = 'src\CoopStory.Sidecar\CoopStory.Sidecar.csproj'
}
$sidecarProject = Get-WorkspacePath -Workspace $workspace -Value $SidecarProjectPath
if (-not (Test-PathWithin -Parent $workspace -Child $sidecarProject) -or
    -not (Test-Path -LiteralPath $sidecarProject -PathType Leaf)) {
    throw 'SidecarProjectPath must point to an existing project inside WorkspaceRoot.'
}
$projectItem = Get-Item -LiteralPath $sidecarProject -Force
if (($projectItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'SidecarProjectPath cannot be a reparse point.'
}
$assetsPath = Join-Path (Split-Path -Parent $sidecarProject) 'obj\project.assets.json'
if (-not (Test-Path -LiteralPath $assetsPath -PathType Leaf)) {
    throw 'project.assets.json is missing. Run an explicit restore outside this script; staging never restores.'
}

$bridgeSourceRoot = Join-Path $workspace 'src\CoopStory.Bridge'
if (-not (Test-Path -LiteralPath $bridgeSourceRoot -PathType Container)) {
    throw 'Bridge sources were not found for freshness verification.'
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
    throw 'CoopStoryBridge.asi is older than the sources/build config. A new warning-clean build is required.'
}

$dotnet = Get-Command dotnet.exe -ErrorAction SilentlyContinue
if ($null -eq $dotnet) {
    throw 'dotnet.exe was not found.'
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
    throw 'dist already exists; Stage-DevPackage refuses to overwrite or merge directories.'
}

Write-Output 'Plan stagingu:'
Write-Output '  validate fresh PE32+ x64 DLL: CoopStoryBridge.asi'
Write-Output '  dotnet publish: Release, framework-dependent, --no-restore'
Write-Output '  copy config example with Story Mode/RDO guards'
Write-Output '  reject ScriptHookRDR2.dll, dinput8.dll and NativeTrainer.asi'
Write-Output '  create a new workspace\dist; never overwrite or delete'

if (-not $Apply) {
    Write-Output 'Dry run complete. Use -Apply after a warning-clean build to create dist.'
    return
}
if (-not $PSCmdlet.ShouldProcess($dist, 'Utworzenie nowego framework-dependent dist')) {
    Write-Output 'Dry-run/WhatIf: staging and dist were not created.'
    return
}

$stagingRoot = Join-Path $workspace 'artifacts\staging'
if (-not (Test-Path -LiteralPath $stagingRoot -PathType Container)) {
    $null = [IO.Directory]::CreateDirectory($stagingRoot)
}
$stagingRootItem = Get-Item -LiteralPath $stagingRoot -Force
if (($stagingRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'artifacts\staging cannot be a reparse point.'
}

$stageName = 'dist-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-' +
    [Guid]::NewGuid().ToString('N')
$stage = Join-Path $stagingRoot $stageName
if (Test-Path -LiteralPath $stage) {
    throw 'The unique staging directory already exists.'
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
    throw ('dotnet publish --no-restore exited with code ' + $publishExitCode +
        '. Partial staging remains for inspection; the script removes nothing.')
}
$warningLines = @($publishOutput | Where-Object {
    $_ -match '(?i)(:\s*warning\s+[A-Z]{2,}\d+|\bwarning\s+[A-Z]{2,}\d+\s*:)'
})
if ($warningLines.Count -gt 0) {
    throw 'dotnet publish emitted a warning; dist will not be created.'
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
        throw 'Framework-dependent staging does not contain all required artifacts.'
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
    throw 'The sidecar framework-dependent runtimeconfig could not be verified.'
}
if ($frameworkName -ne 'Microsoft.NETCore.App') {
    throw 'The sidecar publish does not declare framework-dependent Microsoft.NETCore.App.'
}

$packageItems = @(Get-SafeTreeItems -Root $stage)
$forbiddenNames = @('ScriptHookRDR2.dll', 'dinput8.dll', 'NativeTrainer.asi')
$forbidden = @($packageItems | Where-Object {
    (-not $_.PSIsContainer) -and ($forbiddenNames -contains $_.Name)
})
if ($forbidden.Count -gt 0) {
    throw 'Staging contains a forbidden Script Hook runtime/loader/trainer.'
}
$bundledRuntimeNames = @('hostfxr.dll', 'hostpolicy.dll', 'coreclr.dll')
if (@($packageItems | Where-Object {
    (-not $_.PSIsContainer) -and ($bundledRuntimeNames -contains $_.Name)
}).Count -gt 0) {
    throw 'The sidecar contains a self-contained runtime; a framework-dependent publish is required.'
}

$topLevelNames = @(Get-ChildItem -LiteralPath $stage -Force | ForEach-Object { $_.Name })
foreach ($requiredTopLevel in @('CoopStoryBridge.asi', 'config', 'sidecar')) {
    if ($topLevelNames -notcontains $requiredTopLevel) {
        throw 'Staging has an invalid top-level structure.'
    }
}
if ($topLevelNames.Count -ne 3) {
    throw 'Staging contains unexpected top-level entries.'
}
if ((Get-FileSha256 -Path $bridgeStage) -ne (Get-FileSha256 -Path $bridge)) {
    throw 'The bridge hash in staging does not match the source.'
}
if ((Get-FileSha256 -Path $configStage) -ne (Get-FileSha256 -Path $config)) {
    throw 'The config hash in staging does not match the source.'
}

if (Test-Path -LiteralPath $dist) {
    throw 'dist appeared during staging; the script refuses to overwrite it.'
}

# Directory.Move on the same workspace volume creates dist atomically and
# fails if the destination appears. It neither merges nor deletes content.
[IO.Directory]::Move($stage, $dist)

Write-Output ('dist created: ' + $dist)
Write-Output ('Bridge SHA-256: ' + (Get-FileSha256 -Path (Join-Path $dist 'CoopStoryBridge.asi')))
Write-Output 'ScriptHook/runtime/trainer: not bundled'
