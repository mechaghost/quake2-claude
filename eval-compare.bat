@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0eval-compare.ps1" %*
