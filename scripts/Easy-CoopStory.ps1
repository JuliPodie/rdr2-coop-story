#Requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Install', 'Launch', 'Uninstall')]
    [string]$Action,

    [string]$GamePath,

    [string]$GameExe = (Join-Path ${env:ProgramFiles(x86)} `
        'Steam\steamapps\common\Red Dead Redemption 2\RDR2.exe'),

    [switch]$Preview,

    [switch]$Yes
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$workspace = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\', '/')
if (-not [string]::IsNullOrWhiteSpace($GamePath)) {
    $gameExeFull = [IO.Path]::GetFullPath((Join-Path $GamePath 'RDR2.exe'))
}
else {
    $gameExeFull = [IO.Path]::GetFullPath($GameExe)
}
$gameRoot = Split-Path -Parent $gameExeFull
$expectedGameHash = 'B56C9548F670654A9B73BF25DEF3CD73AF12E269F6E47DBA28A34079ADAF465E'
$runtimeManifestPath = Join-Path $workspace 'artifacts\deploy\runtime-deployment-manifest.json'
$projectManifestPath = Join-Path $workspace 'artifacts\deploy\deployment-manifest.json'
$windowsPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'

$runtimeFiles = @(
    [pscustomobject]@{
        RelativePath = 'ScriptHookRDR2.dll'
        SourceRelativePath = 'bin\ScriptHookRDR2.dll'
        Sha256 = '3AC29FBE8C92B664E358F7D4F0AF2EC9F1CA674885975087EF76BD98BF972A4C'
    },
    [pscustomobject]@{
        RelativePath = 'dinput8.dll'
        SourceRelativePath = 'bin\dinput8.dll'
        Sha256 = '956FB3765572D00F6C08BCAE11E9856A00A68107464A87B6CCC6C1FFED46B88A'
    }
)

function Write-Step {
    param([Parameter(Mandatory = $true)][string]$Text)
    Write-Host ''
    Write-Host ('=== ' + $Text + ' ===') -ForegroundColor Cyan
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

function Copy-DurableExclusive {
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

function Write-Utf8CreateNew {
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

function Assert-GameReady {
    param([switch]$RequireSupportedHash)

    if (-not (Test-Path -LiteralPath $gameExeFull -PathType Leaf)) {
        throw ('Nie znaleziono RDR2.exe: ' + $gameExeFull)
    }
    if (-not ([IO.Path]::GetFileName($gameExeFull)).Equals(
        'RDR2.exe',
        [StringComparison]::OrdinalIgnoreCase)) {
        throw 'GameExe musi wskazywac plik RDR2.exe.'
    }
    if (@(Get-Process -Name RDR2 -ErrorAction SilentlyContinue).Count -gt 0) {
        throw 'RDR2 jest uruchomione. Zamknij gre i uruchom ten plik ponownie.'
    }
    if ($RequireSupportedHash) {
        $actualHash = Get-FileSha256 -Path $gameExeFull
        if ($actualHash -ne $expectedGameHash) {
            throw 'Ta wersja RDR2.exe nie jest obslugiwana. Instalacja zostala zatrzymana.'
        }
    }
}

function Assert-SidecarClosed {
    if (@(Get-Process -Name 'CoopStory.Sidecar' -ErrorAction SilentlyContinue).Count -gt 0) {
        throw 'Sidecar jest uruchomiony. Zamknij jego okno i sprobuj ponownie.'
    }
}

function Get-StaleInstallerTemps {
    $ownedNamePattern =
        '^\.coopstory-(?:[0-9a-f]{32}-(?:bridge|config)|runtime-[0-9a-f]{32})\.tmp$'
    $result = New-Object 'System.Collections.Generic.List[object]'
    foreach ($item in (Get-ChildItem -LiteralPath $gameRoot -Force -File)) {
        if ($item.Name -notmatch $ownedNamePattern) {
            continue
        }
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw ('Stary staging jest reparse pointem; odmowa usuniecia: ' +
                $item.Name)
        }
        if ((Split-Path -Parent $item.FullName) -ne $gameRoot) {
            throw 'Wykryto staging poza katalogiem gry.'
        }
        $result.Add($item)
    }
    return @($result | ForEach-Object { $_ })
}

function Remove-StaleInstallerTemps {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [object[]]$Files
    )

    $ownedNamePattern =
        '^\.coopstory-(?:[0-9a-f]{32}-(?:bridge|config)|runtime-[0-9a-f]{32})\.tmp$'
    foreach ($file in $Files) {
        if ($file.Name -notmatch $ownedNamePattern -or
            (Split-Path -Parent $file.FullName) -ne $gameRoot -or
            ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw 'Lista starych stagingow zmienila sie; odmowa usuniecia.'
        }
        if (Test-Path -LiteralPath $file.FullName -PathType Leaf) {
            Remove-Item -LiteralPath $file.FullName -Force
            Write-Host ('Usunieto nieaktywny staging po przerwaniu: ' +
                $file.Name)
        }
    }
}

function Get-UnsafeGameResidue {
    $result = New-Object 'System.Collections.Generic.List[string]'
    foreach ($name in @(
        'CoopStory.config.json',
        'dinput8.dll',
        'ScriptHookRDR2.dll'
    )) {
        if (Test-Path -LiteralPath (Join-Path $gameRoot $name) -PathType Leaf) {
            $result.Add($name)
        }
    }
    foreach ($asi in (
        Get-ChildItem -LiteralPath $gameRoot -Force -File -Filter '*.asi')) {
        $result.Add($asi.Name)
    }
    return @(($result | Sort-Object -Unique) | ForEach-Object { $_ })
}

function Assert-DistReady {
    $required = @(
        (Join-Path $workspace 'dist\CoopStoryBridge.asi'),
        (Join-Path $workspace 'dist\config\coopstory.example.json'),
        (Join-Path $workspace 'dist\sidecar\CoopStory.Sidecar.exe')
    )
    foreach ($path in $required) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw ('Brakuje gotowego pliku paczki: ' + $path)
        }
    }

    $forbidden = @('ScriptHookRDR2.dll', 'dinput8.dll', 'NativeTrainer.asi')
    $forbiddenInDist = @(Get-ChildItem -LiteralPath (Join-Path $workspace 'dist') -Recurse -Force |
        Where-Object { (-not $_.PSIsContainer) -and ($forbidden -contains $_.Name) })
    if ($forbiddenInDist.Count -gt 0) {
        throw 'Katalog dist zawiera runtime lub trainer. Instalacja zostala zatrzymana.'
    }

    $sidecar = Join-Path $workspace 'dist\sidecar\CoopStory.Sidecar.exe'
    $helpOutput = @(& $sidecar --help 2>&1)
    if ($LASTEXITCODE -ne 0 -or
        (($helpOutput -join "`n") -notmatch 'local-test')) {
        throw 'Sidecar nie uruchamia sie na lokalnym .NET 10 albo nie obsluguje local-test.'
    }
}

function Assert-NoConflictingMods {
    $asiFiles = @(Get-ChildItem -LiteralPath $gameRoot -File -Filter '*.asi' -Force |
        Where-Object {
            -not $_.Name.Equals(
                'CoopStoryBridge.asi',
                [StringComparison]::OrdinalIgnoreCase)
        })
    if ($asiFiles.Count -gt 0) {
        throw ('W katalogu gry jest inny mod ASI: ' +
            (($asiFiles | ForEach-Object { $_.Name }) -join ', ') +
            '. Usun go przed kontrolowanym testem.')
    }

    $knownConflicts = @(
        'NativeTrainer.asi',
        'Rampage.asi',
        'RampageFiles',
        'lml',
        'RedM',
        'vfs.asi',
        'ModManager.Core.dll'
    )
    $present = New-Object 'System.Collections.Generic.List[string]'
    foreach ($name in $knownConflicts) {
        if (Test-Path -LiteralPath (Join-Path $gameRoot $name)) {
            $present.Add($name)
        }
    }
    if ($present.Count -gt 0) {
        throw ('Wykryto konfliktujacy mod/loader: ' + ($present -join ', ') +
            '. Instalator niczego nie usunie automatycznie.')
    }
}

function Resolve-RuntimeRoot {
    $exact = Join-Path $workspace 'ScriptHookRDR2_1.0.1491.17'
    if (Test-Path -LiteralPath $exact -PathType Container) {
        return $exact
    }
    throw 'Brakuje lokalnego, rozpakowanego runtime ScriptHookRDR2_1.0.1491.17.'
}

function Invoke-PrerequisiteVerification {
    param([Parameter(Mandatory = $true)][string]$RuntimeRoot)

    if (-not (Test-Path -LiteralPath $windowsPowerShell -PathType Leaf)) {
        throw 'Nie znaleziono Windows PowerShell 5.1.'
    }
    $verifyScript = Join-Path $workspace 'scripts\Verify-Prerequisites.ps1'
    $sdkRoot = Join-Path $workspace 'ScriptHookRDR2_SDK_1.0.1207.73'
    $commandArguments = @(
        '-NoLogo',
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', $verifyScript,
        '-WorkspaceRoot', $workspace,
        '-GamePath', $gameRoot,
        '-SdkPath', $sdkRoot,
        '-RuntimePath', $RuntimeRoot
    )
    & $windowsPowerShell @commandArguments
    if ($LASTEXITCODE -ne 0) {
        throw 'Weryfikacja wymagan nie przeszla. Przeczytaj FAIL powyzej.'
    }
}

function Read-RuntimeReceipt {
    $result = @{}
    if (-not (Test-Path -LiteralPath $runtimeManifestPath -PathType Leaf)) {
        return $result
    }

    try {
        $manifest = Get-Content -LiteralPath $runtimeManifestPath -Raw |
            ConvertFrom-Json
    }
    catch {
        throw 'Manifest runtime jest uszkodzony. Instalator odmawia zgadywania.'
    }
    if ($manifest.SchemaVersion -ne 1) {
        throw 'Manifest runtime ma nieobslugiwana wersje.'
    }
    $fingerprint = Get-StringSha256 -Value ($gameRoot.ToLowerInvariant())
    if ($manifest.GameRootFingerprint -ne $fingerprint) {
        throw 'Manifest runtime zostal utworzony dla innego katalogu gry.'
    }

    $allowed = @{}
    foreach ($spec in $runtimeFiles) {
        $allowed[$spec.RelativePath.ToLowerInvariant()] = $spec
    }
    foreach ($entry in @($manifest.Files)) {
        $relative = [string]$entry.RelativePath
        $key = $relative.ToLowerInvariant()
        if (-not $allowed.ContainsKey($key) -or $result.ContainsKey($key)) {
            throw 'Manifest runtime zawiera niedozwolony albo powtorzony plik.'
        }
        if ([string]$entry.Sha256 -ne $allowed[$key].Sha256) {
            throw 'Manifest runtime zawiera nieoczekiwany hash.'
        }
        if (-not ($entry.OwnedByEasyInstaller -is [bool])) {
            throw 'Manifest runtime nie zawiera poprawnej informacji ownership.'
        }
        $result[$key] = [pscustomobject]@{
            RelativePath = $relative
            Sha256 = [string]$entry.Sha256
            Length = [int64]$entry.Length
            OwnedByEasyInstaller = [bool]$entry.OwnedByEasyInstaller
        }
    }
    return $result
}

function Get-RuntimePlan {
    param([Parameter(Mandatory = $true)][string]$RuntimeRoot)

    $receipt = Read-RuntimeReceipt
    $plans = New-Object 'System.Collections.Generic.List[object]'
    foreach ($spec in $runtimeFiles) {
        $source = Join-Path $RuntimeRoot $spec.SourceRelativePath
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw ('Pakiet runtime nie zawiera: ' + $spec.SourceRelativePath)
        }
        $sourceHash = Get-FileSha256 -Path $source
        if ($sourceHash -ne $spec.Sha256) {
            throw ('Lokalny runtime ma nieoczekiwany hash: ' + $spec.RelativePath)
        }

        $target = Join-Path $gameRoot $spec.RelativePath
        $targetPresent = Test-Path -LiteralPath $target -PathType Leaf
        if ($targetPresent -and (Get-FileSha256 -Path $target) -ne $spec.Sha256) {
            throw ('Plik ' + $spec.RelativePath +
                ' juz istnieje, ale ma inny hash. Instalator go nie nadpisze.')
        }
        $key = $spec.RelativePath.ToLowerInvariant()
        $previouslyOwned = $receipt.ContainsKey($key) -and
            $receipt[$key].OwnedByEasyInstaller
        $plans.Add([pscustomobject]@{
            RelativePath = $spec.RelativePath
            SourcePath = $source
            TargetPath = $target
            Sha256 = $spec.Sha256
            Length = [int64](Get-Item -LiteralPath $source).Length
            NeedsCopy = (-not $targetPresent)
            OwnedAfterInstall = ((-not $targetPresent) -or $previouslyOwned)
        })
    }
    return @($plans | ForEach-Object { $_ })
}

function Write-RuntimeReceipt {
    param([Parameter(Mandatory = $true)][hashtable]$Entries)

    $directory = Split-Path -Parent $runtimeManifestPath
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
        $null = New-Item -ItemType Directory -Path $directory
    }
    $files = @($Entries.Values | Sort-Object RelativePath | ForEach-Object {
        [ordered]@{
            RelativePath = $_.RelativePath
            Sha256 = $_.Sha256
            Length = [int64]$_.Length
            OwnedByEasyInstaller = [bool]$_.OwnedByEasyInstaller
        }
    })
    $manifest = [ordered]@{
        SchemaVersion = 1
        UpdatedAtUtc = [DateTime]::UtcNow.ToString('o')
        GameRootFingerprint = (Get-StringSha256 -Value ($gameRoot.ToLowerInvariant()))
        GameExecutableSha256 = $expectedGameHash
        SourcePackage = 'ScriptHookRDR2_1.0.1491.17'
        NativeTrainerInstalled = $false
        Files = $files
    }
    $temporary = Join-Path $directory (
        '.runtime-manifest-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    $replacementBackup = Join-Path $directory (
        '.runtime-manifest-backup-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    Write-Utf8CreateNew -Path $temporary -Content (
        $manifest | ConvertTo-Json -Depth 6)
    try {
        if (Test-Path -LiteralPath $runtimeManifestPath -PathType Leaf) {
            [IO.File]::Replace(
                $temporary,
                $runtimeManifestPath,
                $replacementBackup,
                $true)
            if (Test-Path -LiteralPath $replacementBackup -PathType Leaf) {
                Remove-Item -LiteralPath $replacementBackup -Force
            }
        }
        else {
            [IO.File]::Move($temporary, $runtimeManifestPath)
        }
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
        if (Test-Path -LiteralPath $replacementBackup) {
            Remove-Item -LiteralPath $replacementBackup -Force
        }
    }
}

function Install-Runtime {
    param([Parameter(Mandatory = $true)][object[]]$Plans)

    $receiptPreviouslyExisted =
        Test-Path -LiteralPath $runtimeManifestPath -PathType Leaf
    $originalEntries = Read-RuntimeReceipt
    $journalEntries = @{}
    foreach ($plan in $Plans) {
        $key = $plan.RelativePath.ToLowerInvariant()
        $journalEntries[$key] = [pscustomobject]@{
            RelativePath = $plan.RelativePath
            Sha256 = $plan.Sha256
            Length = $plan.Length
            OwnedByEasyInstaller = [bool]$plan.OwnedAfterInstall
        }
    }

    # Persist the complete ownership plan before the first final Move. After an
    # abrupt stop, a copied hash-matching DLL is still tracked by the receipt.
    Write-RuntimeReceipt -Entries $journalEntries
    $journalHash = Get-FileSha256 -Path $runtimeManifestPath
    $committedPlans = New-Object 'System.Collections.Generic.List[object]'
    try {
        foreach ($plan in $Plans) {
            if (-not $plan.NeedsCopy) {
                if ($plan.OwnedAfterInstall) {
                    Write-Host ('Runtime juz zainstalowany przez ten instalator: ' +
                        $plan.RelativePath)
                }
                else {
                    Write-Host ('Identyczny runtime byl juz obecny; pozostaje cudzy: ' +
                        $plan.RelativePath)
                }
                continue
            }

            $tag = [Guid]::NewGuid().ToString('N')
            $staging = Join-Path $gameRoot (
                '.coopstory-runtime-' + $tag + '.tmp')
            try {
                Copy-DurableExclusive -Source $plan.SourcePath -Destination $staging
                if ((Get-FileSha256 -Path $staging) -ne $plan.Sha256) {
                    throw ('Weryfikacja stagingu nie powiodla sie: ' +
                        $plan.RelativePath)
                }
                if (Test-Path -LiteralPath $plan.TargetPath) {
                    throw ('Target pojawil sie podczas stagingu: ' +
                        $plan.RelativePath)
                }
                [IO.File]::Move($staging, $plan.TargetPath)
                $committedPlans.Add($plan)
                if ((Get-FileSha256 -Path $plan.TargetPath) -ne $plan.Sha256) {
                    throw ('Weryfikacja finalnego runtime nie powiodla sie: ' +
                        $plan.RelativePath)
                }
                Write-Host ('Skopiowano oficjalny runtime: ' +
                    $plan.RelativePath) -ForegroundColor Green
            }
            finally {
                if (Test-Path -LiteralPath $staging) {
                    Remove-Item -LiteralPath $staging -Force
                }
            }
        }
    }
    catch {
        $rollbackComplete = $true
        foreach ($committedPlan in $committedPlans) {
            if (Test-Path -LiteralPath $committedPlan.TargetPath -PathType Leaf) {
                if ((Get-FileSha256 -Path $committedPlan.TargetPath) -eq
                    $committedPlan.Sha256) {
                    Remove-Item -LiteralPath $committedPlan.TargetPath -Force
                }
                else {
                    $rollbackComplete = $false
                }
            }
        }
        if ($rollbackComplete -and
            (Test-Path -LiteralPath $runtimeManifestPath -PathType Leaf) -and
            (Get-FileSha256 -Path $runtimeManifestPath) -eq $journalHash) {
            if ($receiptPreviouslyExisted) {
                Write-RuntimeReceipt -Entries $originalEntries
            }
            else {
                Remove-Item -LiteralPath $runtimeManifestPath -Force
            }
        }
        throw
    }
}

function Test-ConfigSafety {
    param([Parameter(Mandatory = $true)][string]$Path)

    try {
        $config = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    }
    catch {
        throw 'CoopStory.config.json jest uszkodzony.'
    }
    if ($config.schemaVersion -ne 1 -or
        $config.safety.storyModeOnly -ne $true -or
        $config.safety.refuseOnlineMode -ne $true) {
        throw 'Konfiguracja nie zawiera obowiazkowych blokad Story Mode/RDO.'
    }
}

function Test-ProjectAlreadyInstalled {
    if (-not (Test-Path -LiteralPath $projectManifestPath -PathType Leaf)) {
        return $false
    }
    try {
        $manifest = Get-Content -LiteralPath $projectManifestPath -Raw |
            ConvertFrom-Json
    }
    catch {
        throw 'Manifest moda jest uszkodzony. Uzyj bezpiecznego deinstalatora.'
    }
    $fingerprint = Get-StringSha256 -Value ($gameRoot.ToLowerInvariant())
    if ($manifest.SchemaVersion -ne 1 -or
        $manifest.GameRootFingerprint -ne $fingerprint) {
        throw 'Manifest moda nie pasuje do tego katalogu gry.'
    }

    $bridgeTarget = Join-Path $gameRoot 'CoopStoryBridge.asi'
    $configTarget = Join-Path $gameRoot 'CoopStory.config.json'
    if (-not (Test-Path -LiteralPath $bridgeTarget -PathType Leaf) -or
        -not (Test-Path -LiteralPath $configTarget -PathType Leaf)) {
        throw 'Manifest istnieje, ale instalacja moda jest niepelna.'
    }
    $bridgeEntry = @($manifest.Files | Where-Object {
        $_.RelativePath -eq 'CoopStoryBridge.asi'
    }) | Select-Object -First 1
    if ($null -eq $bridgeEntry) {
        throw 'Manifest moda nie zawiera bridge.'
    }
    $installedHash = Get-FileSha256 -Path $bridgeTarget
    $distHash = Get-FileSha256 -Path (
        Join-Path $workspace 'dist\CoopStoryBridge.asi')
    if ($installedHash -ne [string]$bridgeEntry.Sha256 -or
        $installedHash -ne $distHash) {
        throw 'Zainstalowany bridge jest inny. Najpierw uzyj deinstalatora.'
    }
    Test-ConfigSafety -Path $configTarget
    return $true
}

function Confirm-Exact {
    param(
        [Parameter(Mandatory = $true)][string]$Expected,
        [Parameter(Mandatory = $true)][string]$Prompt
    )

    if ($Yes) {
        return
    }
    $answer = Read-Host $Prompt
    if ($answer -cne $Expected) {
        throw 'Anulowano. Nie zmieniono katalogu gry.'
    }
}

function Install-Easy {
    Write-Step 'Kontrola gry i paczki'
    Assert-GameReady -RequireSupportedHash
    Assert-SidecarClosed
    Assert-DistReady
    Assert-NoConflictingMods
    $runtimeRoot = Resolve-RuntimeRoot
    Invoke-PrerequisiteVerification -RuntimeRoot $runtimeRoot
    $runtimePlans = @(Get-RuntimePlan -RuntimeRoot $runtimeRoot)
    $staleTemps = @(Get-StaleInstallerTemps)
    $projectInstalled = Test-ProjectAlreadyInstalled
    $runtimeNeedsCopy = @($runtimePlans | Where-Object { $_.NeedsCopy }).Count -gt 0

    if ($projectInstalled -and -not $runtimeNeedsCopy -and
        $staleTemps.Count -eq 0) {
        Write-Host ''
        Write-Host 'Mod i runtime sa juz poprawnie zainstalowane.' -ForegroundColor Green
        Write-Host 'Uruchom teraz: 2_URUCHOM_TEST.bat'
        return
    }

    Write-Step 'Bezpieczny dry-run instalacji'
    if ($projectInstalled) {
        Write-Host '  pliki projektu: juz zainstalowane; naprawiany bedzie tylko runtime'
    }
    else {
        & (Join-Path $workspace 'scripts\Install-DevBuild.ps1') `
            -GamePath $gameRoot `
            -WorkspaceRoot $workspace
    }
    foreach ($plan in $runtimePlans) {
        if ($plan.NeedsCopy) {
            Write-Host ('  copy runtime: ' + $plan.RelativePath)
        }
        else {
            Write-Host ('  leave identical runtime: ' + $plan.RelativePath)
        }
    }
    Write-Host '  NativeTrainer.asi: NIGDY NIE JEST KOPIOWANY'
    foreach ($temp in $staleTemps) {
        Write-Host ('  remove stale non-loadable staging: ' + $temp.Name)
    }

    if ($Preview) {
        Write-Host ''
        Write-Host 'Preview zakonczony. Nie zmieniono katalogu gry.' -ForegroundColor Yellow
        return
    }

    Write-Host ''
    Write-Host 'To jest eksperymentalny smoke test, nie gotowy coop kampanii.' `
        -ForegroundColor Yellow
    Confirm-Exact -Expected 'INSTALUJ' `
        -Prompt 'Aby kontynuowac, wpisz dokladnie INSTALUJ'

    Remove-StaleInstallerTemps -Files $staleTemps

    Write-Step 'Backup save i baseline'
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
    & (Join-Path $workspace 'scripts\Backup-Saves.ps1') `
        -DestinationRoot (Join-Path $workspace '_backups\saves-easy-installer') `
        -BackupName ('RDR2-saves-' + $stamp)
    & (Join-Path $workspace 'scripts\Capture-GameBaseline.ps1') `
        -GamePath $gameRoot `
        -OutputPath (Join-Path $workspace (
            'artifacts\baselines\easy-before-install-' + $stamp + '.json'))

    if (-not $projectInstalled) {
        Write-Step 'Instalacja nieaktywnego bridge i konfiguracji'
        & (Join-Path $workspace 'scripts\Install-DevBuild.ps1') `
            -GamePath $gameRoot `
            -WorkspaceRoot $workspace `
            -Apply
    }

    # The loader is the last commit. If an earlier project step fails, no new
    # executable loader is left in the game directory.
    Write-Step 'Instalacja oficjalnego runtime i loadera jako ostatni krok'
    Install-Runtime -Plans $runtimePlans

    Write-Host ''
    Write-Host 'INSTALACJA GOTOWA.' -ForegroundColor Green
    Write-Host 'Nastepnie kliknij 2_URUCHOM_TEST.bat i wybierz tylko Story Mode.'
}

function Get-OwnedRuntimeForRemoval {
    $receipt = Read-RuntimeReceipt
    $owned = New-Object 'System.Collections.Generic.List[object]'
    foreach ($entry in $receipt.Values) {
        if (-not $entry.OwnedByEasyInstaller) {
            continue
        }
        $target = Join-Path $gameRoot $entry.RelativePath
        if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
            continue
        }
        if ((Get-FileSha256 -Path $target) -ne $entry.Sha256) {
            throw ('Nasz runtime zostal zmieniony: ' + $entry.RelativePath +
                '. Deinstalator odmawia usuniecia.')
        }
        $owned.Add([pscustomobject]@{
            RelativePath = $entry.RelativePath
            FullPath = $target
            Sha256 = $entry.Sha256
        })
    }
    return @($owned | ForEach-Object { $_ })
}

function Uninstall-Runtime {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [object[]]$OwnedFiles
    )

    if (-not (Test-Path -LiteralPath $runtimeManifestPath -PathType Leaf)) {
        return
    }
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
    $backup = Join-Path $workspace (
        'artifacts\uninstall-backups\runtime-' + $stamp)
    $null = New-Item -ItemType Directory -Path $backup

    foreach ($file in $OwnedFiles) {
        $target = Join-Path $backup $file.RelativePath
        Copy-DurableExclusive -Source $file.FullPath -Destination $target
        if ((Get-FileSha256 -Path $target) -ne $file.Sha256) {
            throw ('Backup runtime nie przeszedl weryfikacji: ' +
                $file.RelativePath)
        }
    }
    $manifestBackup = Join-Path $backup 'runtime-deployment-manifest.json'
    Copy-DurableExclusive -Source $runtimeManifestPath -Destination $manifestBackup
    $manifestHash = Get-FileSha256 -Path $runtimeManifestPath
    if ((Get-FileSha256 -Path $manifestBackup) -ne $manifestHash) {
        throw 'Backup manifestu runtime nie przeszedl weryfikacji.'
    }

    $removalOrder = @($OwnedFiles | Sort-Object @{
        Expression = {
            if ($_.RelativePath -eq 'dinput8.dll') { 0 } else { 1 }
        }
    })
    foreach ($file in $removalOrder) {
        if ((Get-FileSha256 -Path $file.FullPath) -ne $file.Sha256) {
            throw ('Runtime zmienil sie podczas backupu: ' + $file.RelativePath)
        }
        Remove-Item -LiteralPath $file.FullPath -Force
    }
    if ((Get-FileSha256 -Path $runtimeManifestPath) -ne $manifestHash) {
        throw 'Manifest runtime zmienil sie podczas operacji.'
    }
    Remove-Item -LiteralPath $runtimeManifestPath -Force
    Write-Host ('Backup usunietego runtime: ' + $backup)
}

function Uninstall-Easy {
    Write-Step 'Kontrola przed deinstalacja'
    Assert-GameReady
    Assert-SidecarClosed
    $ownedRuntime = @(Get-OwnedRuntimeForRemoval)
    $hasProject = Test-Path -LiteralPath $projectManifestPath -PathType Leaf
    $hasRuntimeReceipt = Test-Path -LiteralPath $runtimeManifestPath -PathType Leaf
    $staleTemps = @(Get-StaleInstallerTemps)
    if (-not $hasProject -and -not $hasRuntimeReceipt -and
        $staleTemps.Count -eq 0) {
        $untrackedResidue = @(Get-UnsafeGameResidue)
        if ($untrackedResidue.Count -gt 0) {
            Write-Host ('UWAGA: brak manifestu, ale nadal obecne: ' +
                ($untrackedResidue -join ', ')) -ForegroundColor Red
            throw 'Deinstalator nie bedzie zgadywal, co wolno usunac. NIE WCHODZ DO RDO.'
        }
        Write-Host 'Nie znaleziono instalacji wykonanej przez ten instalator.'
        return
    }

    foreach ($file in $ownedRuntime) {
        Write-Host ('  backup + remove owned runtime: ' + $file.RelativePath)
    }
    $receipt = Read-RuntimeReceipt
    foreach ($entry in $receipt.Values) {
        if (-not $entry.OwnedByEasyInstaller) {
            Write-Host ('  leave pre-existing runtime: ' + $entry.RelativePath)
        }
    }
    if ($hasProject) {
        & (Join-Path $workspace 'scripts\Uninstall-DevBuild.ps1') `
            -GamePath $gameRoot `
            -WorkspaceRoot $workspace
    }
    foreach ($temp in $staleTemps) {
        Write-Host ('  remove stale non-loadable staging: ' + $temp.Name)
    }

    if ($Preview) {
        Write-Host 'Preview zakonczony. Niczego nie usunieto.' -ForegroundColor Yellow
        return
    }
    Confirm-Exact -Expected 'ODINSTALUJ' `
        -Prompt 'Aby kontynuowac, wpisz dokladnie ODINSTALUJ'

    # Disable the loader first. If a later project-file cleanup fails, an
    # inert ASI is safer than a live dinput8 loader.
    if ($hasRuntimeReceipt) {
        Uninstall-Runtime -OwnedFiles $ownedRuntime
    }
    if ($hasProject) {
        & (Join-Path $workspace 'scripts\Uninstall-DevBuild.ps1') `
            -GamePath $gameRoot `
            -WorkspaceRoot $workspace `
            -Apply
    }
    Remove-StaleInstallerTemps -Files $staleTemps

    Write-Host ''
    Write-Host 'DEINSTALACJA GOTOWA.' -ForegroundColor Green
    Write-Host 'Pliki innych modow i cudzy runtime pozostaly nietkniete.'
    $remainingUnsafe = @(Get-UnsafeGameResidue)
    if ($remainingUnsafe.Count -gt 0) {
        Write-Host ''
        Write-Host ('UWAGA: nadal obecne: ' +
            ($remainingUnsafe -join ', ')) `
            -ForegroundColor Red
        Write-Host 'NIE WCHODZ DO RDO, dopoki nie usuniesz ich recznie.' `
            -ForegroundColor Red
    }
}

function Launch-Easy {
    Write-Step 'Kontrola instalacji'
    Assert-GameReady -RequireSupportedHash
    Assert-DistReady
    Assert-NoConflictingMods
    if (-not (Test-ProjectAlreadyInstalled)) {
        throw 'Mod nie jest zainstalowany. Najpierw kliknij 1_ZAINSTALUJ_MOD.bat.'
    }
    foreach ($spec in $runtimeFiles) {
        $target = Join-Path $gameRoot $spec.RelativePath
        if (-not (Test-Path -LiteralPath $target -PathType Leaf) -or
            (Get-FileSha256 -Path $target) -ne $spec.Sha256) {
            throw ('Brakuje poprawnego runtime: ' + $spec.RelativePath)
        }
    }
    if (@(Get-Process -Name 'CoopStory.Sidecar' -ErrorAction SilentlyContinue).Count -gt 0) {
        throw 'Sidecar juz dziala. Zamknij jego okno i uruchom launcher ponownie.'
    }

    $sidecar = Join-Path $workspace 'dist\sidecar\CoopStory.Sidecar.exe'
    $config = Join-Path $gameRoot 'CoopStory.config.json'
    Test-ConfigSafety -Path $config

    $launchDirectory = Join-Path $workspace 'artifacts\launch'
    if (-not (Test-Path -LiteralPath $launchDirectory -PathType Container)) {
        $null = New-Item -ItemType Directory -Path $launchDirectory
    }
    $launchId = [Guid]::NewGuid().ToString('N')
    $readyFile = Join-Path $launchDirectory ('local-test-' + $launchId + '.ready')
    $failureFile = Join-Path $launchDirectory ('local-test-' + $launchId + '.failed')
    $escapedSidecar = $sidecar.Replace("'", "''")
    $escapedConfig = $config.Replace("'", "''")
    $escapedReady = $readyFile.Replace("'", "''")
    $escapedFailure = $failureFile.Replace("'", "''")
    $command = @"
`$Host.UI.RawUI.WindowTitle = 'RDR2 Coop Story - LOCAL TEST'
Write-Host 'Nie zamykaj tego okna podczas testu.' -ForegroundColor Cyan
Write-Host 'Sukces: pojawia sie LOCAL_TEST_BRIDGE_ACTIVE i LOCAL_TEST_GUEST_STREAMING.'
& '$escapedSidecar' local-test --config '$escapedConfig' --ready-file '$escapedReady' --motion-profile puppet
`$sidecarExit = `$LASTEXITCODE
if (-not (Test-Path -LiteralPath '$escapedReady')) {
    [IO.File]::WriteAllText('$escapedFailure', [string]`$sidecarExit)
}
Write-Host ''
Write-Host 'Sidecar zakonczyl prace. Przeczytaj komunikat bledu powyzej.' -ForegroundColor Yellow
Read-Host 'Nacisnij Enter, aby zamknac to okno'
exit `$sidecarExit
"@
    $encoded = [Convert]::ToBase64String(
        [Text.Encoding]::Unicode.GetBytes($command))
    $sidecarWindow = Start-Process -FilePath $windowsPowerShell `
        -ArgumentList (
            '-NoLogo -NoProfile -ExecutionPolicy Bypass -EncodedCommand ' +
            $encoded) `
        -WorkingDirectory (Split-Path -Parent $sidecar) `
        -PassThru

    $ready = $false
    $readyWait = [Diagnostics.Stopwatch]::StartNew()
    while ($readyWait.ElapsedMilliseconds -lt 8000) {
        if (Test-Path -LiteralPath $failureFile -PathType Leaf) {
            $failureCode = Get-Content -LiteralPath $failureFile -Raw
            throw ('Local-test zakonczyl sie przed gotowoscia. Kod: ' +
                $failureCode)
        }
        if (Test-Path -LiteralPath $readyFile -PathType Leaf) {
            $sidecarProcesses = @(
                Get-Process -Name 'CoopStory.Sidecar' -ErrorAction SilentlyContinue)
            if ($sidecarProcesses.Count -gt 0) {
                $ready = $true
                break
            }
        }
        $sidecarWindow.Refresh()
        if ($sidecarWindow.HasExited) {
            throw 'Okno local-test zamknelo sie przed osiagnieciem gotowosci.'
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $ready) {
        throw 'Local-test nie osiagnal gotowosci w ciagu 8 sekund. Steam nie zostal uruchomiony.'
    }

    Write-Host 'Local-test gotowy. Otwieram RDR2 przez Steam...'
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = 'steam://rungameid/1174180'
    $startInfo.UseShellExecute = $true
    try {
        $null = [Diagnostics.Process]::Start($startInfo)
    }
    catch {
        Write-Warning 'Nie udalo sie otworzyc Steam automatycznie. Kliknij Graj w Steam recznie.'
    }

    Write-Host ''
    Write-Host 'W grze wybierz wylacznie STORY MODE.' -ForegroundColor Yellow
    Write-Host 'NIGDY nie wchodz do Red Dead Online z zainstalowanym modem.'
    Write-Host 'Przed RDO zawsze uruchom 3_ODINSTALUJ_MOD.bat.'
}

try {
    switch ($Action) {
        'Install' { Install-Easy }
        'Launch' { Launch-Easy }
        'Uninstall' { Uninstall-Easy }
    }
}
catch {
    Write-Host ''
    Write-Host ('BLAD: ' + $_.Exception.Message) -ForegroundColor Red
    if ($Action -eq 'Install' -and -not $Preview) {
        Write-Host 'NIE URUCHAMIAJ GRY ANI RDO.' -ForegroundColor Red
        Write-Host 'Zachowaj ten folder i uruchom 3_ODINSTALUJ_MOD.bat.' `
            -ForegroundColor Red
    }
    elseif ($Action -eq 'Uninstall' -and -not $Preview) {
        Write-Host 'Loader moze nadal byc obecny. NIE WCHODZ DO RDO.' `
            -ForegroundColor Red
        Write-Host 'Zachowaj ten folder, zamknij gre/sidecar i ponow deinstalacje.' `
            -ForegroundColor Red
    }
    exit 1
}

exit 0
