# Build and run the pure-C host tests with MSVC cl.exe.
#   powershell -ExecutionPolicy Bypass -File tests\run_host_tests.ps1
# Finds vcvars64.bat from a VS 2022 install; override with -VcVars "<path>".
param(
    [string]$VcVars = ""
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
$out  = Join-Path $here "out"
New-Item -ItemType Directory -Force -Path $out | Out-Null

if (-not $VcVars) {
    $cands = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    )
    $VcVars = $cands | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $VcVars -or -not (Test-Path $VcVars)) { throw "vcvars64.bat not found; pass -VcVars <path>" }

# generate request-frame vectors from the Python oracle
python "$here\gen_frames.py" | Out-File -Encoding ascii "$out\frames.hex"

$inc_blt = Join-Path $root "firmware\components\blt_proto\include"
$src_blt = Join-Path $root "firmware\components\blt_proto\blt_proto.c"
$inc_bms = Join-Path $root "firmware\components\bms\include"
$src_bms = Join-Path $root "firmware\components\bms\bms_jbd.c"

# each test: name, extra include, extra source, args
$tests = @(
    @{ name = "test_blt_proto"; inc = $inc_blt; src = $src_blt; args = "`"$out\frames.hex`"" }
)
if ((Test-Path $src_bms) -and (Test-Path "$here\host\test_bms_jbd.c")) {
    $tests += @{ name = "test_bms_jbd"; inc = $inc_bms; src = $src_bms; args = "" }
}

$fail = 0
foreach ($t in $tests) {
    $exe = Join-Path $out ($t.name + ".exe")
    $csrc = Join-Path $here ("host\" + $t.name + ".c")
    # compile with cwd=$out so .obj/.pdb land there (avoids /Fo trailing-backslash quoting)
    $cl = "cl /nologo /W4 /O2 /TC /I `"$($t.inc)`" `"$csrc`" `"$($t.src)`" /Fe:`"$($t.name).exe`""
    cmd /c "`"$VcVars`" >nul 2>&1 && cd /d `"$out`" && $cl" | Write-Host
    if ($LASTEXITCODE -ne 0) { Write-Host "COMPILE FAILED: $($t.name)"; $fail++; continue }
    Write-Host "--- $($t.name) ---"
    cmd /c "`"$exe`" $($t.args)"
    if ($LASTEXITCODE -ne 0) { $fail++ }
}
if ($fail) { Write-Host "`nHOST TESTS FAILED ($fail)"; exit 1 } else { Write-Host "`nALL HOST TESTS PASSED"; exit 0 }
