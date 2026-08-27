#Requires -Version 5.1

<#
.SYNOPSIS
Builds, stages, and installs a local development test of RDR2 Coop Story.

.DESCRIPTION
Runs the managed and native test suites, creates a fresh dist package, then
replaces only the project-owned files recorded by the development manifest.
It never copies Script Hook or a trainer from dist. RDR2 must be closed.

.EXAMPLE
.\scripts\Refresh-DevTest.ps1 -GamePath 'D:\Games\Red Dead Redemption 2'

.EXAMPLE
.\scripts\Refresh-DevTest.ps1 -GamePath 'D:\Games\Red Dead Redemption 2' `
    -SdkPath 'D:\Tools\ScriptHookRDR2_SDK_1.0.1207.73' -Launch
#>
[CmdletBinding()]
param(
    [string]$GamePath = "D:\Program Files (x86)\Neuer Ordner\steamapps\common\Red Dead Redemption 2",

    [ValidateSet('bridge-asi-vs2026', 'bridge-asi-vs2022')]
    [string]$BridgePreset = 'bridge-asi-vs2026',

    [string]$SdkPath = ".\ScriptHookRDR2_SDK_1.0.1207.73",

    [switch]$SkipNativeBuild,

    [switch]$Launch
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$workspace = [IO.Path]::GetFullPath(
    (Split-Path -Parent $PSScriptRoot)).TrimEnd('\', '/')
$gameRoot = [IO.Path]::GetFullPath($GamePath).TrimEnd('\', '/')
$gameExe = Join-Path $gameRoot 'RDR2.exe'
$manifest = Join-Path $workspace 'artifacts\deploy\deployment-manifest.json'
$dist = Join-Path $workspace 'dist'

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$Description,
        [Parameter(Mandatory = $true)][scriptblock]$Command
    )

    Write-Host ''
    Write-Host ('=== ' + $Description + ' ===') -ForegroundColor Cyan
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE."
    }
}

if (-not (Test-Path -LiteralPath $gameExe -PathType Leaf)) {
    throw "RDR2.exe was not found in GamePath: $gameRoot"
}
if (@(Get-Process -Name RDR2 -ErrorAction SilentlyContinue).Count -gt 0) {
    throw 'RDR2 is running. Close it before refreshing the development test.'
}
if (@(Get-Process -Name CoopStory.Sidecar -ErrorAction SilentlyContinue).Count -gt 0) {
    throw 'CoopStory.Sidecar is running. Stop it before refreshing the development test.'
}
$orphanedProjectFiles = New-Object 'System.Collections.Generic.List[string]'
foreach ($projectFileName in @('CoopStoryBridge.asi', 'CoopStory.config.json')) {
    $projectFilePath = Join-Path $gameRoot $projectFileName
    if (Test-Path -LiteralPath $projectFilePath -PathType Leaf) {
        $orphanedProjectFiles.Add($projectFilePath)
    }
}
if (-not (Test-Path -LiteralPath $manifest -PathType Leaf) -and
    $orphanedProjectFiles.Count -gt 0) {
    throw @"
Existing project files were found in the game directory, but the development
deployment manifest is missing. The refresh script will not overwrite files it
cannot prove it owns. If these were installed through the launcher, use its
Uninstall action first. Otherwise, move the listed files to a backup folder
manually, then run this script again:
$($orphanedProjectFiles -join [Environment]::NewLine)
"@
}

Invoke-Checked 'Managed build' {
    dotnet build (Join-Path $workspace 'CoopStory.slnx') -c Release
}
Invoke-Checked 'Managed self-tests' {
    dotnet run --project (Join-Path $workspace 'tests\CoopStory.SelfTest') -c Release
}
Invoke-Checked 'Launcher self-tests' {
    dotnet run --project (Join-Path $workspace 'tests\CoopStory.Launcher.SelfTest') -c Release
}

if (-not $SkipNativeBuild) {
    if ([string]::IsNullOrWhiteSpace($SdkPath)) {
        $SdkPath = [Environment]::GetEnvironmentVariable(
            'SCRIPT_HOOK_RDR2_SDK_DIR',
            'Process')
    }
    if ([string]::IsNullOrWhiteSpace($SdkPath)) {
        $workspaceSdk = Join-Path $workspace 'ScriptHookRDR2_SDK_1.0.1207.73'
        if (Test-Path -LiteralPath $workspaceSdk -PathType Container) {
            $SdkPath = $workspaceSdk
        }
    }
    if ([string]::IsNullOrWhiteSpace($SdkPath) -or
        -not (Test-Path -LiteralPath $SdkPath -PathType Container)) {
        throw @"
The Script Hook SDK is required for the ASI build. Supply its extracted root
with -SdkPath, or set SCRIPT_HOOK_RDR2_SDK_DIR before running this script.
"@
    }
    $env:SCRIPT_HOOK_RDR2_SDK_DIR =
        [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $SdkPath).Path)

    Invoke-Checked "Configure $BridgePreset" {
        # The include/library lookup variables are cached by CMake. Clear them
        # before configuring so a changed SDK path cannot retain an older SDK
        # location and fail later at the C++ compile step.
        cmake --preset $BridgePreset `
            -U COOPSTORY_SCRIPT_HOOK_INCLUDE_DIR `
            -U COOPSTORY_SCRIPT_HOOK_LIBRARY
    }
    Invoke-Checked "Build $BridgePreset" {
        cmake --build --preset ($BridgePreset + '-release')
    }
    Invoke-Checked "Test $BridgePreset" {
        ctest --preset ($BridgePreset + '-release')
    }
}

$bridge = Join-Path $workspace (
    'build\' + $BridgePreset + '\src\CoopStory.Bridge\Release\CoopStoryBridge.asi')
if (-not (Test-Path -LiteralPath $bridge -PathType Leaf)) {
    throw "The built ASI was not found: $bridge"
}

if (Test-Path -LiteralPath $dist) {
    $distFull = [IO.Path]::GetFullPath($dist).TrimEnd('\', '/')
    $workspacePrefix = $workspace.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $distFull.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase) -or
        $distFull.Equals($workspace, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to delete a dist directory outside the workspace.'
    }
    $distItem = Get-Item -LiteralPath $distFull -Force
    if (($distItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'Refusing to delete dist because it is a symbolic link or junction.'
    }
    Write-Host ''
    Write-Host 'Removing the previous dist package' -ForegroundColor Yellow
    Remove-Item -LiteralPath $distFull -Recurse -Force
}

Invoke-Checked 'Stage new dist package' {
    & (Join-Path $workspace 'scripts\Stage-DevPackage.ps1') `
        -BridgePath $bridge `
        -WorkspaceRoot $workspace `
        -Apply
}

if (Test-Path -LiteralPath $manifest -PathType Leaf) {
    Invoke-Checked 'Uninstall previous project-owned test files' {
        & (Join-Path $workspace 'scripts\Uninstall-DevBuild.ps1') `
            -GamePath $gameRoot `
            -WorkspaceRoot $workspace `
            -Apply
    }
}

Invoke-Checked 'Install refreshed project-owned test files' {
    & (Join-Path $workspace 'scripts\Install-DevBuild.ps1') `
        -GamePath $gameRoot `
        -WorkspaceRoot $workspace `
        -Apply
}

# Install-DevBuild deliberately handles only project-owned files. The checked
# local Script Hook runtime is a separate prerequisite, so use the existing
# installer to verify or install it after the bridge/config update. It never
# takes runtime files from dist and never installs NativeTrainer.asi.
Invoke-Checked 'Verify/install local Script Hook runtime' {
    & (Join-Path $workspace 'scripts\Easy-CoopStory.ps1') `
        -Action Install `
        -GamePath $gameRoot `
        -Yes
}

Write-Host ''
Write-Host 'Refresh complete. Script Hook remains separate and was not copied.' -ForegroundColor Green
Write-Host 'Use Story Mode only; uninstall project files before entering RDO.' -ForegroundColor Yellow

if ($Launch) {
    Invoke-Checked 'Launch local Story Mode test' {
        & (Join-Path $workspace 'scripts\Easy-CoopStory.ps1') `
            -Action Launch `
            -GamePath $gameRoot
    }
}
