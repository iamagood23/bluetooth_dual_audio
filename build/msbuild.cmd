@echo off
rem Locates Visual Studio / Build Tools (2022+) with the C++ workload and runs MSBuild.
rem Usage: build\msbuild.cmd <args...>
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: vswhere.exe not found. Install Visual Studio 2022 or Build Tools 2022 with "Desktop development with C++".
  exit /b 1
)
set "MSBUILD="
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
  if not defined MSBUILD set "MSBUILD=%%i"
)
if not defined MSBUILD (
  echo ERROR: MSBuild not found.
  exit /b 1
)
"%MSBUILD%" %*
exit /b %ERRORLEVEL%
