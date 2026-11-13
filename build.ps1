# Configures and builds the Release version with LLVM-MinGW (i686) + Ninja, then runs the tests.
#
#   powershell -ExecutionPolicy Bypass -File build.ps1            # build + tests
#   powershell -ExecutionPolicy Bypass -File build.ps1 -NoTests   # build only
#
# Tools (installed with winget):
#   winget install Kitware.CMake
#   winget install Ninja-build.Ninja
#   winget install MartinStorsjo.LLVM-MinGW.UCRT
param(
    [switch]$NoTests,
    [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

function Find-Tool([string]$name, [string[]]$candidates) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($pattern in $candidates) {
        $hit = Get-ChildItem $pattern -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    throw "$name not found. Install it with winget (see the header of build.ps1)."
}

$packages = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages"
$cmake = Find-Tool "cmake.exe" @("$env:ProgramFiles\CMake\bin\cmake.exe")
$ctest = Join-Path (Split-Path -Parent $cmake) "ctest.exe"
$ninja = Find-Tool "ninja.exe" @("$packages\Ninja-build.Ninja*\ninja.exe")
$clang = Find-Tool "i686-w64-mingw32-clang++.exe" @("$packages\MartinStorsjo.LLVM-MinGW.*\*\bin\i686-w64-mingw32-clang++.exe")
$llvmRoot = Split-Path -Parent (Split-Path -Parent $clang)

Write-Host "cmake : $cmake"
Write-Host "ninja : $ninja"
Write-Host "clang : $clang"

$build = Join-Path $root $BuildDir
& $cmake -S $root -B $build -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_BUILD_TYPE=Release" `
    "-DCMAKE_TOOLCHAIN_FILE=$root\cmake\toolchain-llvm-mingw-i686.cmake" `
    "-DLLVM_MINGW_ROOT=$llvmRoot"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

& $cmake --build $build
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

if (-not $NoTests) {
    & $ctest --test-dir $build --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Tests failed" }
}

Write-Host ""
Write-Host "Built: $build\MP3SkipCutscenes.asi"
