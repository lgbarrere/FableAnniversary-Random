@echo off

set "SOLUTION=.\dinput8\dinput8.slnx"
set "BUILD_DLL=.\dinput8\Release\dinput8.dll"
set "DEFAULT_DIR_FTLC_STEAM=%ProgramFiles(x86)%\Steam\steamapps\common\Fable The Lost Chapters"
set "FABLE_EXE=Fable.exe"

:: ----------------------
:: Build the DLL
:: ----------------------

:: Locate MSBuild via vswhere (works for VS 2017+)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%VSWHERE%" (
    echo vswhere.exe not found. Please install Visual Studio Build Tools.
    pause
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
    set "MSBUILD=%%i"
)

if not defined MSBUILD (
    echo MSBuild not found. Please install Visual Studio Build Tools.
    pause
    exit /b 1
)

echo Building dinput8.dll (Release^|x86)...
"%MSBUILD%" "%SOLUTION%" /p:Configuration=Release /p:Platform=x86 /v:minimal /nologo
if errorlevel 1 (
    echo Build failed!
    pause
    exit /b 1
)
echo Build succeeded.

:: ----------------------
:: Deploy the DLL
:: ----------------------

if not exist "%BUILD_DLL%" (
    echo Build DLL not found!
    pause
    exit /b 1
)

if not exist "%DEFAULT_DIR_FTLC_STEAM%" (
    echo Fable folder not found!
    pause
    exit /b 1
)

echo Copying %BUILD_DLL% to %DEFAULT_DIR_FTLC_STEAM%
copy /Y "%BUILD_DLL%" "%DEFAULT_DIR_FTLC_STEAM%\dinput8.dll"

echo Done.

if exist "%DEFAULT_DIR_FTLC_STEAM%\%FABLE_EXE%" (
    echo Launching Fable...
    start "" "%DEFAULT_DIR_FTLC_STEAM%\%FABLE_EXE%"
) else (
    echo Fable.exe not found!
)

exit
