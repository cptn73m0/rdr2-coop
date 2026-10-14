@echo off
title RDR2 CO-OP relay server
cd /d "%~dp0"

set "SERVER_EXE=build-server\Release\rdr2_coop_server.exe"
set "PORT=%1"
if "%PORT%"=="" set "PORT=7777"

if not exist "%SERVER_EXE%" (
    echo Server not built. Building...
    cmake -S server -B build-server -G "Visual Studio 16 2019" -A x64
    cmake --build build-server --config Release
)

if not exist "%SERVER_EXE%" (
    echo Build failed. Check that VS2019 Build Tools are installed.
    pause
    exit /b 1
)

echo Starting RDR2 CO-OP server on port %PORT%...
echo.
echo Logs are shown in this window and written to build-server\server_console.log
echo To stop the server, simply close this window (or press Ctrl+C).
echo.

"%SERVER_EXE%" %PORT%

echo.
echo Server stopped.
pause