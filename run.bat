@echo off
rem Launch DualAudio (Release build; builds it first if missing).  run.bat debug -> Debug build
setlocal
cd /d "%~dp0"
set CFG=Release
if /i "%~1"=="debug" set CFG=Debug
if not exist "bin\%CFG%\DualAudio.exe" (
  call "%~dp0build.bat" %CFG% || exit /b 1
)
start "" "bin\%CFG%\DualAudio.exe"
