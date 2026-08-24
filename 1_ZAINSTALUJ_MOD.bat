@echo off
setlocal
title RDR2 Coop Story - instalator
pushd "%~dp0"

set "GAME_PATH=%ProgramFiles(x86)%\Steam\steamapps\common\Red Dead Redemption 2"
if not "%~1"=="" set "GAME_PATH=%~1"

echo RDR2 COOP STORY - BEZPIECZNY INSTALATOR TESTOWY
echo.
echo Zamknij RDR2 przed kontynuowaniem.
echo Instalator nie pobiera plikow, nie wymaga administratora
echo i nigdy nie kopiuje NativeTrainer.asi.
echo.

"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Easy-CoopStory.ps1" -Action Install -GamePath "%GAME_PATH%"
set "RESULT=%ERRORLEVEL%"

echo.
if not "%RESULT%"=="0" echo Instalacja nie zostala zakonczona. Przeczytaj BLAD powyzej.
if "%RESULT%"=="0" echo Gotowe. Teraz uruchom 2_URUCHOM_TEST.bat.
echo.
pause
popd
exit /b %RESULT%
