@echo off
set "VSCMD_SKIP_SENDTELEMETRY=1"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 1
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "GEPATCH_VS=%%I"
if not defined GEPATCH_VS exit /b 1
call "%GEPATCH_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d %~dp0..
%*
