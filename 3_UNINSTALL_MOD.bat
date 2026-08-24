@echo off
setlocal
title RDR2 Coop Story - uninstaller
pushd "%~dp0"

set "GAME_PATH=%ProgramFiles(x86)%\Steam\steamapps\common\Red Dead Redemption 2"
if not "%~1"=="" set "GAME_PATH=%~1"

echo RDR2 COOP STORY - SAFE UNINSTALLER
echo.
echo Close RDR2 before continuing.
echo You MUST uninstall this mod before starting Red Dead Online.
echo The uninstaller creates a backup and removes only files owned by this installation.
echo.

"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Easy-CoopStory.ps1" -Action Uninstall -GamePath "%GAME_PATH%"
set "RESULT=%ERRORLEVEL%"

echo.
if not "%RESULT%"=="0" echo Uninstallation did not complete. Read the ERROR message above.
if "%RESULT%"=="0" echo Uninstallation completed. Review any WARNING messages above.
echo.
pause
popd
exit /b %RESULT%
