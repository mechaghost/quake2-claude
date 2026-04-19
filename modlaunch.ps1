#requires -Version 5.1
<#
Build and launch this mod against Quake II Rerelease.

  - Source of truth: .\rerelease\ in this repo.
  - Mod name: read from .\modname.txt (change it to rename your mod).
  - DLL drop target: %USERPROFILE%\Saved Games\Nightdive Studios\Quake II\<modname>\game_x64.dll
    (the engine only loads mods from there).

Usage:
  modlaunch.ps1                 # incremental rebuild if needed, then launch
  modlaunch.ps1 -Build          # force full rebuild
  modlaunch.ps1 -NoBuild        # skip build, just launch
  modlaunch.ps1 -NoLaunch       # build only
  modlaunch.ps1 -Clean          # wipe build intermediates + staged DLL
  modlaunch.ps1 -Config Debug   # Debug instead of Release
#>

[CmdletBinding()]
param(
    [switch]$Build,
    [switch]$NoBuild,
    [switch]$NoLaunch,
    [switch]$Clean,
    [switch]$WithIntro,        # default: skip intros by passing +map on launch
    [switch]$Fullscreen,       # default: windowed (no mouse grab)
    [int]$EvalSeconds = 0,     # if >0, mod auto-quits after N seconds
    [string]$StartMap = 'q2dm1',
    [int]$FragLimit = 20,
    [int]$BotSkill  = -1,      # 0..3 (Kex range); -1 = leave default
    [ValidateSet('', 'normal','stationary','passive','noaim','deaf','instant','crippled')]
    [string]$EnemyMode = '',   # preset ablations for the engine bot
    [switch]$DebugBot,         # enable bot_debugSystem/draw cvars + ultron_bot_debug
    [hashtable]$Cvars = @{},   # arbitrary extra cvars: @{ultron_bot_fire_cone=3}
    [ValidateSet('Release','Debug')]
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'

$RepoRoot  = Split-Path -Parent $MyInvocation.MyCommand.Path
$SrcDir    = Join-Path $RepoRoot 'rerelease'
$Sln       = Join-Path $SrcDir   'game.sln'
$ModName   = (Get-Content -LiteralPath (Join-Path $RepoRoot 'modname.txt') -Raw).Trim()
$ModDir    = Join-Path ([Environment]::GetFolderPath('UserProfile')) "Saved Games\Nightdive Studios\Quake II\$ModName"
$DllPath   = Join-Path $ModDir 'game_x64.dll'
$Engine    = 'D:\SteamLibrary\steamapps\common\Quake 2\rerelease\quake2ex_steam.exe'
$MSBuild   = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
$VcpkgRoot = 'D:\dev\vcpkg'

function Get-NewestSource {
    Get-ChildItem -LiteralPath $SrcDir -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Extension -in '.cpp','.h','.hpp' -and
            $_.FullName -notmatch '\\(x64|Debug|Release|vcpkg_installed)\\'
        } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

function Test-NeedsBuild {
    if (-not (Test-Path -LiteralPath $DllPath)) { return $true }
    $newest = Get-NewestSource
    if (-not $newest) { return $false }
    $dllT = (Get-Item -LiteralPath $DllPath).LastWriteTime
    Write-Verbose "Newest src: $($newest.Name) @ $($newest.LastWriteTime)   DLL @ $dllT"
    return $newest.LastWriteTime -gt $dllT
}

function Invoke-Build {
    if (-not (Test-Path -LiteralPath $MSBuild)) { throw "MSBuild not found at $MSBuild" }
    if (-not (Test-Path -LiteralPath $Sln))     { throw "Solution not found at $Sln" }
    New-Item -ItemType Directory -Force -Path $ModDir | Out-Null
    $env:VCPKG_ROOT = $VcpkgRoot
    Write-Host "Building '$ModName' [$Config] -> $ModDir" -ForegroundColor Cyan
    # MSBuild wants OutDir/IntDir to end with a separator, but a trailing backslash
    # before the closing quote escapes the quote in cmd.exe. Use forward slashes.
    $outArg = ($ModDir -replace '\\','/') + '/'
    $intArg = ((Join-Path $SrcDir "x64\$Config") -replace '\\','/') + '/'
    & $MSBuild $Sln -m -nologo -v:minimal "-p:Configuration=$Config" -p:Platform=x64 `
        -p:VcpkgEnableManifest=true "-p:OutDir=$outArg" "-p:IntDir=$intArg"
    if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)" }
}

function Invoke-Clean {
    foreach ($dir in @('x64','Debug','Release','vcpkg_installed')) {
        $p = Join-Path $SrcDir $dir
        if (Test-Path -LiteralPath $p) {
            Write-Host "Removing $p" -ForegroundColor DarkGray
            Remove-Item -LiteralPath $p -Recurse -Force
        }
    }
    if (Test-Path -LiteralPath $DllPath) {
        Write-Host "Removing $DllPath" -ForegroundColor DarkGray
        Get-ChildItem -LiteralPath $ModDir -Filter 'game_x64.*' | Remove-Item -Force
    }
}

if ($Clean) { Invoke-Clean; if (-not $Build) { exit 0 } }

if (-not $NoBuild) {
    if ($Build -or (Test-NeedsBuild)) {
        Invoke-Build
    } else {
        Write-Host "DLL is up to date; skipping build (use -Build to force)." -ForegroundColor DarkGray
    }
}

if ($NoLaunch) { exit 0 }

if (-not (Test-Path -LiteralPath $Engine))  { throw "Engine not found at $Engine" }
if (-not (Test-Path -LiteralPath $DllPath)) { throw "No DLL at $DllPath - build first." }

$launchArgs = @('+set','game',$ModName)
if (-not $WithIntro) {
    # Kex skips the studio/logo intros when a map is specified on the CLI.
    # We also pre-seal deathmatch=1 so the first map load is DM, not SP.
    $launchArgs += @('+set','deathmatch','1','+map',$StartMap)
}
if (-not $Fullscreen) {
    # Kex uses v_* cvars for video. v_windowmode: 0=windowed, 1=fullscreen
    # (confirmed by reading the user's kexengine.cfg). Small dev window so
    # the mouse is free and we can see the console / editor alongside.
    # g_nativeMouse=1 switches to the OS cursor so it's not captured by the
    # engine in windowed mode. in_nograb=1 is a Quake-convention fallback in
    # case the build honors it. The bot drives the view via delta_angles so
    # we don't need mouse input at all.
    $launchArgs += @(
        '+set','v_windowmode','0',
        '+set','v_width',  '800',
        '+set','v_height', '600',
        '+set','g_showintromovie','0',
        '+set','g_nativeMouse','1',
        '+set','in_nograb','1'
    )
}
if ($EvalSeconds -gt 0) {
    $launchArgs += @('+set','ultron_eval_seconds',"$EvalSeconds")
}
if ($FragLimit -gt 0) {
    $launchArgs += @('+set','fraglimit',"$FragLimit")
}
if ($BotSkill -ge 0) {
    $launchArgs += @('+set','bot_skill',"$BotSkill")
}

# Enemy ablation presets. Mapping is based on the Kex bot_* cvar semantics
# documented in docs/kex-vocab-guide.md. 'crippled' stacks several for an
# easy-mode opponent; 'instant' is hard-mode (perfect aim).
$enemyPresets = @{
    'normal'     = @{}
    'stationary' = @{ 'bot_move_disable'  = 1 }
    'passive'    = @{ 'bot_combatDisabled' = 1 }
    'noaim'      = @{ 'bot_aim_disabled'   = 1 }
    'deaf'       = @{ 'bot_senses_disabled'= 1 }
    'instant'    = @{ 'bot_aim_instant'    = 1 }
    'crippled'   = @{ 'bot_aim_disabled'   = 1; 'bot_senses_disabled' = 1; 'bot_move_disable' = 1 }
}
if ($EnemyMode -and $enemyPresets.ContainsKey($EnemyMode)) {
    foreach ($k in $enemyPresets[$EnemyMode].Keys) {
        $launchArgs += @('+set', $k, "$($enemyPresets[$EnemyMode][$k])")
    }
}

if ($DebugBot) {
    $launchArgs += @(
        '+set','ultron_bot_debug','1',
        '+set','bot_debugSystem','1',
        '+set','con_showfps','1'
    )
}

# Arbitrary extras (e.g. @{ultron_bot_fire_cone=3; ultron_bot_no_strafe=1}).
foreach ($k in $Cvars.Keys) {
    $launchArgs += @('+set', "$k", "$($Cvars[$k])")
}

Write-Host "Launching: quake2ex_steam.exe $($launchArgs -join ' ')" -ForegroundColor Cyan
Start-Process -FilePath $Engine -ArgumentList $launchArgs
