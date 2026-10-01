@echo off
setlocal EnableExtensions
set "UE_ENGINE=C:\Program Files\Epic Games\UE_5.6"
set "SCRIPT_DIR=%~dp0"
set "PROJECT_UPROJECT=%SCRIPT_DIR%SimTextureStrm.uproject"
set "EDITOR_EXE=%UE_ENGINE%\Engine\Binaries\Win64\UnrealEditor.exe"

if not exist "%PROJECT_UPROJECT%" (
  echo ERROR: Project file not found:
  echo   %PROJECT_UPROJECT%
  exit /b 1
)

echo Opening SimTextureStrm.uproject...

REM Env vars preserve paths with spaces for PowerShell (avoids broken -ArgumentList from cmd).
set "LAUNCH_EDITOR=%EDITOR_EXE%"
set "LAUNCH_PROJECT=%PROJECT_UPROJECT%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0LaunchProject.ps1"
if errorlevel 1 (
  echo ERROR: Launch script failed.
  exit /b 1
)
