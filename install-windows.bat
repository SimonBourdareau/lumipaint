@echo off
setlocal

rem  LumiPaint installer for Windows.
rem
rem  Copies both plugin formats into the per-user plugin folders. Per-user rather than
rem  Program Files deliberately: those locations need no administrator prompt, and every
rem  host that scans the machine-wide folders scans these as well. A plugin that installs
rem  without a UAC dialog is also a plugin nobody has to think twice about running.
rem
rem  The device program is not installed. It is flashed onto the keyboard through ROLI
rem  Dashboard, which no script can do for you.

set "CLAPDIR=%LOCALAPPDATA%\Programs\Common\CLAP"
set "VST3DIR=%LOCALAPPDATA%\Programs\Common\VST3"

echo.
echo   LumiPaint installer
echo   -------------------
echo.

if not exist "%~dp0LumiPaint.clap" (
    echo   ERROR: LumiPaint.clap is not next to this script.
    echo   Unzip the whole download into one folder and run it from there.
    echo.
    pause
    exit /b 1
)

if not exist "%~dp0LumiPaint.vst3\Contents\x86_64-win\LumiPaint.vst3" (
    echo   ERROR: the LumiPaint.vst3 folder is missing or incomplete.
    echo   Unzip the whole download - copying just the outer folder is not enough.
    echo.
    pause
    exit /b 1
)

echo   Installing CLAP  to %CLAPDIR%
if not exist "%CLAPDIR%" mkdir "%CLAPDIR%"
copy /y "%~dp0LumiPaint.clap" "%CLAPDIR%\" >nul

if errorlevel 1 (
    echo   FAILED. Is the plugin loaded in a DAW right now? Close it and try again.
    echo.
    pause
    exit /b 1
)

echo   Installing VST3  to %VST3DIR%
if exist "%VST3DIR%\LumiPaint.vst3" rmdir /s /q "%VST3DIR%\LumiPaint.vst3"
if not exist "%VST3DIR%" mkdir "%VST3DIR%"
xcopy /e /i /y /q "%~dp0LumiPaint.vst3" "%VST3DIR%\LumiPaint.vst3\" >nul

if errorlevel 1 (
    echo   FAILED. Is the plugin loaded in a DAW right now? Close it and try again.
    echo.
    pause
    exit /b 1
)

echo.
echo   Done. Rescan plugins in your DAW and LumiPaint will appear.
echo.
echo   One thing left, and nothing can do it for you:
echo.
echo     Open ROLI Dashboard and drag lumi_paint.littlefoot - in this folder -
echo     onto the picture of your keyboard. The keyboard needs firmware 1.3.0
echo     or later, and this replaces ROLI's own program until you factory reset.
echo.
echo   Until that is done the plugin loads but the keyboard will not light.
echo.
pause
