param(
    [string]$Gxx = ""
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $here "plugin.cpp"
$out = Join-Path $here "NFSU2FixedHoodCam.asi"

if (-not $Gxx) {
    $cmd = Get-Command g++ -ErrorAction SilentlyContinue
    if (-not $cmd) {
        throw "g++ not found on PATH. Install a 32-bit MinGW-w64 toolchain or pass -Gxx <path-to-g++.exe>."
    }
    $Gxx = $cmd.Source
}
if (-not (Test-Path $Gxx)) {
    throw "g++ not found at '$Gxx'."
}

# cc1plus/ld need the mingw32 runtime DLLs from the compiler directory.
$env:PATH = (Split-Path -Parent $Gxx) + ";" + $env:PATH
& $Gxx -std=c++17 -O2 -m32 -shared -static-libgcc -static-libstdc++ `
    -mno-sse -mno-sse2 -mfpmath=387 `
    -o $out $src -lkernel32 -luser32
if ($LASTEXITCODE -ne 0) { throw "g++ failed with exit code $LASTEXITCODE" }
Get-Item $out | Select-Object FullName, Length, LastWriteTime
