@echo off
rem Builds a test copy of the app for UI measurements: FPSO_TEST_INSTANCE (own window classes,
rem mutex and ETW session, so it runs beside an installed copy, and logs "settings:" and "loop:"
rem lines once a second) and FpsoAsInvoker (no admin prompt, so scripts can send it input).
rem Output: build\Release-user\. Run build.bat once first: it needs build\Release\FPSOverlay.Sensors.dll.
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo No Visual Studio with the C++ tools was found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0..\.."
rem cl.exe reads CL from the environment; MSBuild does not notice it change, hence /t:Rebuild.
set "CL=/DFPSO_TEST_INSTANCE"
msbuild FPSOverlay.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:FpsoAsInvoker=true /p:SolutionDir=%CD%\ /m /nologo /verbosity:minimal
