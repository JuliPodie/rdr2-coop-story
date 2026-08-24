@echo off
setlocal
title RDR2 Coop Story - local test
pushd "%~dp0"

set "GAME_PATH=%ProgramFiles(x86)%\Steam\steamapps\common\Red Dead Redemption 2"
if not "%~1"=="" set "GAME_PATH=%~1"

echo RDR2 COOP STORY - LOCAL SMOKE TEST
echo.
echo This launcher starts a synthetic second player and opens the game through Steam.
echo Select STORY MODE ONLY. Never enter Red Dead Online with the mod installed.
echo.

"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Easy-CoopStory.ps1" -Action Launch -GamePath "%GAME_PATH%"
set "RESULT=%ERRORLEVEL%"

echo.
if not "%RESULT%"=="0" echo The test did not start. Read the ERROR message above.
echo.
pause
popd
exit /b %RESULT%
