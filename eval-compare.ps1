#requires -Version 5.1
<#
Compare two eval logs (from eval-logs/) and print a diff of the key stats.

Usage:
  eval-compare.ps1 -A eval-baseline-20260418-224605.log -B eval-stationary-20260418-225010.log
  eval-compare.ps1 -Latest 2        # compare the two most recent logs
#>

[CmdletBinding()]
param(
    [string]$A,
    [string]$B,
    [int]$Latest = 0,
    [string]$Dir = "$PSScriptRoot\eval-logs"
)

function Parse-Log {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { throw "Log not found: $Path" }
    $lines = Get-Content -LiteralPath $Path
    $eval  = $lines | Where-Object { $_ -match '\[eval\]' }
    $last  = $eval  | Where-Object { $_ -match '\[eval\] t=' } | Select-Object -Last 1
    $stats = [ordered]@{ file = (Split-Path -Leaf $Path) }
    if ($last -and $last -match 't=(?<t>[\d\.]+)\s+score=(?<s>-?\d+)\s+health=(?<h>-?\d+)\s+fire_ticks=(?<f>\d+)\s+target_ticks=(?<tt>\d+)\s+idle_ticks=(?<i>\d+)') {
        $stats['t']       = [double]$Matches.t
        $stats['score']   = [int]$Matches.s
        $stats['health']  = [int]$Matches.h
        $stats['fire']    = [int]$Matches.f
        $stats['target']  = [int]$Matches.tt
        $stats['idle']    = [int]$Matches.i
    }
    return $stats
}

if ($Latest -ge 2) {
    $recent = Get-ChildItem -LiteralPath $Dir -Filter 'eval-*.log' -File | Sort-Object LastWriteTime -Descending | Select-Object -First $Latest
    if ($recent.Count -lt 2) { throw "Not enough logs in $Dir (need >=2)" }
    $B = $recent[0].FullName
    $A = $recent[1].FullName
}

if (-not $A -or -not $B) { throw "Specify -A and -B, or pass -Latest 2" }

# Resolve possibly-bare filenames relative to $Dir
foreach ($v in 'A','B') {
    $p = Get-Variable -Name $v -ValueOnly
    if (-not (Test-Path -LiteralPath $p)) {
        $candidate = Join-Path $Dir $p
        if (Test-Path -LiteralPath $candidate) { Set-Variable -Name $v -Value $candidate }
    }
}

$sa = Parse-Log $A
$sb = Parse-Log $B

Write-Host ""
Write-Host "A: $($sa.file)" -ForegroundColor Cyan
Write-Host "B: $($sb.file)" -ForegroundColor Cyan
Write-Host ""

$keys = @('score','health','fire','target','idle','t')
Write-Host ("{0,-10} {1,10} {2,10} {3,10} {4}" -f 'metric','A','B','delta','change')
Write-Host "-----------------------------------------------------------"
foreach ($k in $keys) {
    $va = if ($sa.Contains($k)) { $sa[$k] } else { '?' }
    $vb = if ($sb.Contains($k)) { $sb[$k] } else { '?' }
    if ($va -ne '?' -and $vb -ne '?') {
        $delta = $vb - $va
        $pct = if ($va -ne 0) { [math]::Round(($delta / [math]::Abs($va)) * 100, 1) } else { 'n/a' }
        $arrow = if ($delta -gt 0) { "+$delta" } elseif ($delta -lt 0) { "$delta" } else { '0' }
        $pctStr = if ($pct -eq 'n/a') { '' } else { " ($pct%)" }
        Write-Host ("{0,-10} {1,10} {2,10} {3,10} {4}" -f $k, $va, $vb, $arrow, $pctStr)
    } else {
        Write-Host ("{0,-10} {1,10} {2,10}" -f $k, $va, $vb)
    }
}
