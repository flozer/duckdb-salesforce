# build_windows_release.ps1 — GitHub Actions / local MSVC release build.
#
# Builds the static and loadable Salesforce extension into build/release using
# the same vcpkg manifest dependency path expected by DuckDB community CI.

param(
    [string]$DuckDBVersion = $env:DUCKDB_VERSION,
    [string]$BuildDir = "build\release"
)

$ErrorActionPreference = 'Continue'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$buildPath = Join-Path $root $BuildDir

if (-not $DuckDBVersion) {
    $DuckDBVersion = 'v1.5.3'
}

$vcpkgRoot = $env:VCPKG_INSTALLATION_ROOT
if (-not $vcpkgRoot) {
    foreach ($candidate in @('C:\vcpkg', 'C:\Users\fernando.souza\vcpkg')) {
        if (Test-Path (Join-Path $candidate 'scripts\buildsystems\vcpkg.cmake')) {
            $vcpkgRoot = $candidate
            break
        }
    }
}
if (-not $vcpkgRoot) {
    throw 'vcpkg root not found. Set VCPKG_INSTALLATION_ROOT.'
}

$toolchain = Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
if (-not (Test-Path $toolchain)) {
    throw "vcpkg toolchain not found at $toolchain"
}

Set-Location $root

git -C duckdb fetch --depth 1 origin tag $DuckDBVersion
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
git -C duckdb checkout -q $DuckDBVersion
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$rootPosix = $root -replace '\\', '/'
$buildPosix = $buildPath -replace '\\', '/'
$toolchainPosix = $toolchain -replace '\\', '/'

$cfg = @(
    '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_TOOLCHAIN_FILE=$toolchainPosix",
    '-DVCPKG_TARGET_TRIPLET=x64-windows-static',
    "-DVCPKG_MANIFEST_DIR=$rootPosix",
    '-DVCPKG_MANIFEST_MODE=ON',
    '-DEXTENSION_STATIC_BUILD=1',
    "-DDUCKDB_EXTENSION_CONFIGS=$rootPosix/extension_config.cmake",
    "-DUNITTEST_ROOT_DIRECTORY=$rootPosix",
    '-DENABLE_EXTENSION_AUTOLOADING=1',
    '-DENABLE_EXTENSION_AUTOINSTALL=1',
    '-S', "$rootPosix/duckdb",
    '-B', $buildPosix
)

cmake @cfg
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $buildPosix --config Release --target salesforce_extension salesforce_loadable_extension
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

exit 0
