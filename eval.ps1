#requires -Version 5.1
<#
Eval harness: launch the game with the current mod, let it run for a bounded
duration, kill it if it doesn't self-exit, then summarize what happened.

Workflow Claude (or the user) runs in a loop:
  1. .\eval.ps1 -Duration 30       # runs one match, mod auto-quits at 30s
  2. read the printed summary + eval.log
  3. edit code
  4. repeat

Usage:
  eval.ps1                         # 30s window on q2dm1, windowed mode
  eval.ps1 -Duration 60            # 60s window
  eval.ps1 -Map q2dm3              # different map
  eval.ps1 -OutDir .\eval-logs     # where to save the log (default ./eval-logs)
  eval.ps1 -KeepRunning            # skip the auto-kill pass (for manual poke)
#>

[CmdletBinding()]
param(
    [int]$Duration = 30,
    [string]$Map = 'q2dm1',
    [string]$OutDir = "$PSScriptRoot\eval-logs",
    [switch]$KeepRunning,
    [switch]$Fullscreen,
    [switch]$Quiet             # suppress the tail-of-log dump at end
)

$ErrorActionPreference = 'Stop'

$SavedGames = Join-Path ([Environment]::GetFolderPath('UserProfile')) 'Saved Games\Nightdive Studios\Quake II'
$StdoutPath = Join-Path $SavedGames 'stdout.txt'
$StderrPath = Join-Path $SavedGames 'stderr.txt'
$CrashLog   = 'D:\SteamLibrary\steamapps\common\Quake 2\rerelease\CRASHLOG.TXT'

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stamp    = (Get-Date).ToString('yyyyMMdd-HHmmss')
$outFile  = Join-Path $OutDir "eval-$stamp.log"

# --- 1) clear prior logs so we only see this run's output ----------------
Remove-Item -LiteralPath $StdoutPath,$StderrPath,$CrashLog -ErrorAction SilentlyContinue

# --- 2) launch via modlaunch (which builds if needed, then starts engine) --
$mlArgs = @{
    StartMap    = $Map
    EvalSeconds = $Duration
}
if ($Fullscreen) { $mlArgs['Fullscreen'] = $true }
Write-Host "[eval] launching: duration=$Duration map=$Map" -ForegroundColor Cyan
& (Join-Path $PSScriptRoot 'modlaunch.ps1') @mlArgs | Out-Host

# --- 3) wait for the game to exit, or force-kill past the margin ---------
if ($KeepRunning) {
    Write-Host "[eval] -KeepRunning set; leaving the game alive. Exiting harness." -ForegroundColor Yellow
    exit 0
}

# Give the engine time to boot and actually start the process before we
# look for it. Kex takes ~4-6s on a warm cache.
Start-Sleep -Seconds 6

$seen = Get-Process -Name quake2ex_steam -ErrorAction SilentlyContinue
if (-not $seen) {
    Write-Host "[eval] warning: quake2ex_steam not running 6s after launch; log capture may be empty" -ForegroundColor Yellow
}

# Poll until process exits or we pass the deadline (Duration + startup grace
# + flush margin).
$deadline = (Get-Date).AddSeconds($Duration + 20)
while ((Get-Date) -lt $deadline) {
    $proc = Get-Process -Name quake2ex_steam -ErrorAction SilentlyContinue
    if (-not $proc) { break }
    Start-Sleep -Seconds 1
}
$proc = Get-Process -Name quake2ex_steam -ErrorAction SilentlyContinue
if ($proc) {
    Write-Host "[eval] process did not auto-quit; force-killing" -ForegroundColor Yellow
    Stop-Process -Name quake2ex_steam -Force
    Start-Sleep -Seconds 2   # give the engine a moment to flush stdout.txt
}

# --- 4) collect output ---------------------------------------------------
$stdoutLines = @()
$stderrLines = @()
if (Test-Path -LiteralPath $StdoutPath) {
    # Copy into outFile so we keep a record independent of the game's next run
    Copy-Item -LiteralPath $StdoutPath -Destination $outFile -Force
    $stdoutLines = Get-Content -LiteralPath $StdoutPath
}
if (Test-Path -LiteralPath $StderrPath) {
    $stderrLines = Get-Content -LiteralPath $StderrPath
}
$crashed = Test-Path -LiteralPath $CrashLog

$modLines = $stdoutLines | Where-Object { $_ -match '\[(mymod|eval)\]' }
$evalLines = $modLines | Where-Object { $_ -match '^\[eval\]' }

# --- 5) parse the last telemetry line for a quick stats snapshot ---------
$last = $evalLines | Where-Object { $_ -match '\[eval\] t=' } | Select-Object -Last 1
$stats = [ordered]@{
    duration_s    = $Duration
    crashed       = $crashed
    stdout_lines  = $stdoutLines.Count
    mymod_lines   = $modLines.Count
    eval_lines    = $evalLines.Count
    log_file      = $outFile
}
if ($last -and $last -match 't=(?<t>[\d\.]+)\s+score=(?<s>-?\d+)\s+health=(?<h>-?\d+)\s+fire_ticks=(?<f>\d+)\s+target_ticks=(?<tt>\d+)\s+idle_ticks=(?<i>\d+)') {
    $stats['last_t']          = [double]$Matches.t
    $stats['last_score']      = [int]$Matches.s
    $stats['last_health']     = [int]$Matches.h
    $stats['fire_ticks']      = [int]$Matches.f
    $stats['target_ticks']    = [int]$Matches.tt
    $stats['idle_ticks']      = [int]$Matches.i
}

# --- 6) print summary ----------------------------------------------------
Write-Host ""
Write-Host "=== eval summary ===" -ForegroundColor Green
$stats.GetEnumerator() | ForEach-Object { Write-Host ("  {0,-14} {1}" -f $_.Key, $_.Value) }

if ($crashed) {
    Write-Host ""
    Write-Host "--- CRASHLOG.TXT head ---" -ForegroundColor Red
    Get-Content -LiteralPath $CrashLog -TotalCount 25 | ForEach-Object { Write-Host "  $_" }
}

if (-not $Quiet) {
    Write-Host ""
    Write-Host "--- mod log (last 30 lines) ---" -ForegroundColor Gray
    $modLines | Select-Object -Last 30 | ForEach-Object { Write-Host "  $_" }
}

# emit a machine-readable tail so a caller can parse without re-running
$json = $stats | ConvertTo-Json -Compress
Write-Host ""
Write-Host "JSON: $json"
