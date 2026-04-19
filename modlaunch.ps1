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
    # Kex exposes several fullscreen cvars; set all the common ones so
    # whichever the engine actually honors, we stay windowed and release
    # the mouse grab.
    $launchArgs += @(
        '+set','r_fullscreen','0',
        '+set','r_windowmode','0',
        '+set','vid_fullscreen','0',
        '+set','r_width',  '1280',
        '+set','r_height', '720'
    )
}
if ($EvalSeconds -gt 0) {
    $launchArgs += @('+set','mymod_eval_seconds',"$EvalSeconds")
}
Write-Host "Launching: quake2ex_steam.exe $($launchArgs -join ' ')" -ForegroundColor Cyan
Start-Process -FilePath $Engine -ArgumentList $launchArgs
