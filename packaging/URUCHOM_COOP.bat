@echo off
setlocal
cd /d "%~dp0"

set "DOTNET_EXE=%ProgramFiles%\dotnet\dotnet.exe"
if not exist "%DOTNET_EXE%" (
    set "DOTNET_EXE=dotnet.exe"
)

"%DOTNET_EXE%" --list-runtimes 2>nul | findstr /B /C:"Microsoft.WindowsDesktop.App 10." >nul
if errorlevel 1 (
    echo.
    echo BRAK WYMAGANEGO PROGRAMU:
    echo Zainstaluj Microsoft .NET 10 Desktop Runtime dla Windows x64:
    echo https://dotnet.microsoft.com/en-us/download/dotnet/10.0
    echo.
    echo Po instalacji uruchom ten plik ponownie.
    echo.
    pause
    exit /b 2
)

if not exist "CoopStory.Launcher.exe" (
    echo.
    echo BLAD: Nie znaleziono CoopStory.Launcher.exe w tym folderze.
    echo Rozpakuj caly ZIP do zwyklego folderu i sprobuj ponownie.
    echo.
    pause
    exit /b 1
)

start "" "CoopStory.Launcher.exe"
exit /b 0
