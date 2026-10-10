@echo off
rem build.bat - build mrmod.dll. Release|Win32 only.

setlocal
set "PROJ=%~dp0mrmod.vcxproj"
set "OUT=%~dp0..\..\build\mrmod\mrmod.dll"

set "MSBUILD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -products * -version [15.0^,16.0^) -requires Microsoft.Component.MSBuild -property installationPath`) do (
        if exist "%%i\MSBuild\15.0\Bin\MSBuild.exe" set "MSBUILD=%%i\MSBuild\15.0\Bin\MSBuild.exe"
    )
)
if not defined MSBUILD (
    for %%e in (Professional Enterprise Community BuildTools) do (
        if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2017\%%e\MSBuild\15.0\Bin\MSBuild.exe" (
            set "MSBUILD=%ProgramFiles(x86)%\Microsoft Visual Studio\2017\%%e\MSBuild\15.0\Bin\MSBuild.exe"
        )
    )
)
if not defined MSBUILD (
    echo No Visual Studio 2017 MSBuild found - mrmod needs the v141 toolset.
    exit /b 1
)

"%MSBUILD%" "%PROJ%" /p:Configuration=Release /p:Platform=Win32 /v:minimal /nologo
if errorlevel 1 exit /b 1
if not exist "%OUT%" exit /b 1

rem A stale DLL is the quietest failure here, so print what was built.
for %%f in ("%OUT%") do echo   %%~ff  %%~zf bytes  %%~tf
exit /b 0
