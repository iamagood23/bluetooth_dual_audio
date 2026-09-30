@echo off
rem Build and run the automated unit tests (no audio hardware needed).
setlocal
cd /d "%~dp0"
call "%~dp0build\msbuild.cmd" tests\DualAudioTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m /nologo /v:m || exit /b 1
"%~dp0bin\Release\DualAudioTests.exe"
exit /b %ERRORLEVEL%
