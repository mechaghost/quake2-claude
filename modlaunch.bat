@echo off
REM Double-clickable entry point. Pass any PS args, e.g. modlaunch.bat -Build
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0modlaunch.ps1" %*
