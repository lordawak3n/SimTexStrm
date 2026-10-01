@echo off
setlocal EnableExtensions
set "UE_ENGINE=C:\Program Files\Epic Games\UE_5.6"
set "SCRIPT_DIR=%~dp0"
set "PROJECT_UPROJECT=%SCRIPT_DIR%SimTextureStrm.uproject"
set "BUILD_BAT=%UE_ENGINE%\Engine\Build\BatchFiles\Build.bat"

echo [BuildGame] Starting Unreal build...
echo [BuildGame] Engine: %UE_ENGINE%
echo [BuildGame] Project: %PROJECT_UPROJECT%
echo [BuildGame] Command: "%BUILD_BAT%" SimTextureStrmEditor Win64 Development -Project="%PROJECT_UPROJECT%" -WaitMutex -FromMsBuild -architecture=x64
echo.

if not exist "%BUILD_BAT%" (
  echo [BuildGame] ERROR: Build.bat not found at:
  echo [BuildGame]   %BUILD_BAT%
  exit /b 1
)

if not exist "%PROJECT_UPROJECT%" (
  echo [BuildGame] ERROR: .uproject not found at:
  echo [BuildGame]   %PROJECT_UPROJECT%
  exit /b 1
)

echo [BuildGame] Running UnrealBuildTool (this can pause at -WaitMutex)...
call "%BUILD_BAT%" SimTextureStrmEditor Win64 Development -Project="%PROJECT_UPROJECT%" -WaitMutex -FromMsBuild -architecture=x64
set "BUILD_EXIT=%ERRORLEVEL%"
echo.

if not "%BUILD_EXIT%"=="0" (
  echo [BuildGame] FAILED with exit code %BUILD_EXIT%.
  exit /b %BUILD_EXIT%
)

echo [BuildGame] SUCCESS.
exit /b 0
