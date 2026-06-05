# build_rtools_local.ps1 — local Windows MinGW/RTOOLS build + offline tests.
#
# This validates the RTOOLS path separately from the normal MSVC/vcpkg build.
# The important bit is VCPKG_MANIFEST_MODE=ON with x64-mingw-static; otherwise
# CMake may pick RTOOLS OpenSSL 1.1.x, while httplib requires OpenSSL >= 3.
#
# Usage:
#   pwsh -File scripts/build_rtools_local.ps1
#   pwsh -File scripts/build_rtools_local.ps1 -Clean

param(
    [string]$RtoolsRoot = 'C:\rtools42',
    [string]$BuildDir = 'D:\Dados\duckdb-salesforce\build\rtools-release',
    [switch]$Clean
)

$ErrorActionPreference = 'Continue'
$root = 'D:\Dados\duckdb-salesforce'
$rtoolsBin = ($RtoolsRoot -replace '\\', '/') + '/x86_64-w64-mingw32.static.posix/bin'
$rtoolsUsr = ($RtoolsRoot -replace '\\', '/') + '/usr/bin'
$ninja = 'C:/Users/fernando.souza/AppData/Local/Microsoft/WinGet/Packages/Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe/ninja.exe'
$vcpkgToolchain = 'C:/Users/fernando.souza/vcpkg/scripts/buildsystems/vcpkg.cmake'
$buildDirPosix = $BuildDir -replace '\\', '/'
$rootPosix = $root -replace '\\', '/'

if (-not (Test-Path "$RtoolsRoot\x86_64-w64-mingw32.static.posix\bin\gcc.exe")) {
    Write-Error "RTOOLS gcc not found under $RtoolsRoot"
    exit 1
}
if (-not (Test-Path ($ninja -replace '/', '\'))) {
    Write-Error "Ninja not found at $ninja"
    exit 1
}
if (-not (Test-Path ($vcpkgToolchain -replace '/', '\'))) {
    Write-Error "vcpkg toolchain not found at $vcpkgToolchain"
    exit 1
}

if ($Clean) {
    $target = Resolve-Path -LiteralPath $BuildDir -ErrorAction SilentlyContinue
    if ($target -and $target.Path.StartsWith($BuildDir)) {
        Remove-Item -LiteralPath $target.Path -Recurse -Force
    }
}

# Offline guarantee: live tests are skipped and live credentials are not needed.
Get-ChildItem Env:SF_LIVE_* -ErrorAction SilentlyContinue | ForEach-Object {
    Remove-Item "Env:$($_.Name)" -ErrorAction SilentlyContinue
}

$env:Path = "$rtoolsBin;$rtoolsUsr;$(Split-Path $ninja);$env:Path"
Set-Location $root

$cfg = @(
    '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DCMAKE_C_COMPILER=$rtoolsBin/gcc.exe",
    "-DCMAKE_CXX_COMPILER=$rtoolsBin/g++.exe",
    "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain",
    '-DVCPKG_TARGET_TRIPLET=x64-mingw-static',
    '-DVCPKG_HOST_TRIPLET=x64-windows',
    "-DVCPKG_MANIFEST_DIR=$rootPosix",
    '-DVCPKG_MANIFEST_MODE=ON',
    '-DEXTENSION_STATIC_BUILD=1',
    "-DDUCKDB_EXTENSION_CONFIGS=$rootPosix/extension_config.cmake",
    '-DDUCKDB_EXPLICIT_PLATFORM=windows_amd64_mingw',
    "-DUNITTEST_ROOT_DIRECTORY=$rootPosix",
    '-DENABLE_EXTENSION_AUTOLOADING=1',
    '-DENABLE_EXTENSION_AUTOINSTALL=1',
    '-S', "$rootPosix/duckdb",
    '-B', $buildDirPosix
)

Write-Host "=== configure RTOOLS/MinGW ==="
& "$rtoolsBin/cmake.exe" @cfg
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "=== build extension + unittest ==="
& "$rtoolsBin/cmake.exe" --build $buildDirPosix --target salesforce_extension salesforce_loadable_extension unittest --parallel 2
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$unittest = Join-Path $BuildDir 'test\unittest.exe'
if (-not (Test-Path $unittest)) {
    Write-Error "unittest binary not found: $unittest"
    exit 1
}

Write-Host "=== offline salesforce tests ==="
$pass = 0
$fail = 0
$skip = 0
$asserts = 0
$failures = @()

Get-ChildItem "$root\test\sql\salesforce_*.test" |
    Where-Object { $_.Name -notlike '*_live*' } |
    ForEach-Object {
        # MinGW registers SQL tests by absolute path; MSVC registers by relative path.
        $filter = $_.FullName -replace '\\', '/'
        $out = (& $unittest $filter 2>&1 | Out-String)
        if ($out -match 'All tests passed \((\d+) assertions') {
            $pass++
            $asserts += [int]$Matches[1]
        } elseif ($out -match 'Skipped tests') {
            $skip++
        } else {
            $fail++
            $tail = (($out -split "`r?`n") | Select-Object -Last 8) -join ' | '
            $failures += "$($_.Name): $tail"
        }
    }

Write-Host "pass=$pass fail=$fail skip=$skip assertions=$asserts"
if ($fail -ne 0) {
    $failures | Select-Object -First 10 | ForEach-Object { Write-Host $_ }
    exit 1
}

exit 0
