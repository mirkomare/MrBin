@echo off
title MrBin CORE - Build + Flash
set /p PORT="Porta COM (es. COM15): "
if "%PORT%"=="" (
  echo Porta non inserita.
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-core.ps1" -Port %PORT%
