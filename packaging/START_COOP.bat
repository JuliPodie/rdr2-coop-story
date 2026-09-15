@echo off
setlocal
cd /d "%~dp0CoopStory"

if not exist "CoopStory.Launcher.exe" (
    echo.
    echo ERROR: CoopStory\CoopStory.Launcher.exe was not found.
    echo Extract the complete ZIP to a normal folder and try again.
    echo.
    pause
    exit /b 1
)

start "" "%~dp0CoopStory\CoopStory.Launcher.exe"
exit /b 0
