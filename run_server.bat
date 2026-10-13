@echo off
chcp 65001 >nul
title RDR2 CO-OP — сервер-релей
cd /d "%~dp0"

set SERVER_EXE=build-server\Release\rdr2_coop_server.exe
set PORT=%1
if "%PORT%"=="" set PORT=7777

if not exist "%SERVER_EXE%" (
    echo Сервер не собран. Собираю...
    cmake -S server -B build-server -G "Visual Studio 16 2019" -A x64
    cmake --build build-server --config Release
)

if not exist "%SERVER_EXE%" (
    echo Не удалось собрать сервер. Проверьте, установлен ли Visual Studio 2019 Build Tools.
    pause
    exit /b 1
)

echo Запускаю сервер RDR2 CO-OP на порту %PORT%...
echo.
"%SERVER_EXE%" %PORT%

echo.
echo Сервер остановлен.
pause
