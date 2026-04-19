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
    # (confirmed from kexengine.cfg). Small dev window so the mouse is free
    # and we can see the console / editor alongside.
    #
    # IMPORTANT: kexengine.cfg is loaded AFTER CLI "+set" commands, so a
    # previously-persisted g_nativeMouse=0 would re-trap the mouse despite
    # our launch arg. Using "+seta" (set + archive) forces our value through
    # AND writes it back on exit so it stays fixed. Applied to the other
    # windowed-mode cvars for the same reason.
    $launchArgs += @(
        '+seta','v_windowmode','0',
        '+seta','v_width',  '800',
        '+seta','v_height', '600',
        '+seta','g_showintromovie','0',
        '+seta','g_nativeMouse','1',
        # Kill mouse input entirely so the user's hand on the mouse never
        # rotates the view (client-side prediction otherwise tilts the
        # camera before the server corrects back). Ultron aims via the
        # server's delta_angles, so zero mouse sensitivity is the clean
        # config for demo mode.
        '+seta','sensitivity','0',
        '+seta','m_pitch','0',
        '+seta','m_yaw','0',
        '+seta','m_side','0',
        '+seta','m_forward','0',
        '+seta','cl_mousesmooth','0',
        '+seta','freelook','0'
    )
    # Also overwrite the persisted values directly, belt-and-suspenders, in
    # case the engine reads these configs before the CLI queue executes.
    # kexengine.cfg holds "seta" cvars; system.cfg holds "set" cvars
    # including the legacy mouse multipliers (sensitivity / m_pitch / ...).
    $qiiDir = Join-Path ([Environment]::GetFolderPath('UserProfile')) 'Saved Games\Nightdive Studios\Quake II'
    $kex    = Join-Path $qiiDir 'kexengine.cfg'
    $sys    = Join-Path $qiiDir 'system.cfg'

    if (Test-Path -LiteralPath $kex) {
        $txt = Get-Content -LiteralPath $kex -Raw
        $new = $txt -replace 'seta g_nativeMouse "0"', 'seta g_nativeMouse "1"' `
                    -replace 'seta v_windowmode "1"',  'seta v_windowmode "0"' `
                    -replace 'seta cl_mousesmooth "1"','seta cl_mousesmooth "0"'
        if ($new -ne $txt) { Set-Content -LiteralPath $kex -Value $new -NoNewline }
    }

    # Strip menu-triggering keybinds from the mod's keybinds.cfg so
    # Escape / F1-F10 / right-click don't summon UI and steal input.
    $kbd = Join-Path $qiiDir "$ModName\keybinds.cfg"
    if (Test-Path -LiteralPath $kbd) {
        $txt = Get-Content -LiteralPath $kbd -Raw
        # Remove any line that references togglemenu, menu_main, menu_*, etc.
        # — those are the engine commands the escape-key binding calls.
        $new = ($txt -split "`n" | Where-Object {
            $_ -notmatch '(?i)(togglemenu|menu_main|menu_|score)'
        }) -join "`n"
        if ($new -ne $txt) {
            Set-Content -LiteralPath $kbd -Value $new -NoNewline
        }
    }

    # Zero the legacy mouse multipliers in system.cfg so client-side
    # prediction never rotates the view from user input.
    if (Test-Path -LiteralPath $sys) {
        $txt = Get-Content -LiteralPath $sys -Raw
        $new = $txt `
            -replace 'set sensitivity "[^"]*"',   'set sensitivity "0"' `
            -replace 'set hsensitivity "[^"]*"',  'set hsensitivity "0"' `
            -replace 'set m_pitch "[^"]*"',       'set m_pitch "0"'
        if ($new -notmatch 'set m_yaw ')      { $new += "`nset m_yaw `"0`"`n" }
        if ($new -notmatch 'set m_side ')     { $new += "`nset m_side `"0`"`n" }
        if ($new -notmatch 'set m_forward ')  { $new += "`nset m_forward `"0`"`n" }
        if ($new -ne $txt) { Set-Content -LiteralPath $sys -Value $new -NoNewline }
        Write-Host "[modlaunch] patched kex + system configs for hands-off mode" -ForegroundColor DarkGray
    }
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
