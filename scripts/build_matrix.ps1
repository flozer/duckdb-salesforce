# build_matrix.ps1 — build + run the offline test suite against a matrix of
# DuckDB refs (tag, branch, or SHA), locally (Windows). Proves the extension
# builds/tests per DuckDB version (it is version-locked to the DuckDB used at
# build time).
#
# - Does NOT change the committed submodule pin: the submodule's original HEAD
#   is discovered before any mutation and restored in a `finally` block, even
#   on error.
# - -Tags are GATING refs (must include -Baseline). -CanaryRefs are additional
#   refs built/tested in the SAME run but never gate the exit code — a ref is
#   canary because it's listed in -CanaryRefs, not because of a global switch,
#   so a run can mix an exact gating baseline with a non-gating canary safely.
# - Each ref+SHA+configuration builds into its own isolated, short directory
#   under build/matrix/ — no shared CMake cache between refs or configs. Pass
#   -Clean to wipe that exact directory first, guaranteeing an independent
#   from-scratch build (no unintentional reuse of a previous attempt's cache).
# - Every ref+config gets its own log directory (build/matrix/<dir>/_logs/):
#   configure.log, build.log, load_smoke.log, tests_summary.log, plus one file
#   per failed/skipped test. Logs are local build artifacts (build/ is
#   gitignored) — never commit them.
# - Offline tests only: SF_LIVE_* are cleared before every run; *_live.test
#   files are always excluded from the glob (belt and suspenders).
# - Never touches tags/releases/publication, never runs remote CI.
# - No manual cmd-string quoting for cmake/ninja: vcvars is sourced once into
#   this process's own environment, then cmake/ninja/the CLI are invoked
#   directly via PowerShell's `&` call operator with array arguments, so a
#   path containing spaces (-Root, VCPKG_ROOT, etc.) is passed as one argv
#   token natively — no cmd.exe re-parsing, no quoting bugs.
#
# Tool discovery (no hardcoded personal paths): cmake/ninja are resolved via
# PATH (Get-Command) or the DUCKDB_SF_CMAKE / DUCKDB_SF_NINJA env vars.
# vcvars64.bat is resolved via vswhere.exe (standard VS Installer location) or
# DUCKDB_SF_VCVARS. The vcpkg toolchain file is resolved via VCPKG_ROOT or
# DUCKDB_SF_VCPKG_TOOLCHAIN.
#
# Usage:
#   pwsh -File scripts/build_matrix.ps1
#   pwsh -File scripts/build_matrix.ps1 -Tags v1.5.4,v1.5.5 -Baseline v1.5.4
#   pwsh -File scripts/build_matrix.ps1 -Tags v1.5.5 -Configuration Both
#   pwsh -File scripts/build_matrix.ps1 -Tags v1.5.5 -Baseline v1.5.5 -CanaryRefs <mainSHA> -Clean
#     (v1.5.5 is built/tested as the gating baseline; <mainSHA> is built/
#      tested in the SAME run as a non-gating canary; -Clean wipes only the
#      canary's own exact build directory first, so it's an independent
#      from-scratch attempt, not a reuse of a stale directory.)
#
# Exit code: non-zero if the build, LOAD smoke, or any offline test fails for
# any *non-canary* ref, or if -Baseline's ref never actually produced a
# non-canary, non-failing result (a missing baseline is a hard failure, never
# a silent "passing").

[CmdletBinding()]
param(
    [string[]]$Tags = @('v1.5.4', 'v1.5.5'),
    [string]$Baseline = 'v1.5.4',
    [string[]]$CanaryRefs = @(),
    [ValidateSet('Release', 'Debug', 'Both')]
    [string]$Configuration = 'Release',
    [switch]$Clean,
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')),
    [string]$VcVars = $env:DUCKDB_SF_VCVARS,
    [string]$CMakePath = $env:DUCKDB_SF_CMAKE,
    [string]$NinjaPath = $env:DUCKDB_SF_NINJA,
    [string]$VcpkgToolchain = $env:DUCKDB_SF_VCPKG_TOOLCHAIN
)

$ErrorActionPreference = 'Stop'
# Forward-slash normalized deliberately: $Root flows into -DUNITTEST_ROOT_
# DIRECTORY, which DuckDB's build embeds as a compiled-in string constant in
# unittest.exe. A raw Windows backslash path there (e.g. "D:\Dados\...")
# corrupted that embedded constant badly enough to crash unittest.exe at
# runtime (0xC0000409) on every single test file, even from a from-scratch
# clean build -- reproduced twice. Forward slashes are accepted by every
# Windows API/CMake path argument used in this script, so this is a safe
# normalization, not just a cosmetic one.
$Root = (Resolve-Path $Root).Path -replace '\\', '/'

# Some parent shells (e.g. Git Bash / MSYS) don't pass through env var names
# containing parentheses, so a pwsh child process can be missing
# "ProgramFiles(x86)" even though it's a standard, non-personal Windows path.
# vcvars64.bat probes for it internally; set it defensively if absent.
if (-not ${env:ProgramFiles(x86)}) {
    ${env:ProgramFiles(x86)} = 'C:\Program Files (x86)'
}

function Resolve-VcVars([string]$EnvValue) {
    if ($EnvValue -and (Test-Path $EnvValue)) { return $EnvValue }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -property installationPath 2>$null
        if ($vsPath) {
            $candidate = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path $candidate) { return $candidate }
        }
    }
    throw "Cannot locate vcvars64.bat. Set `$env:DUCKDB_SF_VCVARS to its full path."
}

function Resolve-Tool([string]$EnvValue, [string]$CommandName, [string]$FriendlyName) {
    if ($EnvValue -and (Test-Path $EnvValue)) { return $EnvValue }
    $found = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }
    throw "Cannot locate $FriendlyName. Set `$env:DUCKDB_SF_$($FriendlyName.ToUpper()) or put '$CommandName' on PATH."
}

function Resolve-VcpkgToolchain([string]$EnvValue) {
    if ($EnvValue -and (Test-Path $EnvValue)) { return $EnvValue }
    if ($env:VCPKG_ROOT) {
        $candidate = Join-Path $env:VCPKG_ROOT 'scripts/buildsystems/vcpkg.cmake'
        if (Test-Path $candidate) { return $candidate }
    }
    throw "Cannot locate vcpkg toolchain file. Set `$env:VCPKG_ROOT or `$env:DUCKDB_SF_VCPKG_TOOLCHAIN."
}

function Get-EnvSnapshot {
    $snap = @{}
    Get-ChildItem Env: | ForEach-Object { $snap[$_.Name] = $_.Value }
    return $snap
}

function Restore-EnvSnapshot([hashtable]$Snapshot) {
    Get-ChildItem Env: | ForEach-Object {
        if (-not $Snapshot.ContainsKey($_.Name)) {
            Remove-Item "Env:$($_.Name)" -ErrorAction SilentlyContinue
        }
    }
    foreach ($k in $Snapshot.Keys) {
        [System.Environment]::SetEnvironmentVariable($k, $Snapshot[$k], 'Process')
    }
}

function Invoke-WithVcVars {
    param([string]$VcVarsPath, [scriptblock]$Action)
    # The only remaining cmd.exe hop: vcvars64.bat is a .bat file with no
    # PowerShell equivalent. Its own output is discarded (>nul 2>&1); only
    # the resulting environment (dumped via `set`) is captured and imported,
    # so the actual tool call inside $Action uses PowerShell's native `&`
    # array-argument invocation instead of another cmd-string re-parse.
    #
    # Scoped to just $Action, not the whole script: importing vcvars into
    # the process and leaving it there for the rest of the run corrupts
    # later, unrelated native-exe launches -- unittest.exe crashed with
    # 0xC0000409 (stack-buffer-overrun) when run under a lingering VS dev
    # environment during this fix's own verification. The original
    # environment (whatever it was for THIS call) is restored in `finally`
    # regardless of $Action's outcome, so a build failure can't leave a
    # later LOAD-smoke/test step running under a half-VS environment either.
    $snapshot = Get-EnvSnapshot
    try {
        $envDump = cmd /c "`"$VcVarsPath`" >nul 2>&1 && set"
        foreach ($line in $envDump) {
            if ($line -match '^([^=]+)=(.*)$') {
                [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
            }
        }
        & $Action
    } finally {
        Restore-EnvSnapshot $snapshot
    }
}

$vcvars = Resolve-VcVars $VcVars
$cmake = Resolve-Tool $CMakePath 'cmake.exe' 'CMAKE'
$ninja = Resolve-Tool $NinjaPath 'ninja.exe' 'NINJA'
$toolchain = (Resolve-VcpkgToolchain $VcpkgToolchain) -replace '\\', '/'

function Split-RefList([string[]]$List) {
    return @($List | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}

$Tags = Split-RefList $Tags
$CanaryRefs = Split-RefList $CanaryRefs

if ($Tags.Count -eq 0) { throw "-Tags must list at least one gating ref (the baseline)." }
if ($Tags -notcontains $Baseline) {
    throw "-Baseline '$Baseline' must be one of -Tags. A ref only in -CanaryRefs can never satisfy the baseline -- that was exactly the bug this check exists to prevent (a canary-only run previously reported 'baseline passing' despite the baseline never running as a non-canary ref)."
}

$configs = if ($Configuration -eq 'Both') { @('Release', 'Debug') } else { @($Configuration) }

# Make absolutely sure no live credentials are present (CI/offline guarantee).
Get-ChildItem Env: | Where-Object { $_.Name -like 'SF_LIVE_*' } | ForEach-Object {
    Remove-Item "Env:$($_.Name)" -ErrorAction SilentlyContinue
}

function Get-CompilerInfo {
    $clOutput = Invoke-WithVcVars $vcvars { & cl 2>&1 | Out-String }
    # cl with no args prints its copyright banner to stderr before the usage
    # line; take the first line that actually names the compiler, not
    # whichever stream happened to interleave first.
    $versionLine = ($clOutput -split "`r?`n" | Where-Object { $_ -match 'Compiler|Optimizing' } | Select-Object -First 1)
    if (-not $versionLine) { $versionLine = ($clOutput -split "`r?`n" | Select-Object -First 1) }
    $arch = if ($clOutput -match 'x64') { 'x64' } else { 'unknown' }
    return @{ Compiler = $versionLine.Trim(); Arch = $arch }
}

function Sanitize-RefForPath([string]$ref) {
    $clean = ($ref -replace '[\^~:\\/\s]', '-')
    # A raw 40-char SHA passed as -Tags/-CanaryRefs makes the build directory
    # long enough to push generated unity-build #include paths (which walk
    # back up via several ../ segments) needlessly deep. Keep ref segments
    # short, same as the SHA segment already is.
    if ($clean.Length -gt 12) { return $clean.Substring(0, 12) }
    return $clean
}

function Resolve-RefSha([string]$Ref) {
    Push-Location "$Root/duckdb"
    try {
        git rev-parse "$Ref^{commit}" *> $null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  fetching $Ref ..."
            git fetch --depth 1 origin "$Ref" *> $null
            if ($LASTEXITCODE -ne 0) {
                git fetch --depth 1 origin "tag $Ref" *> $null
            }
        }
        $sha = (git rev-parse "$Ref^{commit}" 2>$null | Out-String).Trim()
        if (-not $sha) { throw "Cannot resolve ref '$Ref' to a commit (not a tag, branch, or SHA reachable in the duckdb submodule)." }
        return $sha
    } finally {
        Pop-Location
    }
}

function Invoke-MatrixBuild([string]$Ref, [string]$Sha, [string]$Config, [bool]$CleanFirst) {
    $dirName = "$(Sanitize-RefForPath $Ref)-$($Sha.Substring(0,8))-$($Config.ToLower())"
    $buildDir = "$Root/build/matrix/$dirName"

    if ($CleanFirst -and (Test-Path $buildDir)) {
        Write-Host "  -Clean: removing $buildDir"
        Remove-Item -Recurse -Force $buildDir
    }

    $logsDir = Join-Path $buildDir '_logs'
    New-Item -ItemType Directory -Force -Path $logsDir | Out-Null
    $configureLog = Join-Path $logsDir 'configure.log'
    $buildLog = Join-Path $logsDir 'build.log'

    # Every element below is ONE array item, so PowerShell's `&` operator
    # passes it as a single argv token to cmake.exe regardless of embedded
    # spaces (e.g. $Root or $toolchain under "C:\Some Path\..."). No manual
    # string quoting, no cmd.exe re-parse.
    $cfgArgs = @(
        '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$ninja",
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
        '-DVCPKG_TARGET_TRIPLET=x64-windows-static',
        "-DVCPKG_MANIFEST_DIR=$Root",
        '-DVCPKG_MANIFEST_MODE=ON',
        '-DEXTENSION_STATIC_BUILD=1',
        "-DDUCKDB_EXTENSION_CONFIGS=$Root/extension_config.cmake",
        "-DUNITTEST_ROOT_DIRECTORY=$Root",
        '-DENABLE_EXTENSION_AUTOLOADING=1',
        '-DENABLE_EXTENSION_AUTOINSTALL=1',
        "-DCMAKE_BUILD_TYPE=$Config",
        '-S', "$Root/duckdb",
        '-B', $buildDir
    )

    # The cmake/ninja/cl invocations need the VS dev environment; everything
    # else (LOAD smoke, offline tests, called by other functions after this
    # one returns) must NOT run under it -- see Invoke-WithVcVars. Both
    # configure and build happen inside ONE Invoke-WithVcVars call so vcvars
    # is imported/restored once per ref+config, not twice.
    return Invoke-WithVcVars $vcvars {
        "### $(Get-Date -Format o)  ref=$Ref sha=$Sha config=$Config" | Out-File $configureLog
        "### command: cmake $($cfgArgs -join ' ')" | Out-File $configureLog -Append
        & $cmake @cfgArgs *>> $configureLog
        $configureExit = $LASTEXITCODE
        "### exit_code=$configureExit" | Out-File $configureLog -Append

        if ($configureExit -ne 0) {
            return @{ ok = $false; dir = $buildDir; logsDir = $logsDir; stage = 'configure'; exitCode = $configureExit }
        }

        "### $(Get-Date -Format o)  ref=$Ref sha=$Sha config=$Config" | Out-File $buildLog
        "### command: cmake --build $buildDir --config $Config" | Out-File $buildLog -Append
        & $cmake --build $buildDir --config $Config *>> $buildLog
        $buildExit = $LASTEXITCODE
        "### exit_code=$buildExit" | Out-File $buildLog -Append

        return @{ ok = ($buildExit -eq 0); dir = $buildDir; logsDir = $logsDir; stage = 'build'; exitCode = $buildExit }
    }
}

function Find-DuckDbCli([string]$buildDir) {
    $candidates = @(
        (Join-Path $buildDir 'duckdb.exe'),
        (Join-Path $buildDir 'Release/duckdb.exe'),
        (Join-Path $buildDir 'Debug/duckdb.exe')
    )
    return ($candidates | Where-Object { Test-Path $_ } | Select-Object -First 1)
}

function Invoke-LoadSmoke([string]$buildDir, [string]$logsDir) {
    # Explicit LOAD check via the built CLI when present. If no CLI binary was
    # produced by this build config, the offline test suite (every .test file
    # starts with `LOAD salesforce;`) is the load gate instead — a missing
    # unittest binary already fails that step, so this returns true here to
    # avoid a redundant false negative; the real gate still fires downstream.
    $logPath = Join-Path $logsDir 'load_smoke.log'
    $cli = Find-DuckDbCli $buildDir
    if (-not $cli) {
        "no duckdb.exe found under $buildDir -- offline test suite (LOAD salesforce; per file) is the load gate instead" |
            Out-File $logPath
        return $true
    }
    "### command: `"$cli`" -unsigned -c `"LOAD salesforce; SELECT 1;`"" | Out-File $logPath
    $out = (& $cli -unsigned -c "LOAD salesforce; SELECT 1;" 2>&1 | Out-String)
    $out | Out-File $logPath -Append
    $ok = ($LASTEXITCODE -eq 0 -and $out -notmatch 'Error')
    "### exit_code=$LASTEXITCODE ok=$ok" | Out-File $logPath -Append
    return $ok
}

function Invoke-OfflineTests([string]$buildDir, [string]$logsDir) {
    $unittest = Join-Path $buildDir 'test/unittest.exe'
    $summaryLog = Join-Path $logsDir 'tests_summary.log'
    if (-not (Test-Path $unittest)) {
        "unittest.exe not found at $unittest" | Out-File $summaryLog
        return @{ ok = $false; assertions = 0; passed = 0; failed = 0; skipped = 0 }
    }
    $passed = 0; $failed = 0; $skipped = 0; $asserts = 0
    $summaryLines = @()
    Push-Location $Root
    try {
        Get-ChildItem "$Root/test/sql/salesforce_*.test" |
            Where-Object { $_.Name -notlike '*_live.test' } |
            Sort-Object Name |
            ForEach-Object {
                $testName = $_.Name
                $out = (& $unittest "test/sql/$testName" 2>&1 | Out-String)
                if ($out -match 'All tests passed \((\d+) assertions?') {
                    $passed++; $asserts += [int]$Matches[1]
                    $summaryLines += "PASS  $testName  ($($Matches[1]) assertions)"
                }
                elseif ($out -match 'Skipped tests') {
                    $skipped++
                    $summaryLines += "SKIP  $testName"
                    $out | Out-File (Join-Path $logsDir "skipped_$testName.log")
                }
                else {
                    $failed++
                    $summaryLines += "FAIL  $testName"
                    $out | Out-File (Join-Path $logsDir "failed_$testName.log")
                }
            }
    } finally {
        Pop-Location
    }
    $summaryLines += ""
    $summaryLines += "passed=$passed failed=$failed skipped=$skipped assertions=$asserts"
    $summaryLines | Out-File $summaryLog
    return @{ ok = ($failed -eq 0); assertions = $asserts; passed = $passed; failed = $failed; skipped = $skipped }
}

Push-Location "$Root/duckdb"
try {
    $originalHead = (git rev-parse HEAD | Out-String).Trim()
} finally {
    Pop-Location
}
Write-Host "Submodule original HEAD: $originalHead"

# Build the ref plan explicitly: each entry carries its own Canary flag,
# determined by which parameter it came from -- never by a single switch that
# would (as it previously did) mark an entire run's Tags as canary and leave
# the baseline unfindable among non-canary results.
$refPlan = @()
foreach ($t in $Tags) { $refPlan += [pscustomobject]@{ Ref = $t; Canary = $false } }
foreach ($c in $CanaryRefs) { $refPlan += [pscustomobject]@{ Ref = $c; Canary = $true } }

$results = @()
$hardFailure = $false
$compilerInfo = Get-CompilerInfo

try {
    foreach ($item in $refPlan) {
        $ref = $item.Ref
        $isCanary = $item.Canary
        Write-Host "=== DuckDB $ref $(if ($isCanary) { '(canary, non-gating)' } else { '(gating)' }) ==="
        $sha = Resolve-RefSha $ref

        Push-Location "$Root/duckdb"
        try {
            git checkout -q $sha 2>&1 | Out-Null
            $checkoutOk = ($LASTEXITCODE -eq 0)
        } finally {
            Pop-Location
        }
        if (-not $checkoutOk) {
            $results += [pscustomobject]@{ Ref = $ref; Sha = $sha; Compiler = $compilerInfo.Compiler; Arch = $compilerInfo.Arch; Configuration = '-'; BuildDir = '-'; Build = 'n/a'; Passed = 0; Failed = 0; Skipped = 0; Assertions = 0; Canary = $isCanary; Status = 'CHECKOUT-FAIL' }
            if (-not $isCanary) { $hardFailure = $true }
            continue
        }

        foreach ($config in $configs) {
            Write-Host "  building $config -> (this is slow; OpenSSL + DuckDB)"
            $build = Invoke-MatrixBuild -Ref $ref -Sha $sha -Config $config -CleanFirst:$Clean
            if (-not $build.ok) {
                $results += [pscustomobject]@{ Ref = $ref; Sha = $sha; Compiler = $compilerInfo.Compiler; Arch = $compilerInfo.Arch; Configuration = $config; BuildDir = $build.dir; Build = "FAIL ($($build.stage))"; Passed = 0; Failed = 0; Skipped = 0; Assertions = 0; Canary = $isCanary; Status = 'BUILD-FAIL' }
                if (-not $isCanary) { $hardFailure = $true }
                continue
            }
            $loadOk = Invoke-LoadSmoke $build.dir $build.logsDir
            if (-not $loadOk) {
                $results += [pscustomobject]@{ Ref = $ref; Sha = $sha; Compiler = $compilerInfo.Compiler; Arch = $compilerInfo.Arch; Configuration = $config; BuildDir = $build.dir; Build = 'ok'; Passed = 0; Failed = 0; Skipped = 0; Assertions = 0; Canary = $isCanary; Status = 'LOAD-FAIL' }
                if (-not $isCanary) { $hardFailure = $true }
                continue
            }
            $t = Invoke-OfflineTests $build.dir $build.logsDir
            $status = if ($t.failed -gt 0) { 'TESTS-FAIL' } elseif ($t.skipped -gt 0) { 'PASS-WITH-SKIPS' } else { 'PASS' }
            $results += [pscustomobject]@{
                Ref           = $ref
                Sha           = $sha
                Compiler      = $compilerInfo.Compiler
                Arch          = $compilerInfo.Arch
                Configuration = $config
                BuildDir      = $build.dir
                Build         = 'ok'
                Passed        = $t.passed
                Failed        = $t.failed
                Skipped       = $t.skipped
                Assertions    = $t.assertions
                Canary        = $isCanary
                Status        = $status
            }
            if ($status -eq 'TESTS-FAIL' -and -not $isCanary) { $hardFailure = $true }
        }
    }
}
finally {
    Push-Location "$Root/duckdb"
    try {
        git checkout -q $originalHead 2>&1 | Out-Null
        $restoredHead = (git rev-parse HEAD | Out-String).Trim()
        if ($restoredHead -ne $originalHead) {
            Write-Error "Submodule restore FAILED: expected $originalHead, got $restoredHead"
            exit 1
        }
        Write-Host "Submodule restored to original HEAD: $restoredHead"
    } finally {
        Pop-Location
    }
}

Write-Host ""
Write-Host "===== DuckDB build matrix ====="
# Format-Table -AutoSize truncates/drops trailing columns when the console
# width is narrow or absent (background/redirected run) instead of wrapping —
# unacceptable for a "reproducible summary" requirement, so print one
# explicit, width-independent block per row instead. Passed/failed/skipped
# are reported as separate numbers -- never folded into a single "green"
# count that would hide an unexpectedly skipped file behind a false "all
# green" impression.
foreach ($r in $results) {
    Write-Host "Ref=$($r.Ref) SHA=$($r.Sha) Configuration=$($r.Configuration) Canary=$($r.Canary)"
    Write-Host "  BuildDir=$($r.BuildDir)"
    Write-Host "  Compiler=$($r.Compiler) Arch=$($r.Arch)"
    Write-Host "  Build=$($r.Build) Passed=$($r.Passed) Failed=$($r.Failed) Skipped=$($r.Skipped) Assertions=$($r.Assertions) Status=$($r.Status)"
}
Write-Host ""
Write-Host "----- CSV (authoritative, for the DEV->PM report) -----"
$results | ConvertTo-Csv -NoTypeInformation | Write-Host

# Baseline gating: -Baseline was already required to be one of -Tags (never
# -CanaryRefs) at parameter-validation time above, so "no non-canary row for
# Baseline" cannot happen from normal use -- this check exists as a second,
# explicit gate anyway, because an empty match must NEVER be treated as
# "baseline passing" (that silent-false-success was the bug this fixes).
$baselineRows = $results | Where-Object { $_.Ref -eq $Baseline -and -not $_.Canary }
if (-not $baselineRows) {
    Write-Host "baseline ${Baseline}: NOT FOUND among non-canary results — failing the matrix"
    exit 1
}
$baselineFailing = $baselineRows | Where-Object { $_.Status -in @('CHECKOUT-FAIL', 'BUILD-FAIL', 'LOAD-FAIL', 'TESTS-FAIL') }
if ($baselineFailing) {
    Write-Host "baseline ${Baseline}: NOT PASSING — failing the matrix"
    exit 1
}
if ($hardFailure) {
    Write-Host "one or more non-canary refs failed build/load/test — failing the matrix"
    exit 1
}
Write-Host "matrix OK (baseline ${Baseline} passing; canary rows do not gate)"
exit 0
