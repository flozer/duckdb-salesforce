# build_matrix.ps1 — build + run the offline test suite against a matrix of
# DuckDB release tags, locally (Windows). Proves the extension builds/tests per
# DuckDB version (it is version-locked to the DuckDB used at build time).
#
# - Does NOT change the committed submodule pin (restored to v1.5.3 at the end).
# - Each version builds into build/matrix/<tag> (separate vcpkg_installed).
# - Offline tests only: SF_LIVE_* are cleared; *_live.test files are skipped.
# - Never touches tags/releases/publication. Nothing leaves flozer/duckdb-salesforce.
#
# Usage:  pwsh -File scripts/build_matrix.ps1            # default v1.5.2, v1.5.3
#         pwsh -c "& ./scripts/build_matrix.ps1 -Tags v1.5.3,v1.6.0"
#         (use -c "& ..." for a custom -Tags list; `-File -Tags a,b` passes one
#          string, not an array.)
#
# Exit code: non-zero if the baseline v1.5.3 fails to build or test.

param([string[]]$Tags = @('v1.5.2', 'v1.5.3'))

$ErrorActionPreference = 'Continue'
$root = 'd:/Dados/duckdb-salesforce'
$pin = '14eca11bd9d4a0de2ea0f078be588a9c1c5b279c' # v1.5.3 (committed submodule pin)
$baseline = 'v1.5.3'

$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = 'C:\Users\fernando.souza\AppData\Local\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe\ninja.exe'
$toolchain = 'C:/Users/fernando.souza/vcpkg/scripts/buildsystems/vcpkg.cmake'

# Make absolutely sure no live credentials are present (CI/offline guarantee).
Get-ChildItem Env:SF_LIVE_* -ErrorAction SilentlyContinue | ForEach-Object {
    Remove-Item "Env:$($_.Name)" -ErrorAction SilentlyContinue
}

function Invoke-Build([string]$buildDir) {
    $cfg = "-G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_TOOLCHAIN_FILE=$toolchain " +
           "-DVCPKG_TARGET_TRIPLET=x64-windows-static -DVCPKG_MANIFEST_DIR=$root " +
           "-DVCPKG_MANIFEST_MODE=ON -DEXTENSION_STATIC_BUILD=1 " +
           "-DDUCKDB_EXTENSION_CONFIGS=$root/extension_config.cmake " +
           "-DUNITTEST_ROOT_DIRECTORY=$root -DENABLE_EXTENSION_AUTOLOADING=1 " +
           "-DENABLE_EXTENSION_AUTOINSTALL=1 -DCMAKE_BUILD_TYPE=Release " +
           "-S $root/duckdb -B $buildDir"
    cmd /c "`"$vcvars`" >nul && `"$cmake`" $cfg && `"$cmake`" --build $buildDir --config Release" 2>&1 |
        Out-Null
    return ($LASTEXITCODE -eq 0)
}

function Invoke-OfflineTests([string]$buildDir) {
    $unittest = Join-Path $buildDir 'test/unittest.exe'
    if (-not (Test-Path $unittest)) { return @{ ok = $false; assertions = 0; note = 'no unittest binary' } }
    $pass = 0; $fail = 0; $asserts = 0; $skip = 0
    # unittest matches the REGISTERED test name (relative, forward-slash, from
    # UNITTEST_ROOT_DIRECTORY) — run from $root and pass "test/sql/<name>".
    Push-Location $root
    try {
        Get-ChildItem "$root/test/sql/salesforce_*.test" | Where-Object { $_.Name -notlike '*_live*' } | ForEach-Object {
            $out = (& $unittest "test/sql/$($_.Name)" 2>&1 | Out-String)
            if ($out -match 'All tests passed \((\d+) assertions') { $pass++; $asserts += [int]$Matches[1] }
            elseif ($out -match 'Skipped tests') { $skip++ }
            else { $fail++; Write-Host "    FAIL $($_.Name)" }
        }
    } finally {
        Pop-Location
    }
    return @{ ok = ($fail -eq 0 -and $pass -gt 0); assertions = $asserts; files_pass = $pass; files_fail = $fail }
}

$results = @()
foreach ($tag in $Tags) {
    Write-Host "=== DuckDB $tag ==="
    git -C "$root/duckdb" rev-parse "$tag^{commit}" *> $null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  fetching tag $tag ..."
        git -C "$root/duckdb" fetch --depth 1 origin tag $tag *> $null
    }
    git -C "$root/duckdb" checkout -q $tag 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        $results += [pscustomobject]@{ Version = $tag; Build = 'n/a'; Tests = 'n/a'; Assertions = 0; Status = 'CHECKOUT-FAIL' }
        continue
    }
    $bdir = "$root/build/matrix/$tag"
    Write-Host "  building -> $bdir (this is slow; OpenSSL + DuckDB)"
    $built = Invoke-Build $bdir
    if (-not $built) {
        $results += [pscustomobject]@{ Version = $tag; Build = 'FAIL'; Tests = '-'; Assertions = 0; Status = 'BUILD-FAIL (file API-drift issue)' }
        continue
    }
    $t = Invoke-OfflineTests $bdir
    $results += [pscustomobject]@{
        Version    = $tag
        Build      = 'ok'
        Tests      = $(if ($t.ok) { 'green' } else { "FAIL ($($t.files_fail))" })
        Assertions = $t.assertions
        Status     = $(if ($t.ok) { 'PASS' } else { 'TESTS-FAIL' })
    }
}

# Restore the committed pin so the worktree is clean.
git -C "$root/duckdb" checkout -q $pin 2>&1 | Out-Null

Write-Host ""
Write-Host "===== DuckDB build matrix ====="
$results | Format-Table -AutoSize

$base = $results | Where-Object { $_.Version -eq $baseline }
if ($base -and $base.Status -eq 'PASS') {
    Write-Host "baseline ${baseline}: PASS"
    exit 0
} else {
    Write-Host "baseline ${baseline}: NOT PASSING — failing the matrix"
    exit 1
}
