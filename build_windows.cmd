@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

set "COMMONLIB_DIR=%SCRIPT_DIR%\external\commonlibsf"
set "SFSE_DIR=%SCRIPT_DIR%\external\sfse"

if not exist "%COMMONLIB_DIR%\include\RE\" (
    echo [ERROR] Missing CommonLibSF: "%COMMONLIB_DIR%"
    exit /b 2
)

if not exist "%SFSE_DIR%\sfse\" (
    echo [ERROR] Missing SFSE headers: "%SFSE_DIR%"
    exit /b 3
)

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%

cd /d "%SCRIPT_DIR%"
if errorlevel 1 exit /b %errorlevel%

cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b %errorlevel%

cmake --build build-vs --config Release
exit /b %errorlevel%
