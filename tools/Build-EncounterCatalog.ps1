[CmdletBinding()]
param(
    [string]$OutputPath = (Join-Path $PSScriptRoot '..\out\rdr2-1491.50-encounter-catalog.json')
)

$ErrorActionPreference = 'Stop'
$repo = 'Halen84/RDR3-Decompiled-Scripts'
$ref = 'master'
$headers = @{ 'User-Agent' = 'CoopStoryEncounterCatalog/1.0' }
$tree = Invoke-RestMethod -Headers $headers -Uri "https://api.github.com/repos/$repo/git/trees/$ref`?recursive=1"
if ($tree.truncated) {
    throw 'GitHub truncated the source tree; refusing to create an incomplete encounter catalog.'
}

function Get-Joaat([string]$Value) {
    [uint64]$hash = 0
    [uint64]$modulus = 4294967296
    foreach ($character in $Value.ToLowerInvariant().ToCharArray()) {
        $hash = ($hash + [uint64][byte][char]$character) % $modulus
        $hash = ($hash * 1025) % $modulus
        $hash = ($hash -bxor ($hash -shr 6)) % $modulus
    }
    $hash = ($hash * 9) % $modulus
    $hash = ($hash -bxor ($hash -shr 11)) % $modulus
    return [uint32](($hash * 32769) % $modulus)
}

function Get-Classification([string]$Script) {
    # A script filename is sufficient to catalogue an observed local event,
    # never sufficient to grant rewards or run Rockstar's script remotely.
    if ($Script -match '^ambush_' -or $Script -match '^beat_(campfire|dark_alley|parlor|slum)_ambush') {
        return @{ lane = 'BridgeOwnedCandidate'; profile = 'RoadsideAmbush' }
    }
    if ($Script -match '^(beat_animal_(attack|mauling)|av_animal_attack)$') {
        return @{ lane = 'BridgeOwnedCandidate'; profile = 'AnimalAttack' }
    }
    if ($Script -match '^beat_(hostage_rescue|kidnap_victim|lone_prisoner|torturing_captive|trapped_woman)$') {
        return @{ lane = 'BridgeOwnedCandidate'; profile = 'HostageRescue' }
    }
    if ($Script -match '^beat_(wagon_threat|prison_wagon|savage_wagon)$') {
        return @{ lane = 'BridgeOwnedCandidate'; profile = 'WagonDefense' }
    }
    if ($Script -match '^beat_(moonshine_camp|murder_campfire|player_camp_attack)$' -or $Script -eq 'av_amb_camp_robbery') {
        return @{ lane = 'BridgeOwnedCandidate'; profile = 'CampClearout' }
    }
    if ($Script -eq 'beat_odriscoll_town_encounter') {
        return @{ lane = 'ExactIdCandidate'; profile = 'HostageRescue' }
    }
    return @{ lane = 'LocalOnly'; profile = $null }
}

$entries = foreach ($item in $tree.tree) {
    if ($item.type -ne 'blob' -or $item.path -notmatch '^1491\.50/(ambush_|beat_|av_).*\.c$') { continue }
    $script = [IO.Path]::GetFileNameWithoutExtension($item.path)
    $classification = Get-Classification $script
    [ordered]@{
        script = $script
        scriptHash = ('0x{0:X8}' -f (Get-Joaat $script))
        sourcePath = $item.path
        sourceBlob = $item.sha
        lane = $classification.lane
        profile = $classification.profile
        eligibility = 'Requires exact native verification on both saves'
        rewards = 'LocalOnly unless a reviewed per-player mapping is added'
        dialogue = 'LocalOnly unless a reviewed dialogue root is added'
    }
}

$catalog = [ordered]@{
    schema = 1
    gameBuild = '1.0.1491.50'
    generatedAtUtc = [DateTime]::UtcNow.ToString('O')
    source = "https://github.com/$repo/tree/$ref/1491.50"
    policy = 'Filename classification is discovery only; it never authorizes Rockstar-script execution, rewards, or dialogue replication.'
    entries = @($entries | Sort-Object script)
}

$directory = Split-Path -Parent $OutputPath
if (-not [string]::IsNullOrWhiteSpace($directory)) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}
$catalog | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $OutputPath -Encoding utf8
Write-Output "Wrote $($catalog.entries.Count) encounter records to $OutputPath"
