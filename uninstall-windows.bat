@echo off
setlocal

rem  Removes what install-windows.bat put in place. The device program is not touched:
rem  to put the keyboard back as it was, factory reset it in ROLI Dashboard.

set "CLAPDIR=%LOCALAPPDATA%\Programs\Common\CLAP"
set "VST3DIR=%LOCALAPPDATA%\Programs\Common\VST3"

echo.
echo   Removing LumiPaint
echo.

if exist "%CLAPDIR%\LumiPaint.clap" (
    del /q "%CLAPDIR%\LumiPaint.clap"
    echo   removed the CLAP
) else (
    echo   no CLAP found
)

if exist "%VST3DIR%\LumiPaint.vst3" (
    rmdir /s /q "%VST3DIR%\LumiPaint.vst3"
    echo   removed the VST3
) else (
    echo   no VST3 found
)

echo.
echo   Done. To put the keyboard back as it was, factory reset it in ROLI Dashboard.
echo.
pause
