@echo off
title MrBin CORE - Build
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-core.ps1"
echo.
pause
