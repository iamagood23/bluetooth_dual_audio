@echo off
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d %~dp0
cl /nologo /std:c++20 /EHsc /O2 /MT /DNOMINMAX /DUNICODE /D_UNICODE Explore.cpp /Fe:explore.exe ole32.lib mmdevapi.lib propsys.lib uuid.lib runtimeobject.lib
