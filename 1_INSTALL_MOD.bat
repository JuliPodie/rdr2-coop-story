@echo off
setlocal
title RDR2 Coop Story - installer
pushd "%~dp0"

set "GAME_PATH=%ProgramFiles(x86)%\Steam\steamapps\common\Red Dead Redemption 2"
if not "%~1"=="" set "GAME_PATH=%~1"

echo RDR2 COOP STORY - SAFE TEST INSTALLER
echo.
echo Close RDR2 before continuing.
echo The installer does not download files, does not require administrator rights,
echo and never copies NativeTrainer.asi.
echo.

"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Easy-CoopStory.ps1" -Action Install -GamePath "%GAME_PATH%"
set "RESULT=%ERRORLEVEL%"

echo.
if not "%RESULT%"=="0" echo Installation did not complete. Read the ERROR message above.
if "%RESULT%"=="0" echo Done. You can now run 2_RUN_TEST.bat.
echo.
pause
popd
exit /b %RESULT%
