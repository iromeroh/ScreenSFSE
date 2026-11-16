@echo off
setlocal
where cl.exe >nul 2>&1
if errorlevel 1 (
    echo Run this from an x64 Native Tools Command Prompt for Visual Studio 2022.
    exit /b 2
)
set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
cmake -S "%SCRIPT_DIR%" -B "%SCRIPT_DIR%\build-vs" -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b %errorlevel%
cmake --build "%SCRIPT_DIR%\build-vs" --config Release --target ScreenSFSEPlugin --parallel 4
exit /b %errorlevel%
