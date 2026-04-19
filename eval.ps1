#requires -Version 5.1
<#
Eval harness: launch the game with the current mod, let it run for a bounded
duration, kill it if it doesn't self-exit, then summarize what happened.
Supports named scenarios (ablate the engine bot) and N repeat runs with
aggregate statistics.

Usage:
  eval.ps1                              # baseline 30s run
  eval.ps1 -Duration 60                 # longer match
  eval.ps1 -Scenario stationary         # enemy bot can't move
  eval.ps1 -Scenario noaim              # enemy bot can't aim
  eval.ps1 -Scenario hardmode           # enemy bot has perfect aim
  eval.ps1 -Runs 3                      # repeat 3x, aggregate avg
  eval.ps1 -Cvars @{ultron_bot_fire_cone=3; ultron_bot_no_strafe=1}
  eval.ps1 -Debug                       # enables ultron_bot_debug + bot_debugSystem
#>

[CmdletBinding()]
param(
    [int]$Duration = 30,
    [string]$Map = 'q2dm1',
    [string]$OutDir = "$PSScriptRoot\eval-logs",
    [switch]$KeepRunning,
    [switch]$Fullscreen,
    [switch]$Quiet,
    [ValidateSet('', 'baseline','stationary','passive','noaim','deaf','hardmode','crippled')]
    [string]$Scenario = 'baseline',
    [int]$Runs = 1,
    [int]$BotSkill = -1,
    [switch]$DebugBot,
    [hashtable]$Cvars = @{}
)

$ErrorActionPreference = 'Stop'

$SavedGames = Join-Path ([Environment]::GetFolderPath('UserProfile')) 'Saved Games\Nightdive Studios\Quake II'
$StdoutPath = Join-Path $SavedGames 'stdout.txt'
$StderrPath = Join-Path $SavedGames 'stderr.txt'
$CrashLog   = 'D:\SteamLibrary\steamapps\common\Quake 2\rerelease\CRASHLOG.TXT'

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Scenario -> (launcher EnemyMode, extra cvars).
$scenarioSpec = @{
    'baseline'   = @{ EnemyMode = 'normal';     Extras = @{} }
    'stationary' = @{ EnemyMode = 'stationary'; Extras = @{} }
    'passive'    = @{ EnemyMode = 'passive';    Extras = @{} }
    'noaim'      = @{ EnemyMode = 'noaim';      Extras = @{} }
    'deaf'       = @{ EnemyMode = 'deaf';       Extras = @{} }
    'hardmode'   = @{ EnemyMode = 'instant';    Extras = @{} }
    'crippled'   = @{ EnemyMode = 'crippled';   Extras = @{} }
}
if (-not $Scenario) { $Scenario = 'baseline' }
if (-not $scenarioSpec.ContainsKey($Scenario)) { throw "Unknown scenario '$Scenario'" }
$spec = $scenarioSpec[$Scenario]

function Invoke-OneRun {
    param([int]$RunIdx)
    $stamp   = (Get-Date).ToString('yyyyMMdd-HHmmss')
    $runTag  = if ($Runs -gt 1) { "-run$RunIdx" } else { '' }
    $outFile = Join-Path $OutDir "eval-$Scenario-$stamp$runTag.log"

    Remove-Item -LiteralPath $StdoutPath,$StderrPath,$CrashLog -ErrorAction SilentlyContinue

    $mergedCvars = @{}
    foreach ($k in $spec.Extras.Keys) { $mergedCvars[$k] = $spec.Extras[$k] }
    foreach ($k in $Cvars.Keys)       { $mergedCvars[$k] = $Cvars[$k] }

    $mlArgs = @{
        StartMap    = $Map
        EvalSeconds = $Duration
        EnemyMode   = $spec.EnemyMode
        Cvars       = $mergedCvars
    }
    if ($Fullscreen) { $mlArgs['Fullscreen'] = $true }
    if ($DebugBot)   { $mlArgs['DebugBot']   = $true }
    if ($BotSkill -ge 0) { $mlArgs['BotSkill'] = $BotSkill }

    Write-Host ("[eval] run {0}/{1}: scenario={2} duration={3}s map={4}" -f $RunIdx,$Runs,$Scenario,$Duration,$Map) -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot 'modlaunch.ps1') @mlArgs | Out-Host
    return $outFile
}

function Wait-AndCollect {
    param([string]$OutFile)

    Start-Sleep -Seconds 6
    if (-not (Get-Process -Name quake2ex_steam -ErrorAction SilentlyContinue)) {
        Write-Host "[eval] warning: engine not running 6s after launch" -ForegroundColor Yellow
    }

    $deadline = (Get-Date).AddSeconds($Duration + 20)
    while ((Get-Date) -lt $deadline) {
        if (-not (Get-Process -Name quake2ex_steam -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Seconds 1
    }
    if (Get-Process -Name quake2ex_steam -ErrorAction SilentlyContinue) {
        Write-Host "[eval] process did not auto-quit; force-killing" -ForegroundColor Yellow
        Stop-Process -Name quake2ex_steam -Force
        Start-Sleep -Seconds 2
    }

    $stdoutLines = @()
    if (Test-Path -LiteralPath $StdoutPath) {
        Copy-Item -LiteralPath $StdoutPath -Destination $OutFile -Force
        $stdoutLines = Get-Content -LiteralPath $StdoutPath
    }
    $crashed = Test-Path -LiteralPath $CrashLog
    $modLines  = $stdoutLines | Where-Object { $_ -match '\[(ultron|eval)\]' }
    $evalLines = $modLines    | Where-Object { $_ -match '\[eval\]' }
    $last = $evalLines | Where-Object { $_ -match '\[eval\] t=' } | Select-Object -Last 1

    # DM-mode sanity: the autostart banner now includes "deathmatch=1" when
    # the engine is actually in DM. Also require an enemy "connected." line
    # with a non-Mechaghost name.
    $dmBanner   = ($stdoutLines | Where-Object { $_ -match '\[ultron\] autostart: .*deathmatch=1' }).Count -gt 0
    $evalStart  = ($stdoutLines | Where-Object { $_ -match '\[eval\] start .* deathmatch=1' }).Count -gt 0
    # Bot names can contain spaces (e.g. "The Makron"), so match up to
    # " connected." at end of line.
    $enemyCnxn  = @($stdoutLines | Where-Object { $_ -match ' connected\.\s*$' -and $_ -notmatch '^Mechaghost connected' }).Count
    $dmValid    = ($dmBanner -or $evalStart) -and ($enemyCnxn -ge 1)

    $stats = [ordered]@{
        scenario     = $Scenario
        duration_s   = $Duration
        crashed      = $crashed
        dm_valid     = $dmValid
        enemy_count  = $enemyCnxn
        stdout_lines = $stdoutLines.Count
        ultron_lines = $modLines.Count
        eval_lines   = $evalLines.Count
        log_file     = $OutFile
    }
    if ($last -and $last -match 't=(?<t>[\d\.]+)\s+score=(?<s>-?\d+)\s+health=(?<h>-?\d+)\s+fire_ticks=(?<f>\d+)\s+target_ticks=(?<tt>\d+)\s+idle_ticks=(?<i>\d+)') {
        $stats['last_t']       = [double]$Matches.t
        $stats['last_score']   = [int]$Matches.s
        $stats['last_health']  = [int]$Matches.h
        $stats['fire_ticks']   = [int]$Matches.f
        $stats['target_ticks'] = [int]$Matches.tt
        $stats['idle_ticks']   = [int]$Matches.i
    }
    return @{ stats = $stats; mod_lines = $modLines; crashed = $crashed }
}

if ($KeepRunning) {
    Invoke-OneRun -RunIdx 1 | Out-Null
    Write-Host "[eval] -KeepRunning set; leaving the game alive. Exiting harness." -ForegroundColor Yellow
    exit 0
}

# --- run N back-to-back -----------------------------------------------------
$all = @()
for ($i = 1; $i -le $Runs; $i++) {
    $outFile = Invoke-OneRun -RunIdx $i
    $res = Wait-AndCollect -OutFile $outFile
    $all += , $res
}

# --- per-run table ---------------------------------------------------------
Write-Host ""
Write-Host "=== eval summary (scenario=$Scenario, runs=$Runs) ===" -ForegroundColor Green
$fmt = '{0,-4} {1,-3} {2,-6} {3,-7} {4,-7} {5,-7} {6,-6} {7,-6} {8}'
Write-Host ($fmt -f 'run','dm','score','health','fire','target','idle','t','log')
$runIdx = 0
foreach ($r in $all) {
    $runIdx++
    $s = $r.stats
    $dm     = if ($s['dm_valid']) { 'OK' } else { 'NO' }
    $score  = if ($s.Contains('last_score'))  { $s['last_score']  } else { '?' }
    $health = if ($s.Contains('last_health')) { $s['last_health'] } else { '?' }
    $fire   = if ($s.Contains('fire_ticks'))  { $s['fire_ticks']  } else { '?' }
    $target = if ($s.Contains('target_ticks')){ $s['target_ticks']} else { '?' }
    $idle   = if ($s.Contains('idle_ticks'))  { $s['idle_ticks']  } else { '?' }
    $t      = if ($s.Contains('last_t'))      { $s['last_t']      } else { '?' }
    Write-Host ($fmt -f $runIdx,$dm,$score,$health,$fire,$target,$idle,$t,(Split-Path -Leaf $s.log_file))
}

# --- aggregate -------------------------------------------------------------
function MeanOf($list) { if ($list.Count -eq 0) { return 0 } ; ($list | Measure-Object -Average).Average }
$scores  = @($all | ForEach-Object { $_.stats['last_score']  } | Where-Object { $_ -ne $null })
$healths = @($all | ForEach-Object { $_.stats['last_health'] } | Where-Object { $_ -ne $null })
$fires   = @($all | ForEach-Object { $_.stats['fire_ticks']  } | Where-Object { $_ -ne $null })
$targets = @($all | ForEach-Object { $_.stats['target_ticks']} | Where-Object { $_ -ne $null })
$idles   = @($all | ForEach-Object { $_.stats['idle_ticks']  } | Where-Object { $_ -ne $null })
$crashCount   = @($all | Where-Object { $_.crashed }).Count
$dmBadCount   = @($all | Where-Object { -not $_.stats['dm_valid'] }).Count

$agg = [ordered]@{
    scenario     = $Scenario
    runs         = $Runs
    duration     = $Duration
    dm_invalid   = $dmBadCount
    avg_score    = [math]::Round((MeanOf $scores),  2)
    avg_health   = [math]::Round((MeanOf $healths), 1)
    avg_fire     = [math]::Round((MeanOf $fires),   0)
    avg_target   = [math]::Round((MeanOf $targets), 0)
    avg_idle     = [math]::Round((MeanOf $idles),   0)
    crashes      = $crashCount
}

Write-Host ""
Write-Host "--- aggregate ---" -ForegroundColor Green
$agg.GetEnumerator() | ForEach-Object { Write-Host ("  {0,-12} {1}" -f $_.Key, $_.Value) }

if ($crashCount -gt 0 -and (Test-Path -LiteralPath $CrashLog)) {
    Write-Host ""
    Write-Host "--- CRASHLOG.TXT head (latest crash) ---" -ForegroundColor Red
    Get-Content -LiteralPath $CrashLog -TotalCount 25 | ForEach-Object { Write-Host "  $_" }
}

if (-not $Quiet -and $all.Count -eq 1) {
    Write-Host ""
    Write-Host "--- mod log (last 30 lines) ---" -ForegroundColor Gray
    $all[0].mod_lines | Select-Object -Last 30 | ForEach-Object { Write-Host "  $_" }
}

$json = $agg | ConvertTo-Json -Compress
Write-Host ""
Write-Host "JSON: $json"
