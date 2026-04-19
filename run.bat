@echo off
REM Launch the game with the current mod. No build, no output. Double-click it.
setlocal enabledelayedexpansion
set /p MODNAME=<"%~dp0modname.txt"
start "" "D:\SteamLibrary\steamapps\common\Quake 2\rerelease\quake2ex_steam.exe" +set game !MODNAME!
