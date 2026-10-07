@echo off
rem Builds FPS Overlay (Release x64), runs the unit tests and makes a portable folder + zip in dist\.
rem Needs Visual Studio 2022 or newer with the "Desktop development with C++" workload and
rem "C++/CLI support". No .NET developer pack is needed.
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio Installer not found. Install Visual Studio 2022 Build Tools or Community.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo No Visual Studio with the C++ tools was found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0"
echo Building...
msbuild FPSOverlay.sln /p:Configuration=Release /p:Platform=x64 /m /nologo /verbosity:minimal || goto :failed

echo.
echo Running unit tests...
build\Release\FPSOverlay.Tests.exe || goto :failed

echo.
echo Packaging...
set "OUT=dist\FPSOverlay"
if exist dist rmdir /s /q dist
mkdir "%OUT%\locales"
copy /y build\Release\FPSOverlay.exe "%OUT%\" >nul
copy /y build\Release\FPSOverlay.Sensors.dll "%OUT%\" >nul
copy /y build\Release\lhwm-wrapper.dll "%OUT%\" >nul
copy /y LICENSE.txt "%OUT%\" >nul
copy /y NOTICE.md "%OUT%\" >nul
copy /y locales\*.json "%OUT%\locales\" >nul
powershell -NoProfile -Command "Compress-Archive -Path 'dist\FPSOverlay' -DestinationPath 'dist\FPSOverlay.zip' -Force" || goto :failed

echo.
echo Done: dist\FPSOverlay\FPSOverlay.exe  (and dist\FPSOverlay.zip)
exit /b 0

:failed
echo.
echo BUILD FAILED
exit /b 1
