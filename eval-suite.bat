@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0eval-suite.ps1" %*
