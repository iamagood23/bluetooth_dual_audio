@echo off
rem Build DualAudio.   build.bat            -> Release
rem                    build.bat debug      -> Debug
rem                    build.bat all        -> Debug and Release
rem Requires Visual Studio 2022 or Build Tools 2022 with "Desktop development with C++".
setlocal
cd /d "%~dp0"
set CONFIGS=Release
if /i "%~1"=="debug" set CONFIGS=Debug
if /i "%~1"=="all" set CONFIGS=Debug Release

if not exist "src\resources\app.ico" (
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-icon.ps1 || exit /b 1
)
for %%C in (%CONFIGS%) do (
  echo === Building %%C ===
  call "%~dp0build\msbuild.cmd" DualAudio.sln /p:Configuration=%%C /p:Platform=x64 /m /nologo /v:m || (
    echo BUILD FAILED ^(%%C^)
    exit /b 1
  )
)
echo.
echo Build succeeded (%CONFIGS%). Output folder: bin\^<Configuration^>\
exit /b 0
