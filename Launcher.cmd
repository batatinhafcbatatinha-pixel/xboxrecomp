@echo off
rem Opens the launcher: choose your game disc image (.iso); it installs what the build needs, builds the game
rem from your disc and starts it. Launcher.exe comes from the port-installer releases (source: tools\port-installer).
rem "Launcher.cmd --update" downloads the newest launcher again.
setlocal
set "EXE=%~dp0.local\launcher\Launcher.exe"
if /i "%~1"=="--update" if exist "%EXE%" del "%EXE%"
if not exist "%EXE%" (
  echo Downloading the launcher...
  if not exist "%~dp0.local\launcher" mkdir "%~dp0.local\launcher"
  curl.exe -L --fail -o "%EXE%" https://github.com/batatinhafcbatatinha-pixel/port-installer/releases/latest/download/Launcher.exe
  if errorlevel 1 (
    echo Download failed. Get Launcher.exe from https://github.com/batatinhafcbatatinha-pixel/port-installer/releases
    echo and put it in "%~dp0.local\launcher\".
    pause
    exit /b 1
  )
)
start "" "%EXE%" --root "%~dp0."
