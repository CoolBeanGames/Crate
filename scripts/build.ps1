# Configure and build Crate with the Visual Studio 18 toolchain (MSVC + Ninja).
# Usage:  powershell -ExecutionPolicy Bypass -File scripts/build.ps1 [Debug|Release]

param([string]$Config = "Debug")
$ErrorActionPreference = "Stop"

$vs = & "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
if (-not $vs) { throw "Visual Studio installation not found." }

$cmakeDir = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake"
$cmake    = Join-Path $cmakeDir "CMake\bin\cmake.exe"
$ninja    = Join-Path $cmakeDir "Ninja\ninja.exe"
$vcvars   = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

$root  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"

$cmd = "`"$vcvars`" && `"$cmake`" -G Ninja -S `"$root`" -B `"$build`" " +
       "-DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=$Config && " +
       "`"$cmake`" --build `"$build`""

& cmd.exe /c $cmd
exit $LASTEXITCODE
