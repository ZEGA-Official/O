# ============================================================
#  O Language Compiler - build.ps1
#  Windows PowerShell build script
#  Z-TEAM
# ============================================================
param(
    [string]$Target  = "release",
    [string]$CC      = "",
    [switch]$Verbose = $false
)

$ErrorActionPreference = "Stop"
$ESC = [char]27
function Green($s) { "$ESC[38;2;88;240;27m$s$ESC[0m" }
function Red($s)   { "$ESC[31m$s$ESC[0m" }
function Yellow($s){ "$ESC[33m$s$ESC[0m" }

$host.UI.RawUI.WindowTitle = "O Lang Build"
try {
    [System.Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public class ConsoleHelper {
    [DllImport("kernel32.dll")] public static extern bool SetConsoleMode(IntPtr h, uint m);
    [DllImport("kernel32.dll")] public static extern bool GetConsoleMode(IntPtr h, out uint m);
    [DllImport("kernel32.dll")] public static extern IntPtr GetStdHandle(int n);
}
"@
    $h = [ConsoleHelper]::GetStdHandle(-11); $mode = 0
    [ConsoleHelper]::GetConsoleMode($h, [ref]$mode) | Out-Null
    [ConsoleHelper]::SetConsoleMode($h, $mode -bor 4) | Out-Null
} catch {}

Write-Host (Green "  ██████  ██       █████  ███  ██  ██████")
Write-Host (Green " ██    ██ ██      ██   ██ ████ ██ ██     ")
Write-Host (Green " ██    ██ ██      ███████ ██ █████ ██  ███")
Write-Host (Green " ██    ██ ██      ██   ██ ██  ████ ██   ██")
Write-Host (Green "  ██████  ███████ ██   ██ ██   ███  ██████")
Write-Host (Green "  O Language Compiler v1.0.0 -- Z-TEAM")
Write-Host ""

if ($CC -eq "") {
    $candidates = @(
        "gcc", "clang",
        "C:\msys64\mingw64\bin\gcc.exe",
        "C:\msys64\usr\bin\gcc.exe",
        "C:\winlibs\mingw64\bin\gcc.exe",
        "C:\ProgramData\chocolatey\bin\gcc.exe"
    )
    foreach ($c in $candidates) {
        try { $null = & $c --version 2>&1; if ($LASTEXITCODE -eq 0) { $CC = $c; break } } catch {}
    }
}

if ($CC -eq "") {
    Write-Host (Red "ERROR: No C compiler found.")
    Write-Host ""
    Write-Host "Install one of:"
    Write-Host "  $(Yellow '1.') MSYS2  https://www.msys2.org/"
    Write-Host "     Then: pacman -S mingw-w64-x86_64-gcc"
    Write-Host "  $(Yellow '2.') Scoop:  scoop install gcc"
    Write-Host "  $(Yellow '3.') Choco:  choco install mingw"
    Write-Host "  $(Yellow '4.') WinLibs: https://winlibs.com/"
    Write-Host ""
    Write-Host "Or WSL: wsl --install"
    exit 1
}

Write-Host "$(Green '[build]') Compiler: $CC"
Write-Host "$(Green '[build]') Version:  $((& $CC --version 2>&1)[0])"

if ($Target -eq "clean") {
    Remove-Item -Force "oc.exe" -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force "build" -ErrorAction SilentlyContinue
    Write-Host "$(Green '[build]') Cleaned."
    exit 0
}

$srcDirs = @("src\frontend","src\ir","src\backend","src\jit",
             "src\output\obj","src\output\elf","src\output\windows","src\output\iso")
$srcs = @("src\driver.c")
foreach ($dir in $srcDirs) {
    if (Test-Path $dir) {
        $srcs += Get-ChildItem -Path $dir -Filter "*.c" | ForEach-Object { $_.FullName }
    }
}
Write-Host "$(Green '[build]') Sources:  $($srcs.Count) files"

$cflags = @("-std=gnu2x","-Iinclude","-Wall","-Wno-unused-parameter",
            "-Wno-unused-function","-Wno-unused-but-set-variable",
            "-D_WIN32_WINNT=0x0600","-DNDEBUG")
if ($Target -eq "debug") { $cflags += @("-O0","-g3") }
else                     { $cflags += @("-O2","-march=native") }

$null = New-Item -ItemType Directory -Force "build" | Out-Null
$allArgs = $cflags + @("-o","oc.exe") + $srcs + @("-lws2_32")

if ($Verbose) { Write-Host "$(Green '[build]') $CC $($allArgs -join ' ')" }

Write-Host "$(Green '[build]') Compiling..."
$t0 = [System.Diagnostics.Stopwatch]::StartNew()
$proc = Start-Process -FilePath $CC -ArgumentList $allArgs `
    -NoNewWindow -Wait -PassThru -RedirectStandardError "build\_cc_errors.txt"
$t0.Stop()

if ($proc.ExitCode -ne 0) {
    Write-Host (Red "BUILD FAILED")
    Get-Content "build\_cc_errors.txt" | Select-Object -First 30 | ForEach-Object { Write-Host (Red $_) }
    exit 1
}

$sz = (Get-Item "oc.exe").Length / 1KB
Write-Host (Green "[build] Built oc.exe  (${sz:F0} KB, $($t0.Elapsed.TotalSeconds.ToString('F2'))s)")
Write-Host ""
Write-Host "Run it:"
Write-Host "  $(Yellow '.\oc.exe examples\hello.o --jit')"
Write-Host "  $(Yellow '.\oc.exe examples\fib.o --jit')"
Write-Host "  $(Yellow '.\oc.exe --help')"
