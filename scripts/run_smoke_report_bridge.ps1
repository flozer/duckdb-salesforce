# LIVE smoke runner for the Report Bridge (ROADMAP §16, shipped in v0.10.0).
#
# Maintainer-gated manual smoke. Drives the three new functions against a REAL
# org using the locally-built Release shell (build/release/duckdb.exe) — NOT the
# community extension (community is still v0.9.2; this never INSTALLs FROM
# community). Credentials are read from the environment ONLY; no secret is ever
# printed, logged, or written to disk. Output below the header is ORG data —
# review before sharing.
#
# Env vars (required): SF_CLIENT_ID, SF_CLIENT_SECRET, SF_REFRESH_TOKEN
# Env vars (optional): SF_LOGIN_URL, SF_API_VERSION, SF_REPORT_ID,
#                      DUCKDB_SHELL_PATH, SALESFORCE_EXTENSION_PATH
#
# Behavior:
#   - SF_REPORT_ID set        -> test that report id directly.
#   - SF_REPORT_ID unset      -> pick the first TABULAR report from
#                                salesforce_reports('sf').
#   - salesforce_report()      uses LIMIT 5 (sample/oracle, not extraction).
#   - salesforce_report_soql() returns one row; the candidate SOQL is NEVER run
#                              at scale automatically.
#   - SALESFORCE_EXTENSION_PATH set -> LOAD that local extension (-unsigned);
#                              otherwise the statically-linked shell is used.
#
# Exit codes:
#   0  success, OR BLOCKED: no accessible TABULAR report (org-data block)
#   2  setup error: duckdb shell not found
#   3  BLOCKED: missing credentials
#   4  FAIL: a Report Bridge function errored (extension failure)
#
# Usage:
#   $env:SF_CLIENT_ID='...'; $env:SF_CLIENT_SECRET='...'; $env:SF_REFRESH_TOKEN='...'
#   $env:SF_LOGIN_URL='https://login.salesforce.com'
#   pwsh -File scripts/run_smoke_report_bridge.ps1
#   # force a specific report:  $env:SF_REPORT_ID='00O...'

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# --- resolve shell + (optional) local extension ------------------------------
$duck = if ($env:DUCKDB_SHELL_PATH) { $env:DUCKDB_SHELL_PATH } else { Join-Path $root 'build/release/duckdb.exe' }
if (-not (Test-Path $duck)) {
    Write-Host "FAIL[setup]: duckdb shell not found: $duck" -ForegroundColor Red
    Write-Host "Build first: cmake --build build/release   (or set DUCKDB_SHELL_PATH)"
    exit 2
}
$extPath  = $env:SALESFORCE_EXTENSION_PATH
$duckArgs = @()
$loadStmt = ''
if ($extPath) {
    if (-not (Test-Path $extPath)) {
        Write-Host "FAIL[setup]: SALESFORCE_EXTENSION_PATH not found: $extPath" -ForegroundColor Red
        exit 2
    }
    $duckArgs = @('-unsigned')
    $loadStmt = "LOAD '$extPath';`n"
    $extDesc  = "$extPath (LOAD, -unsigned)"
} else {
    $extDesc  = 'statically linked in shell (no community INSTALL)'
}

# --- credential preflight (names only, never values) -------------------------
$missing = @()
foreach ($v in 'SF_CLIENT_ID', 'SF_CLIENT_SECRET', 'SF_REFRESH_TOKEN') {
    if (-not (Get-Item "Env:$v" -ErrorAction SilentlyContinue)) { $missing += $v }
}
if ($missing.Count -gt 0) {
    Write-Host "BLOCKED: missing credentials" -ForegroundColor Yellow
    Write-Host ("missing env vars: " + ($missing -join ', '))
    Write-Host "Set them (env auth) and re-run. Values are never read into the log."
    exit 3
}

$loginUrl = if ($env:SF_LOGIN_URL) { $env:SF_LOGIN_URL } else { 'https://login.salesforce.com' }
$apiOpt   = if ($env:SF_API_VERSION) { ", api_version '$($env:SF_API_VERSION)'" } else { '' }
$attach   = "ATTACH 'salesforce://prod' AS sf (TYPE salesforce, auth_source 'env'$apiOpt);"

# --- evidence header ----------------------------------------------------------
$commit = (& git -C $root rev-parse --short HEAD).Trim()
$tag    = ((& git -C $root tag --points-at HEAD) -join ',')
Write-Host "=== Report Bridge live smoke (v0.10.0) ===" -ForegroundColor Cyan
Write-Host ("timestamp     : " + (Get-Date -Format o))
Write-Host ("git commit    : $commit")
Write-Host ("git tag@HEAD  : $tag")
Write-Host ("duckdb shell  : $duck")
Write-Host ("extension     : $extDesc")
Write-Host ("login_url     : $loginUrl")
if ($env:SF_API_VERSION) { Write-Host ("api_version   : $($env:SF_API_VERSION)") }
Write-Host ("report source : " + $(if ($env:SF_REPORT_ID) { "forced SF_REPORT_ID=$($env:SF_REPORT_ID)" } else { "first TABULAR from salesforce_reports('sf')" }))
Write-Host ""

# Run a SQL block; return @{ Out; Failed }. Failure = duckdb error line or
# non-zero exit (so a function error becomes a FAIL, not a silent pass).
function Invoke-Duck([string]$sql) {
    $full = $loadStmt + $sql
    $out  = ($full | & $duck @duckArgs -batch 2>&1) | Out-String
    $bad  = ($LASTEXITCODE -ne 0) -or ($out -match '(?m)(Error|error:)')
    return @{ Out = $out; Failed = $bad }
}

function Fail([string]$step, [string]$out) {
    Write-Host "FAIL[$step]:" -ForegroundColor Red
    Write-Host $out
    exit 4
}

# --- B. discovery / report selection -----------------------------------------
$reportId = $env:SF_REPORT_ID
$reportNm = ''
if (-not $reportId) {
    Write-Host "[1/3] salesforce_reports('sf') -> first TABULAR report" -ForegroundColor Cyan
    $disc = Invoke-Duck "$attach`n.mode csv`n.headers off`nSELECT Id, Name FROM salesforce_reports('sf') WHERE upper(Format) = 'TABULAR' LIMIT 1;"
    if ($disc.Failed) { Fail 'salesforce_reports' $disc.Out }
    $row = ($disc.Out -split "`r?`n" | Where-Object { $_ -match '^00O' } | Select-Object -First 1)
    if (-not $row) {
        Write-Host "BLOCKED: no accessible TABULAR report" -ForegroundColor Yellow
        Write-Host "(live-smoke blocked by org data, not an extension failure)"
        Write-Host "Inspect availability:  SELECT Id, Name, Format FROM salesforce_reports('sf') LIMIT 20;"
        exit 0
    }
    $parts    = $row -split ',', 2
    $reportId = $parts[0].Trim('"').Trim()
    $reportNm = if ($parts.Count -gt 1) { $parts[1].Trim('"').Trim() } else { '' }
} else {
    Write-Host "[1/3] salesforce_reports('sf') (discovery; using forced SF_REPORT_ID)" -ForegroundColor Cyan
    $disc = Invoke-Duck "$attach`n.mode line`nSELECT Id, Name, Format FROM salesforce_reports('sf') LIMIT 10;"
    if ($disc.Failed) { Fail 'salesforce_reports' $disc.Out }
    Write-Host $disc.Out
}
Write-Host ("chosen report : $reportId  $reportNm")
Write-Host ""

# --- D. sample / oracle -------------------------------------------------------
Write-Host "[2/3] salesforce_report('sf', '$reportId') LIMIT 5  (reserved __sf_report_* columns appended)" -ForegroundColor Cyan
$smp = Invoke-Duck "$attach`n.mode line`nSELECT * FROM salesforce_report('sf', '$reportId') LIMIT 5;"
if ($smp.Failed) { Fail 'salesforce_report' $smp.Out }
Write-Host $smp.Out

# --- E. candidate SOQL --------------------------------------------------------
Write-Host "[3/3] salesforce_report_soql('sf', '$reportId')  (one row; candidate, not contract)" -ForegroundColor Cyan
$sq = Invoke-Duck "$attach`n.mode line`nSELECT report_id, report_name, report_type, base_object, columns, filters, soql, translatable, caveats FROM salesforce_report_soql('sf', '$reportId');"
if ($sq.Failed) { Fail 'salesforce_report_soql' $sq.Out }
Write-Host $sq.Out

Write-Host "=== PASS: all three Report Bridge functions returned without error ===" -ForegroundColor Green
Write-Host "Reminder: __sf_report_* are sample diagnostics; report() is a sample/oracle, not extraction."
Write-Host "If translatable=true, manually run the candidate soql via sf.<Object> LIMIT 5 and compare to the sample."
exit 0
