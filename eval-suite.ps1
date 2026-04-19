#requires -Version 5.1
<#
Baseline eval suite: runs a fixed set of scenarios, aggregates the results
into a single JSON report, and appends a line to eval-logs/suite-history.csv
so progress can be tracked commit-by-commit.

Usage:
  eval-suite.ps1                      # default suite (stationary, passive, baseline, hardmode)
  eval-suite.ps1 -Duration 20         # per-run duration
  eval-suite.ps1 -Runs 2              # repeats per scenario, averaged

The suite score is the sum of (avg_score per scenario). It's blunt but
trend-over-time useful — regressions in any scenario show up.
#>

[CmdletBinding()]
param(
    [int]$Duration = 20,
    [int]$Runs = 1,
    [string[]]$Scenarios = @('stationary','passive','baseline','hardmode'),
    [string]$OutDir = "$PSScriptRoot\eval-logs"
)

$ErrorActionPreference = 'Stop'

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stamp   = (Get-Date).ToString('yyyyMMdd-HHmmss')
$sha     = (git -C $PSScriptRoot rev-parse --short HEAD 2>$null) ; if (-not $sha) { $sha = 'nogit' }

$results = @()
foreach ($sc in $Scenarios) {
    Write-Host ""
    Write-Host "[suite] scenario=$sc duration=${Duration}s runs=$Runs" -ForegroundColor Magenta
    $json = & (Join-Path $PSScriptRoot 'eval.ps1') -Scenario $sc -Duration $Duration -Runs $Runs -Quiet 2>&1 |
            ForEach-Object { $_ } |
            Select-String -Pattern '^JSON: (.+)$' |
            ForEach-Object { $_.Matches[0].Groups[1].Value } |
            Select-Object -Last 1
    if (-not $json) { Write-Host "[suite] no JSON from $sc" -ForegroundColor Red; continue }
    $obj = $json | ConvertFrom-Json
    $results += [PSCustomObject]@{
        scenario   = $obj.scenario
        avg_score  = $obj.avg_score
        avg_health = $obj.avg_health
        avg_fire   = $obj.avg_fire
        avg_target = $obj.avg_target
        dm_invalid = $obj.dm_invalid
        crashes    = $obj.crashes
    }
}

Write-Host ""
Write-Host "=== suite summary ($sha @ $stamp) ===" -ForegroundColor Green
$fmt = '{0,-12} {1,9} {2,9} {3,9} {4,9} {5,5} {6,5}'
Write-Host ($fmt -f 'scenario','score','health','fire','target','dmNO','crash')
$total_score = 0
foreach ($r in $results) {
    Write-Host ($fmt -f $r.scenario, $r.avg_score, $r.avg_health, $r.avg_fire, $r.avg_target, $r.dm_invalid, $r.crashes)
    $total_score += [double]$r.avg_score
}
Write-Host ""
Write-Host ("total_score = {0}" -f $total_score) -ForegroundColor Green

# Append to history so we can track trends.
$historyFile = Join-Path $OutDir 'suite-history.csv'
if (-not (Test-Path -LiteralPath $historyFile)) {
    "timestamp,commit,duration,runs,scenario,avg_score,avg_health,avg_fire,avg_target,dm_invalid,crashes" | Out-File -LiteralPath $historyFile -Encoding ASCII
}
foreach ($r in $results) {
    "$stamp,$sha,$Duration,$Runs,$($r.scenario),$($r.avg_score),$($r.avg_health),$($r.avg_fire),$($r.avg_target),$($r.dm_invalid),$($r.crashes)" |
        Out-File -LiteralPath $historyFile -Encoding ASCII -Append
}
Write-Host "[suite] history appended to $historyFile"

# Machine-readable single-line tail for CI / bot consumption.
$payload = [ordered]@{
    timestamp   = $stamp
    commit      = $sha
    duration    = $Duration
    runs        = $Runs
    total_score = $total_score
    scenarios   = $results
}
Write-Host ""
Write-Host ("SUITE_JSON: " + ($payload | ConvertTo-Json -Compress))
