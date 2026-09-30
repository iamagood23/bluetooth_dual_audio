@echo off
rem Creates dist\DualAudio\ (portable release folder) and dist\DualAudio-1.0.0-win-x64.zip.
rem Nothing is installed on the system; the folder can be copied anywhere and run.
setlocal
cd /d "%~dp0"
call "%~dp0build.bat" || exit /b 1
call "%~dp0test.bat" || (echo Tests failed - not packaging. & exit /b 1)

set OUT=dist\DualAudio
if exist dist rmdir /s /q dist
mkdir "%OUT%\config" "%OUT%\logs" "%OUT%\tools" "%OUT%\docs" || exit /b 1
copy /y bin\Release\DualAudio.exe "%OUT%\" >nul || exit /b 1
copy /y bin\Release\DualAudioCli.exe "%OUT%\tools\" >nul || exit /b 1
copy /y README.txt "%OUT%\" >nul || exit /b 1
copy /y docs\*.md "%OUT%\docs\" >nul
echo Settings are created here on first run.> "%OUT%\config\README.txt"
echo Log files are written here.> "%OUT%\logs\README.txt"
powershell -NoProfile -Command "Compress-Archive -Path 'dist\DualAudio' -DestinationPath 'dist\DualAudio-1.0.0-win-x64.zip' -Force" || exit /b 1
echo.
echo Package ready: %OUT%  and  dist\DualAudio-1.0.0-win-x64.zip
