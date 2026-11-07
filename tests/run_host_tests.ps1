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
# component tests link against every .c in that component (shared helpers)
function SrcsOf($comp) {
    (Get-ChildItem (Join-Path $root "firmware\components\$comp") -Filter *.c |
        ForEach-Object { "`"$($_.FullName)`"" }) -join " "
}
$src_bms    = SrcsOf "bms"
$inc_report = Join-Path $root "firmware\components\report\include"
$src_report = SrcsOf "report"
$inc_ipp    = Join-Path $root "firmware\components\print_ipp\include"
$src_ipp    = SrcsOf "print_ipp"
$inc_health = Join-Path $root "firmware\components\health\include"
$src_health = SrcsOf "health"

# discover tests/host/test_*.c and assign includes/sources/args by name
$tests = @()
foreach ($c in Get-ChildItem "$here\host" -Filter "test_*.c" | Sort-Object Name) {
    $name = $c.BaseName
    if ($name -eq "test_blt_proto") {
        $tests += @{ name = $name; inc = $inc_blt; src = "`"$src_blt`""; args = "`"$out\frames.hex`"" }
    } elseif ($name -eq "test_blt_live" -or $name -eq "test_blt_blocks") {
        $tests += @{ name = $name; inc = $inc_blt; src = (SrcsOf "blt_proto"); args = "" }
    } elseif ($name -like "test_bms*") {
        $tests += @{ name = $name; inc = $inc_bms; src = $src_bms; args = "" }
    } elseif ($name -eq "test_report") {
        $tests += @{ name = $name; inc = $inc_report; src = $src_report; args = "`"$out\report.pgm`"" }
    } elseif ($name -eq "test_urf") {
        $tests += @{ name = $name; inc = $inc_report; src = $src_report; args = "" }
    } elseif ($name -eq "test_health") {
        $tests += @{ name = $name; inc = $inc_health; src = $src_health; args = "" }
    } elseif ($name -eq "test_ipp") {
        $tests += @{ name = $name; inc = $inc_ipp; src = $src_ipp; args = "" }
    }
}

$fail = 0
foreach ($t in $tests) {
    $exe = Join-Path $out ($t.name + ".exe")
    $csrc = Join-Path $here ("host\" + $t.name + ".c")
    # compile with cwd=$out so .obj/.pdb land there (avoids /Fo trailing-backslash quoting)
    $cl = "cl /nologo /W4 /O2 /TC /I `"$($t.inc)`" `"$csrc`" $($t.src) /Fe:`"$($t.name).exe`""
    cmd /c "`"$VcVars`" >nul 2>&1 && cd /d `"$out`" && $cl" | Write-Host
    if ($LASTEXITCODE -ne 0) { Write-Host "COMPILE FAILED: $($t.name)"; $fail++; continue }
    Write-Host "--- $($t.name) ---"
    cmd /c "`"$exe`" $($t.args)"
    if ($LASTEXITCODE -ne 0) { $fail++ }
}
if ($fail) { Write-Host "`nHOST TESTS FAILED ($fail)"; exit 1 } else { Write-Host "`nALL HOST TESTS PASSED"; exit 0 }
