@echo off
setlocal
title RDR2 Coop Story - deinstalator
pushd "%~dp0"

set "GAME_PATH=%ProgramFiles(x86)%\Steam\steamapps\common\Red Dead Redemption 2"
if not "%~1"=="" set "GAME_PATH=%~1"

echo RDR2 COOP STORY - BEZPIECZNY DEINSTALATOR
echo.
echo Zamknij RDR2 przed kontynuowaniem.
echo Przed uruchomieniem Red Dead Online MUSISZ odinstalowac ten mod.
echo Deinstalator robi backup i usuwa tylko pliki nalezace do tej instalacji.
echo.

"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Easy-CoopStory.ps1" -Action Uninstall -GamePath "%GAME_PATH%"
set "RESULT=%ERRORLEVEL%"

echo.
if not "%RESULT%"=="0" echo Deinstalacja nie zostala zakonczona. Przeczytaj BLAD powyzej.
if "%RESULT%"=="0" echo Deinstalator zakonczyl prace. Sprawdz komunikaty UWAGA powyzej.
echo.
pause
popd
exit /b %RESULT%
