@echo off
rem izukidio build script - VS2022 MSBuild (amd64) discovered via vswhere.
rem Why: WDK 26100 ships only Microsoft.DriverKit.Build.Tasks.17.0.dll, so the
rem VS2026 (v18) MSBuild always fails with "ValidateNTTargetVersion could not be
rem loaded ... 18.0.dll". Also the 32-bit MSBuild breaks InfVerif (no x86 dll).
rem WDK 28000.2526 is required for native VS2026 IDE driver builds.
setlocal
set "MSBUILD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -version "[17.0,18.0)" -property installationPath`) do (
        if exist "%%i\MSBuild\Current\Bin\amd64\MSBuild.exe" set "MSBUILD=%%i\MSBuild\Current\Bin\amd64\MSBuild.exe"
    )
)
if not defined MSBUILD set "MSBUILD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"
if not exist "%MSBUILD%" (
    echo ERROR: VS2022 amd64 MSBuild not found at "%MSBUILD%"
    exit /b 1
)
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

rem Both artifacts land in build\<CONFIG>\ (SolutionDir forced since we build
rem .vcxproj directly - without it MSBuild defaults SolutionDir to the project dir).
rem Forward slashes: a trailing backslash before the closing quote gets eaten
rem by cmd argv parsing (\" = literal quote) and corrupts the property value.
set "SOLDIR=%~dp0"
set "SOLDIR=%SOLDIR:\=/%"
"%MSBUILD%" "%~dp0izukidio\izukidio.vcxproj" /p:Configuration=%CONFIG% /p:Platform=x64 /p:SolutionDir="%SOLDIR%" /t:Rebuild /v:m || exit /b 1
"%MSBUILD%" "%~dp0izukiloader\izukiloader.vcxproj" /p:Configuration=%CONFIG% /p:Platform=x64 /p:SolutionDir="%SOLDIR%" /t:Rebuild /v:m || exit /b 1
