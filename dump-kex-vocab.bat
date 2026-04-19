@echo off
REM Regenerate docs\kex-cmds.txt and docs\kex-cvars.txt by asking the engine
REM to dump its full cmdlist + cvarlist on boot, then exit. Run this after
REM an engine update to refresh the reference.
setlocal
set "STDOUT=%USERPROFILE%\Saved Games\Nightdive Studios\Quake II\stdout.txt"
set "EXE=D:\SteamLibrary\steamapps\common\Quake 2\rerelease\quake2ex_steam.exe"
del /q "%STDOUT%" 2>nul
start "" "%EXE%" +set game Ultron +set v_windowmode 0 +set v_width 640 +set v_height 480 +set g_showintromovie 0 +cmdlist +cvarlist +wait +quit
powershell -NoProfile -Command "Start-Sleep -Seconds 12; Get-Process quake2ex_steam -EA SilentlyContinue | Stop-Process -Force" 2>nul
powershell -NoProfile -Command ^
    "$f = Get-Content -LiteralPath '%STDOUT%'; ^
     $cmdStart = [array]::IndexOf($f, 'say_team') + 1; ^
     $cmdEnd   = ($f | Select-String -Pattern '^\d+ commands$').LineNumber; ^
     $cvarEnd  = ($f | Select-String -Pattern '^\d+ cvars$').LineNumber; ^
     $f[($cmdStart-1)..($cmdEnd-2)] | Set-Content -LiteralPath '%~dp0docs\kex-cmds.txt'; ^
     $f[$cmdEnd..($cvarEnd-2)]      | Set-Content -LiteralPath '%~dp0docs\kex-cvars.txt'; ^
     Write-Host \"cmds: $($cmdEnd-$cmdStart) lines, cvars: $($cvarEnd-$cmdEnd-1) lines\""
