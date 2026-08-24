@echo off
setlocal
cd /d "%~dp0"

set "DOTNET_EXE=%ProgramFiles%\dotnet\dotnet.exe"
if not exist "%DOTNET_EXE%" set "DOTNET_EXE=dotnet.exe"

"%DOTNET_EXE%" --list-runtimes 2>nul | findstr /B /C:"Microsoft.WindowsDesktop.App 10." >nul
if errorlevel 1 (
    echo.
    echo REQUIRED COMPONENT MISSING:
    echo Install Microsoft .NET 10 Desktop Runtime for Windows x64:
    echo https://dotnet.microsoft.com/en-us/download/dotnet/10.0
    echo.
    echo Run this file again after installation.
    echo.
    pause
    exit /b 2
)

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
