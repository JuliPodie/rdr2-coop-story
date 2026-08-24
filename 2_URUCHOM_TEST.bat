@echo off
setlocal
title RDR2 Coop Story - test lokalny
pushd "%~dp0"

set "GAME_PATH=%ProgramFiles(x86)%\Steam\steamapps\common\Red Dead Redemption 2"
if not "%~1"=="" set "GAME_PATH=%~1"

echo RDR2 COOP STORY - LOKALNY SMOKE TEST
echo.
echo Ten launcher uruchomi syntetycznego drugiego gracza i otworzy gre przez Steam.
echo W grze wybierz WYLACZNIE STORY MODE. Nie wchodz do Red Dead Online.
echo.

"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Easy-CoopStory.ps1" -Action Launch -GamePath "%GAME_PATH%"
set "RESULT=%ERRORLEVEL%"

echo.
if not "%RESULT%"=="0" echo Test nie zostal uruchomiony. Przeczytaj BLAD powyzej.
echo.
pause
popd
exit /b %RESULT%
