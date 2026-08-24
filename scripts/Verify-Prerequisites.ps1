#Requires -Version 5.1

[CmdletBinding()]
param(
    [string]$WorkspaceRoot,
    [string]$GamePath,
    [string]$SdkPath,
    [string]$RuntimePath,
    [string]$ExpectedGameVersion = '1.0.1491.50',
    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string]$ExpectedGameSha256 = 'B56C9548F670654A9B73BF25DEF3CD73AF12E269F6E47DBA28A34079ADAF465E',
    [string]$ExpectedVisualStudioChannel = '17.14',
    [string]$ExpectedWindowsSdk = '10.0.26100.0',
    [switch]$Json
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$checks = New-Object 'System.Collections.Generic.List[object]'

if ([string]::IsNullOrWhiteSpace($WorkspaceRoot)) {
    $WorkspaceRoot = Split-Path -Parent $PSScriptRoot
}

function Add-Check {
    param(
        [Parameter(Mandatory = $true)][string]$Category,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][ValidateSet('PASS', 'WARN', 'SKIP', 'FAIL')][string]$Status,
        [Parameter(Mandatory = $true)][string]$Detail
    )

    $checks.Add([pscustomobject]@{
        Category = $Category
        Name = $Name
        Status = $Status
        Detail = $Detail
    })
}

function Get-NormalizedVersion {
    param([AllowNull()][string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $null
    }

    $match = [regex]::Match($Value, '\d+\.\d+\.\d+\.\d+')
    if ($match.Success) {
        return $match.Value
    }

    return $Value.Trim()
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

function Get-DefaultPackagePath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$DirectoryPattern,
        [Parameter(Mandatory = $true)][string]$ArchivePattern
    )

    $directory = Get-ChildItem -LiteralPath $Root -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like $DirectoryPattern } |
        Sort-Object Name -Descending |
        Select-Object -First 1
    if ($null -ne $directory) {
        return $directory.FullName
    }

    $incoming = Join-Path $Root 'prereqs\incoming'
    if (Test-Path -LiteralPath $incoming -PathType Container) {
        $archive = Get-ChildItem -LiteralPath $incoming -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like $ArchivePattern } |
            Sort-Object Name -Descending |
            Select-Object -First 1
        if ($null -ne $archive) {
            return $archive.FullName
        }
    }

    return $null
}

function Test-PackageEntries {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string[]]$RequiredEntries
    )

    if (Test-Path -LiteralPath $Path -PathType Container) {
        foreach ($entry in $RequiredEntries) {
            $nativeEntry = $entry.Replace('/', [IO.Path]::DirectorySeparatorChar)
            if (-not (Test-Path -LiteralPath (Join-Path $Path $nativeEntry) -PathType Leaf)) {
                return $false
            }
        }
        return $true
    }

    if ((Test-Path -LiteralPath $Path -PathType Leaf) -and
        [IO.Path]::GetExtension($Path).Equals('.zip', [StringComparison]::OrdinalIgnoreCase)) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = $null
        try {
            $archive = [IO.Compression.ZipFile]::OpenRead($Path)
            $entries = @{}
            foreach ($entry in $archive.Entries) {
                $normalized = $entry.FullName.Replace('\', '/').TrimStart('/').ToLowerInvariant()
                $entries[$normalized] = $true
            }

            foreach ($required in $RequiredEntries) {
                $needle = $required.Replace('\', '/').TrimStart('/').ToLowerInvariant()
                $found = $false
                foreach ($key in $entries.Keys) {
                    if ($key -eq $needle -or $key.EndsWith('/' + $needle, [StringComparison]::Ordinal)) {
                        $found = $true
                        break
                    }
                }
                if (-not $found) {
                    return $false
                }
            }
            return $true
        }
        finally {
            if ($null -ne $archive) {
                $archive.Dispose()
            }
        }
    }

    return $false
}

function Get-PackageEntryText {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$EntryName
    )

    if (Test-Path -LiteralPath $Path -PathType Container) {
        $entryPath = Join-Path $Path ($EntryName.Replace('/', [IO.Path]::DirectorySeparatorChar))
        if (Test-Path -LiteralPath $entryPath -PathType Leaf) {
            return [IO.File]::ReadAllText($entryPath)
        }
        return $null
    }

    if ((Test-Path -LiteralPath $Path -PathType Leaf) -and
        [IO.Path]::GetExtension($Path).Equals('.zip', [StringComparison]::OrdinalIgnoreCase)) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = $null
        try {
            $archive = [IO.Compression.ZipFile]::OpenRead($Path)
            $needle = $EntryName.Replace('\', '/').TrimStart('/').ToLowerInvariant()
            $entry = $archive.Entries |
                Where-Object {
                    $normalized = $_.FullName.Replace('\', '/').TrimStart('/').ToLowerInvariant()
                    $normalized -eq $needle -or
                        $normalized.EndsWith('/' + $needle, [StringComparison]::Ordinal)
                } |
                Select-Object -First 1
            if ($null -eq $entry) {
                return $null
            }
            $stream = $entry.Open()
            try {
                $reader = New-Object IO.StreamReader($stream)
                try {
                    return $reader.ReadToEnd()
                }
                finally {
                    $reader.Dispose()
                }
            }
            finally {
                $stream.Dispose()
            }
        }
        finally {
            if ($null -ne $archive) {
                $archive.Dispose()
            }
        }
    }

    return $null
}

function Find-SteamGamePath {
    $steamRoots = New-Object 'System.Collections.Generic.List[string]'
    try {
        $steamValue = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction Stop).SteamPath
        if (-not [string]::IsNullOrWhiteSpace($steamValue)) {
            $steamRoots.Add($steamValue)
        }
    }
    catch {
        # Steam may be installed without this per-user registry value.
    }

    if (-not [string]::IsNullOrWhiteSpace(${env:ProgramFiles(x86)})) {
        $defaultSteam = Join-Path ${env:ProgramFiles(x86)} 'Steam'
        if (-not $steamRoots.Contains($defaultSteam)) {
            $steamRoots.Add($defaultSteam)
        }
    }

    $libraryRoots = New-Object 'System.Collections.Generic.List[string]'
    foreach ($steamRoot in $steamRoots) {
        if (-not (Test-Path -LiteralPath $steamRoot -PathType Container)) {
            continue
        }

        if (-not $libraryRoots.Contains($steamRoot)) {
            $libraryRoots.Add($steamRoot)
        }

        $libraryFile = Join-Path $steamRoot 'steamapps\libraryfolders.vdf'
        if (-not (Test-Path -LiteralPath $libraryFile -PathType Leaf)) {
            continue
        }

        try {
            foreach ($line in [IO.File]::ReadLines($libraryFile)) {
                $match = [regex]::Match($line, '"path"\s+"([^"]+)"')
                if ($match.Success) {
                    $candidate = $match.Groups[1].Value.Replace('\\', '\')
                    if (-not $libraryRoots.Contains($candidate)) {
                        $libraryRoots.Add($candidate)
                    }
                }
            }
        }
        catch {
            # A locked or malformed Steam manifest is reported as game-not-detected.
        }
    }

    foreach ($libraryRoot in $libraryRoots) {
        $appManifest = Join-Path $libraryRoot 'steamapps\appmanifest_1174180.acf'
        if (-not (Test-Path -LiteralPath $appManifest -PathType Leaf)) {
            continue
        }

        try {
            $installDir = $null
            foreach ($line in [IO.File]::ReadLines($appManifest)) {
                $match = [regex]::Match($line, '"installdir"\s+"([^"]+)"')
                if ($match.Success) {
                    $installDir = $match.Groups[1].Value
                    break
                }
            }
            if (-not [string]::IsNullOrWhiteSpace($installDir)) {
                $candidate = Join-Path (Join-Path $libraryRoot 'steamapps\common') $installDir
                if (Test-Path -LiteralPath (Join-Path $candidate 'RDR2.exe') -PathType Leaf) {
                    return $candidate
                }
            }
        }
        catch {
            # Do not expose local manifest contents or paths in diagnostics.
        }
    }

    return $null
}

try {
    $workspaceFull = [IO.Path]::GetFullPath($WorkspaceRoot)
    if (-not (Test-Path -LiteralPath $workspaceFull -PathType Container)) {
        throw 'WorkspaceRoot does not exist.'
    }
}
catch {
    Add-Check -Category 'Workspace' -Name 'Workspace root' -Status 'FAIL' -Detail 'Nie mozna bezpiecznie rozpoznac katalogu projektu.'
    $workspaceFull = $null
}

# Visual Studio Build Tools and MSVC
$vsWhere = $null
if (-not [string]::IsNullOrWhiteSpace(${env:ProgramFiles(x86)})) {
    $candidateVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $candidateVsWhere -PathType Leaf) {
        $vsWhere = $candidateVsWhere
    }
}
if ($null -eq $vsWhere) {
    $vsWhereCommand = Get-Command vswhere.exe -ErrorAction SilentlyContinue
    if ($null -ne $vsWhereCommand) {
        $vsWhere = $vsWhereCommand.Source
    }
}

$plannedVsInstall = $null
$vsInstall = $null
if ($null -ne $vsWhere) {
    try {
        $versionRange = '[' + $ExpectedVisualStudioChannel + ',' +
            ([version]($ExpectedVisualStudioChannel + '.0')).Major + '.' +
            (([version]($ExpectedVisualStudioChannel + '.0')).Minor + 1) + ')'
        $plannedVsInstall = (& $vsWhere -latest -products '*' -version $versionRange `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null | Select-Object -First 1)
        $vsInstall = (& $vsWhere -latest -products '*' `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null | Select-Object -First 1)
    }
    catch {
        $plannedVsInstall = $null
        $vsInstall = $null
    }
}

if ([string]::IsNullOrWhiteSpace($vsInstall)) {
    Add-Check -Category 'Build' -Name 'Visual Studio C++ workload' `
        -Status 'FAIL' -Detail 'Nie znaleziono instalacji Visual Studio z komponentem C++ x64/x86.'
    Add-Check -Category 'Build' -Name ('Planned VS channel ' + $ExpectedVisualStudioChannel) `
        -Status 'WARN' -Detail 'Planowany kanal nie jest zainstalowany.'
    Add-Check -Category 'Build' -Name 'MSVC x64 compiler' -Status 'FAIL' `
        -Detail 'Nie mozna potwierdzic cl.exe bez instalacji Visual Studio C++.'

    $cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($null -ne $cmakeCommand) {
        Add-Check -Category 'Build' -Name 'CMake' -Status 'PASS' -Detail 'CMake jest dostepny w PATH.'
    }
    else {
        Add-Check -Category 'Build' -Name 'CMake' -Status 'FAIL' -Detail 'Nie znaleziono CMake.'
    }
}
else {
    Add-Check -Category 'Build' -Name 'Visual Studio C++ workload' `
        -Status 'PASS' -Detail 'Znaleziono instalacje Visual Studio z komponentem C++ x64/x86.'
    if ([string]::IsNullOrWhiteSpace($plannedVsInstall)) {
        Add-Check -Category 'Build' -Name ('Planned VS channel ' + $ExpectedVisualStudioChannel) `
            -Status 'WARN' -Detail 'Planowany kanal nie jest zainstalowany; uzywany jest nowszy toolchain.'
    }
    else {
        Add-Check -Category 'Build' -Name ('Planned VS channel ' + $ExpectedVisualStudioChannel) `
            -Status 'PASS' -Detail 'Planowany kanal z workloadem C++ jest dostepny.'
    }

    $msvcRoot = Join-Path $vsInstall 'VC\Tools\MSVC'
    $msvcDirectory = Get-ChildItem -LiteralPath $msvcRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        Where-Object {
            Test-Path -LiteralPath (Join-Path $_.FullName 'bin\Hostx64\x64\cl.exe') -PathType Leaf
        } |
        Select-Object -First 1
    if ($null -ne $msvcDirectory) {
        Add-Check -Category 'Build' -Name 'MSVC x64 compiler' -Status 'PASS' `
            -Detail ('Kompilator Hostx64/x64 jest dostepny; toolset ' + $msvcDirectory.Name + '.')
    }
    else {
        Add-Check -Category 'Build' -Name 'MSVC x64 compiler' -Status 'FAIL' `
            -Detail 'Nie znaleziono kompilatora cl.exe dla Hostx64/x64.'
    }

    $vsCmake = Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path -LiteralPath $vsCmake -PathType Leaf) {
        Add-Check -Category 'Build' -Name 'CMake' -Status 'PASS' -Detail 'CMake z Visual Studio jest dostepny.'
    }
    else {
        $cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
        if ($null -ne $cmakeCommand) {
            Add-Check -Category 'Build' -Name 'CMake' -Status 'PASS' -Detail 'CMake jest dostepny w PATH.'
        }
        else {
            Add-Check -Category 'Build' -Name 'CMake' -Status 'FAIL' -Detail 'Nie znaleziono CMake.'
        }
    }
}

# Windows SDK
$sdkRoot = $null
try {
    $sdkRoot = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' `
        -Name KitsRoot10 -ErrorAction Stop).KitsRoot10
}
catch {
    try {
        $sdkRoot = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots' `
            -Name KitsRoot10 -ErrorAction Stop).KitsRoot10
    }
    catch {
        $sdkRoot = $null
    }
}

if (-not [string]::IsNullOrWhiteSpace($sdkRoot)) {
    $windowsHeader = Join-Path $sdkRoot ('Include\' + $ExpectedWindowsSdk + '\um\Windows.h')
    $resourceCompiler = Join-Path $sdkRoot ('bin\' + $ExpectedWindowsSdk + '\x64\rc.exe')
    if ((Test-Path -LiteralPath $windowsHeader -PathType Leaf) -and
        (Test-Path -LiteralPath $resourceCompiler -PathType Leaf)) {
        Add-Check -Category 'Build' -Name ('Windows SDK ' + $ExpectedWindowsSdk) `
            -Status 'PASS' -Detail 'Naglowki i narzedzia x64 sa dostepne.'
    }
    else {
        Add-Check -Category 'Build' -Name ('Windows SDK ' + $ExpectedWindowsSdk) `
            -Status 'FAIL' -Detail 'Brakuje wymaganych naglowkow lub narzedzi x64.'
    }
}
else {
    Add-Check -Category 'Build' -Name ('Windows SDK ' + $ExpectedWindowsSdk) `
        -Status 'FAIL' -Detail 'Nie znaleziono Windows Kits.'
}

# .NET sidecar runtime/SDK
$dotnetCommand = Get-Command dotnet.exe -ErrorAction SilentlyContinue
if ($null -eq $dotnetCommand) {
    Add-Check -Category 'Build' -Name '.NET SDK 10' -Status 'FAIL' -Detail 'Nie znaleziono polecenia dotnet.'
}
else {
    try {
        $dotnetSdks = @(& $dotnetCommand.Source --list-sdks 2>$null)
        if (@($dotnetSdks | Where-Object { $_ -match '^10\.' }).Count -gt 0) {
            Add-Check -Category 'Build' -Name '.NET SDK 10' -Status 'PASS' -Detail 'SDK 10.x jest dostepny.'
        }
        else {
            Add-Check -Category 'Build' -Name '.NET SDK 10' -Status 'FAIL' -Detail 'dotnet dziala, ale brak SDK 10.x.'
        }
    }
    catch {
        Add-Check -Category 'Build' -Name '.NET SDK 10' -Status 'FAIL' -Detail 'Nie udalo sie odczytac listy SDK.'
    }
}

# ScriptHook SDK and runtime packages; archives are inspected in place, never extracted.
if ([string]::IsNullOrWhiteSpace($SdkPath) -and $null -ne $workspaceFull) {
    $SdkPath = Get-DefaultPackagePath -Root $workspaceFull `
        -DirectoryPattern 'ScriptHookRDR2_SDK_*' -ArchivePattern '*ScriptHookRDR2*SDK*.zip'
}
if ([string]::IsNullOrWhiteSpace($SdkPath)) {
    Add-Check -Category 'ScriptHook' -Name 'ScriptHookRDR2 SDK' -Status 'FAIL' -Detail 'Nie znaleziono SDK w workspace ani prereqs\incoming.'
}
else {
    try {
        $sdkOk = Test-PackageEntries -Path $SdkPath -RequiredEntries @(
            'inc/main.h',
            'inc/natives.h',
            'inc/types.h',
            'lib/ScriptHookRDR2.lib',
            'readme.txt'
        )
        $sdkReadme = Get-PackageEntryText -Path $SdkPath -EntryName 'readme.txt'
        if ($sdkOk -and $sdkReadme -match '(?i)v1\.0\.1207\.73') {
            Add-Check -Category 'ScriptHook' -Name 'ScriptHookRDR2 SDK' -Status 'PASS' -Detail 'Wymagane naglowki, biblioteka i warunki SDK sa obecne.'
        }
        else {
            Add-Check -Category 'ScriptHook' -Name 'ScriptHookRDR2 SDK' -Status 'FAIL' -Detail 'Pakiet nie zawiera kompletu plikow lub deklaracji wersji 1.0.1207.73.'
        }
    }
    catch {
        Add-Check -Category 'ScriptHook' -Name 'ScriptHookRDR2 SDK' -Status 'FAIL' -Detail 'Pakiet jest nieczytelny lub uszkodzony.'
    }
}

if ([string]::IsNullOrWhiteSpace($RuntimePath) -and $null -ne $workspaceFull) {
    $RuntimePath = Get-DefaultPackagePath -Root $workspaceFull `
        -DirectoryPattern 'ScriptHookRDR2_1.0.1491.17*' -ArchivePattern '*ScriptHookRDR2*1.0.1491.17*.zip'
}
if ([string]::IsNullOrWhiteSpace($RuntimePath)) {
    Add-Check -Category 'ScriptHook' -Name 'ScriptHookRDR2 runtime 1.0.1491.17' -Status 'FAIL' -Detail 'Nie znaleziono runtime w workspace ani prereqs\incoming.'
}
else {
    try {
        $runtimeOk = Test-PackageEntries -Path $RuntimePath -RequiredEntries @(
            'bin/ScriptHookRDR2.dll',
            'bin/dinput8.dll',
            'readme.txt'
        )
        $runtimeReadme = Get-PackageEntryText -Path $RuntimePath -EntryName 'readme.txt'
        if ($runtimeOk -and $runtimeReadme -match '(?i)v1\.0\.1491\.17') {
            Add-Check -Category 'ScriptHook' -Name 'ScriptHookRDR2 runtime 1.0.1491.17' -Status 'PASS' -Detail 'Wymagane pliki runtime sa obecne; installer projektu ich nie kopiuje.'
        }
        else {
            Add-Check -Category 'ScriptHook' -Name 'ScriptHookRDR2 runtime 1.0.1491.17' -Status 'FAIL' -Detail 'Pakiet nie zawiera kompletu plikow lub deklaracji wersji 1.0.1491.17.'
        }
    }
    catch {
        Add-Check -Category 'ScriptHook' -Name 'ScriptHookRDR2 runtime 1.0.1491.17' -Status 'FAIL' -Detail 'Pakiet jest nieczytelny lub uszkodzony.'
    }
}

# Game discovery is Steam-manifest based. No local path is printed.
if ([string]::IsNullOrWhiteSpace($GamePath)) {
    $GamePath = Find-SteamGamePath
}

if ([string]::IsNullOrWhiteSpace($GamePath)) {
    Add-Check -Category 'Game' -Name 'Steam manifest' -Status 'SKIP' -Detail 'Nie wykryto instalacji Steam; podaj -GamePath, jesli uzywasz innego launchera.'
    Add-Check -Category 'Game' -Name 'RDR2.exe version' -Status 'SKIP' -Detail 'Brak sciezki gry.'
    Add-Check -Category 'Game' -Name 'RDR2.exe SHA-256' -Status 'SKIP' -Detail 'Brak sciezki gry.'
}
else {
    try {
        $gameExe = Join-Path ([IO.Path]::GetFullPath($GamePath)) 'RDR2.exe'
        if (-not (Test-Path -LiteralPath $gameExe -PathType Leaf)) {
            throw 'RDR2.exe missing.'
        }

        Add-Check -Category 'Game' -Name 'Steam manifest / explicit game root' -Status 'PASS' -Detail 'Rozpoznano katalog zawierajacy RDR2.exe.'

        $versionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($gameExe)
        $actualVersion = Get-NormalizedVersion $versionInfo.ProductVersion
        if ($null -eq $actualVersion) {
            $actualVersion = Get-NormalizedVersion $versionInfo.FileVersion
        }
        if ($actualVersion -eq $ExpectedGameVersion) {
            Add-Check -Category 'Game' -Name 'RDR2.exe version' -Status 'PASS' -Detail ('Wersja zgodna: ' + $ExpectedGameVersion + '.')
        }
        else {
            Add-Check -Category 'Game' -Name 'RDR2.exe version' -Status 'FAIL' -Detail ('Oczekiwano ' + $ExpectedGameVersion + '; wykryta wersja jest nieobslugiwana.')
        }

        $actualHash = Get-FileSha256 -Path $gameExe
        if ($actualHash -eq $ExpectedGameSha256.ToUpperInvariant()) {
            Add-Check -Category 'Game' -Name 'RDR2.exe SHA-256' -Status 'PASS' -Detail 'Hash odpowiada obslugiwanemu buildowi.'
        }
        else {
            Add-Check -Category 'Game' -Name 'RDR2.exe SHA-256' -Status 'FAIL' -Detail 'Hash nie odpowiada obslugiwanemu buildowi; mod nie powinien byc uruchamiany.'
        }

        $runtimeInstalled = (Test-Path -LiteralPath (Join-Path $GamePath 'ScriptHookRDR2.dll') -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $GamePath 'dinput8.dll') -PathType Leaf)
        if ($runtimeInstalled) {
            Add-Check -Category 'Game' -Name 'ScriptHook runtime in game' -Status 'PASS' -Detail 'Runtime jest obecny w katalogu gry.'
        }
        else {
            Add-Check -Category 'Game' -Name 'ScriptHook runtime in game' -Status 'WARN' -Detail 'Runtime nie jest zainstalowany; skrypty projektu celowo go nie instaluja.'
        }
    }
    catch {
        Add-Check -Category 'Game' -Name 'Game verification' -Status 'FAIL' -Detail 'Nie mozna bezpiecznie odczytac RDR2.exe.'
    }
}

if ($Json) {
    $checks | ConvertTo-Json -Depth 4
}
else {
    $checks | Format-Table -AutoSize
}

if (@($checks | Where-Object { $_.Status -eq 'FAIL' }).Count -gt 0) {
    exit 1
}

exit 0
