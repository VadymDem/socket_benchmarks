@echo off
rem Збірка бенчмарку сокетів (Windows/MinGW-w64 GCC)
rem Потрібен gcc у PATH або вказати шлях явно.
if "%GCC%"=="" set GCC=gcc
%GCC% -O2 -Wall -Wextra -o sockets_bench.exe sockets_bench.c -lws2_32
if errorlevel 1 (
    echo Помилка збірки!
    exit /b 1
)
echo OK: sockets_bench.exe