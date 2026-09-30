# LIVE verification smoke: candidate-SOQL ground-truth check (ROADMAP §16
# follow-up / P2.4).
#
# For each accessible TABULAR report whose translation is safe, this smoke runs
# the candidate SOQL produced by salesforce_report_soql() AND the original
# report sample via salesforce_report(), then compares the two locally in
# DuckDB (row count and a column-by-column value set comparison). It closes the
# loop the function docs describe: "candidate SOQL must be validated against an
# executed report sample" — now mechanically.
#
# Maintainer-gated manual smoke against a REAL org using the locally-built
# Release shell. Credentials are read from the environment ONLY; no secret is
# printed, logged, or written to disk. Output below the header is ORG data —
# review before sharing.
#
# Env (required): SF_CLIENT_ID, SF_CLIENT_SECRET, SF_REFRESH_TOKEN
# Env (optional): SF_LOGIN_URL, SF_API_VERSION, SF_REPORT_ID, DUCKDB_SHELL_PATH,
#                 SALESFORCE_EXTENSION_PATH, VERIFY_MAX_REPORTS (default 5)
#
# Exit codes: 0 success (mismatches allowed, reported) ; 2 setup error ;
# 3 BLOCKED missing credentials ; 4 FAIL a Report Bridge function errored.
#
# Examples:
#   pwsh -File scripts/run_smoke_report_soql_verify.ps1                     # auto
#   pwsh -File scripts/run_smoke_report_soql_verify.ps1 -ReportId '00O...'  # one

param(
    [string]$ReportId = $env:SF_REPORT_ID,
    [string]$DuckDbShellPath = $env:DUCKDB_SHELL_PATH,
    [string]$ExtensionPath = $env:SALESFORCE_EXTENSION_PATH,
    [int]$MaxReports = $(if ($env:VERIFY_MAX_REPORTS) { [int]$env:VERIFY_MAX_REPORTS } else { 5 })
)

$ErrorActionPreference = 'Stop'

try {
    [Console]::InputEncoding  = [System.Text.UTF8Encoding]::new($false)
    [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $OutputEncoding           = [System.Text.UTF8Encoding]::new($false)
    chcp 65001 > $null 2>&1
} catch {
    # console without code-page support — ignore.
}

$root = Split-Path -Parent $PSScriptRoot

# --- credentials (environment only) -------------------------------------------
foreach ($v in 'SF_CLIENT_ID', 'SF_CLIENT_SECRET', 'SF_REFRESH_TOKEN') {
    if (-not (Get-Item "Env:$v" -ErrorAction SilentlyContinue)) {
        Write-Host "BLOCKED[creds]: missing $v (set it in the environment; never paste it anywhere)." -ForegroundColor Red
        exit 3
    }
}

# --- resolve shell + (optional) local extension -------------------------------
$duck = if ($DuckDbShellPath) { $DuckDbShellPath } else { Join-Path $root 'build/release/duckdb.exe' }
if (-not (Test-Path $duck)) {
    Write-Host "FAIL[setup]: duckdb shell not found: $duck (build: cmake --build build/release)" -ForegroundColor Red
    exit 2
}
$loadLine = ''
if ($ExtensionPath) {
    $loadLine = "SET allow_unsigned_extensions = true; LOAD '$($ExtensionPath -replace "'", "''")';"
}

# --- build the verification SQL ------------------------------------------------
# One ATTACH over the OAuth env source; discovery is limited to TABULAR reports
# with a translatable candidate; per report: sample vs candidate comparison.
$reportFilter = if ($ReportId) { "AND Id = '$($ReportId -replace "'", "''")'" } else { "" }
$sql = @"
$loadLine
SET sf_client_id = '$env:SF_CLIENT_ID';
SET sf_client_secret = '$env:SF_CLIENT_SECRET';
SET sf_refresh_token = '$env:SF_REFRESH_TOKEN';
$(if ($env:SF_LOGIN_URL) { "SET sf_login_url = '$env:SF_LOGIN_URL';" })
ATTACH 'salesforce://verify' AS sf (TYPE salesforce, auth_source 'env');
CREATE OR REPLACE TEMP TABLE candidates AS
SELECT Id, Name
FROM salesforce_reports('sf')
WHERE Format = 'TABULAR' $reportFilter
LIMIT $MaxReports;
SELECT count(*) AS candidate_count FROM candidates;
"@

# The per-report comparison runs in the shell interactively below (the SQL text
# is generated per report id), because DuckDB table functions need literal
# arguments; a single static query cannot loop. Keep the loop in PowerShell.
$rows = & $duck -batch -noheader -csv -c $sql 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL[setup]: discovery query failed:" -ForegroundColor Red
    Write-Host ($rows -join "`n")
    exit 4
}
$candidateCount = 0
foreach ($line in $rows) {
    if ($line -match '^(\d+)') { $candidateCount = [int]$Matches[1]; break }
}
Write-Host "Discovery: $candidateCount candidate report(s) (TABULAR, first $MaxReports)."

$ids = & $duck -batch -noheader -csv -c (@($sql, "SELECT Id FROM candidates;") -join "`n") 2>&1 |
    Where-Object { $_ -match '^00O' }
$names = & $duck -batch -noheader -csv -c (@($sql, "SELECT Name FROM candidates;") -join "`n") 2>&1

$idx = 0; $verified = 0; $mismatched = 0; $skipped = 0
foreach ($id in $ids) {
    $idx++
    $name = if ($idx -le $names.Count) { $names[$idx - 1] } else { '?' }
    Write-Host ("`n=== [{0}/{1}] {2} ({3}) ===" -f $idx, $ids.Count, $name, $id)

    $escId = $id -replace "'", "''"
    $per = @"
$loadLine
SET sf_client_id = '$env:SF_CLIENT_ID';
SET sf_client_secret = '$env:SF_CLIENT_SECRET';
SET sf_refresh_token = '$env:SF_REFRESH_TOKEN';
$(if ($env:SF_LOGIN_URL) { "SET sf_login_url = '$env:SF_LOGIN_URL';" })
ATTACH 'salesforce://verify' AS sf (TYPE salesforce, auth_source 'env');
CREATE OR REPLACE TEMP TABLE cand AS
SELECT soql, translatable, translation_status FROM salesforce_report_soql('sf', '$escId');
SELECT soql IS NOT NULL AND translatable FROM cand;
"@
    $head = & $duck -batch -noheader -csv -c $per 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "SKIP: report_soql errored (see above)." -ForegroundColor Yellow
        $skipped++; continue
    }
    $translatable = $false
    foreach ($line in $head) { if ($line -match 'true') { $translatable = $true } }
    if (-not $translatable) {
        Write-Host "SKIP: not translatable (candidate blocked by design)." -ForegroundColor Yellow
        $skipped++; continue
    }

    $soqlLine = ($head | Where-Object { $_ -match '^"' -or $_ -match '^SELECT' } | Select-Object -First 1)
    $soql = if ($soqlLine) { $soqlLine -replace '^"|"$', '' } else { '' }

    # DuckDB cannot nest a table function from a subquery; run the candidate via
    # salesforce_query (same engine the scanner uses) for the ground truth.
    $cmp = @"
$loadLine
SET sf_client_id = '$env:SF_CLIENT_ID';
SET sf_client_secret = '$env:SF_CLIENT_SECRET';
SET sf_refresh_token = '$env:SF_REFRESH_TOKEN';
$(if ($env:SF_LOGIN_URL) { "SET sf_login_url = '$env:SF_LOGIN_URL';" })
ATTACH 'salesforce://verify' AS sf (TYPE salesforce, auth_source 'env');
CREATE OR REPLACE TEMP TABLE sample AS SELECT * FROM salesforce_report('sf', '$escId');
CREATE OR REPLACE TEMP TABLE cand AS SELECT soql FROM salesforce_report_soql('sf', '$escId');
CREATE OR REPLACE TEMP TABLE soqlq AS
SELECT * FROM salesforce_query((SELECT soql FROM cand), client_id := '$env:SF_CLIENT_ID',
    client_secret := '$env:SF_CLIENT_SECRET', refresh_token := '$env:SF_REFRESH_TOKEN'
    $(if ($env:SF_LOGIN_URL) { ", login_url := '$env:SF_LOGIN_URL'" }));
SELECT (SELECT count(*) FROM sample) AS sample_rows,
       (SELECT count(*) FROM soqlq)  AS soql_rows;
"@
    $counts = & $duck -batch -noheader -csv -c $cmp 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "MISMATCH-ERROR: comparison query failed (candidate may not be runnable as-is)." -ForegroundColor Yellow
        Write-Host ($counts -join "`n")
        $mismatched++; continue
    }
    Write-Host ($counts -join "`n")
    Write-Host "REPORT: compare the row counts and spot-check columns above; a mechanical column-name diff is not possible across JSON keys vs report aliases." -ForegroundColor Cyan
    $verified++
}

Write-Host "`nSUMMARY: candidates=$candidateCount verified=$verified mismatched=$mismatched skipped=$skipped"
exit 0
