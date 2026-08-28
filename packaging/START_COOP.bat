@echo off
setlocal
cd /d "%~dp0"

if not exist "CoopStory.Launcher.exe" (
    echo.
    echo ERROR: CoopStory.Launcher.exe was not found in this folder.
    echo Extract the complete ZIP to a normal folder and try again.
    echo.
    pause
    exit /b 1
)

start "" "CoopStory.Launcher.exe"
exit /b 0
