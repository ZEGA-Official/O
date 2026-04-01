@echo off
:: ============================================================
::  O Language Compiler - build.bat
::  Windows CMD build script
::  Z-TEAM
:: ============================================================
setlocal EnableDelayedExpansion

title O Lang Build

echo.
echo   O Language Compiler - Z-TEAM
echo   ================================
echo.

:: Find compiler
set CC=
for %%G in (
    gcc
    clang
    "C:\msys64\mingw64\bin\gcc.exe"
    "C:\msys64\usr\bin\gcc.exe"
    "C:\winlibs\mingw64\bin\gcc.exe"
    "C:\ProgramData\chocolatey\bin\gcc.exe"
) do (
    if "!CC!"=="" (
        %%G --version >nul 2>&1
        if !errorlevel! equ 0 set CC=%%G
    )
)

if "%CC%"=="" (
    echo [ERROR] No C compiler found.
    echo.
    echo Install one of:
    echo   1. MSYS2: https://www.msys2.org/
    echo      Then in MSYS2: pacman -S mingw-w64-x86_64-gcc
    echo   2. Scoop:  scoop install gcc
    echo   3. Choco:  choco install mingw
    echo   4. WinLibs: https://winlibs.com/
    echo.
    echo Or use WSL: wsl --install
    pause
    exit /b 1
)

echo [build] Compiler: %CC%

if /i "%1"=="clean" (
    del /f oc.exe 2>nul
    rmdir /s /q build 2>nul
    echo [build] Cleaned.
    exit /b 0
)

set SRCS=src\driver.c
for /r src\frontend %%F in (*.c) do set SRCS=!SRCS! "%%F"
for /r src\ir       %%F in (*.c) do set SRCS=!SRCS! "%%F"
for /r src\backend  %%F in (*.c) do set SRCS=!SRCS! "%%F"
for /r src\jit      %%F in (*.c) do set SRCS=!SRCS! "%%F"
for /r src\output   %%F in (*.c) do set SRCS=!SRCS! "%%F"

set FLAGS=-std=gnu2x -O2 -Iinclude -Wall -Wno-unused-parameter
set FLAGS=%FLAGS% -Wno-unused-function -Wno-unused-but-set-variable
set FLAGS=%FLAGS% -DNDEBUG -D_WIN32_WINNT=0x0600

if /i "%1"=="debug" (
    set FLAGS=-std=gnu2x -O0 -g3 -Iinclude -Wall -D_WIN32_WINNT=0x0600
)

echo [build] Compiling...
if not exist build mkdir build

%CC% %FLAGS% -o oc.exe %SRCS% -lws2_32 2>build\errors.txt

if %errorlevel% neq 0 (
    echo [ERROR] Build failed:
    type build\errors.txt
    exit /b 1
)

echo [build] Built oc.exe successfully!
echo.
echo Run it:
echo   oc.exe examples\hello.o --jit
echo   oc.exe examples\fib.o --jit
echo   oc.exe --help
echo.
