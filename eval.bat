@echo off
REM Double-click: run a 30s eval. Pass args through to eval.ps1 for custom runs.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0eval.ps1" %*
