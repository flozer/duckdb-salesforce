# LIVE smoke runner: aggregate precision (P1.4 decision evidence).
#
# Compares Salesforce server-side SUM/AVG (via salesforce_aggregate) against
# local DuckDB DECIMAL aggregation over the SAME predicate window, and prints
# both plus the numeric delta. Read-only, small quota footprint (2 aggregate
# calls + one bounded row scan).
#
# Maintainer-gated manual smoke against a REAL org using the locally-built
# Release shell. Credentials are read from the environment ONLY (SF_CLIENT_ID,
# SF_CLIENT_SECRET, SF_REFRESH_TOKEN; optional SF_LOGIN_URL). No secret is
# printed, logged, or written to disk. Aggregate output below the header is
# ORG data — review before sharing.
#
# Parameters: -Object, -Field, -Where (SOQL predicate), -Since (timestamp used
# as CreatedDate >= <Since> when -Where is empty). Defaults exercise a custom
# currency field on the maintainer org; adjust per org.
#
# Exit codes: 0 success ; 2 setup error ; 3 missing credentials.

param(
    [string]$Object = 'Produto_Oferta__c',
    [string]$Field = 'VlrLiq__c',
    [string]$Where = '',
    [string]$Since = '2026-01-01T00:00:00Z',
    [string]$DuckDbShellPath = $env:DUCKDB_SHELL_PATH,
    [string]$ExtensionPath = $env:SALESFORCE_EXTENSION_PATH
)

$ErrorActionPreference = 'Stop'
foreach ($v in 'SF_CLIENT_ID', 'SF_CLIENT_SECRET', 'SF_REFRESH_TOKEN') {
    if (-not (Get-Item "Env:$v" -ErrorAction SilentlyContinue)) {
        Write-Host "BLOCKED[creds]: missing $v (environment only; never paste)." -ForegroundColor Red
        exit 3
    }
}
$duck = if ($DuckDbShellPath) { $DuckDbShellPath } else { Join-Path (Split-Path -Parent $PSScriptRoot) 'build/release/duckdb.exe' }
if (-not (Test-Path $duck)) {
    Write-Host "FAIL[setup]: duckdb shell not found: $duck" -ForegroundColor Red
    exit 2
}
$loadLine = if ($ExtensionPath) { "SET allow_unsigned_extensions = true; LOAD '$($ExtensionPath -replace "'", "''")';" } else { '' }
$pred = if ($Where) { $Where } else { "CreatedDate >= $Since" }
$escPred = $pred -replace "'", "''"
$login = if ($env:SF_LOGIN_URL) { ", login_url := '$($env:SF_LOGIN_URL -replace "'", "''")'" } else { '' }

$sql = @"
$loadLine
ATTACH 'salesforce://precision' AS sf (TYPE salesforce,
    client_id := '$($env:SF_CLIENT_ID -replace "'", "''")',
    client_secret := '$($env:SF_CLIENT_SECRET -replace "'", "''")',
    refresh_token := '$($env:SF_REFRESH_TOKEN -replace "'", "''")'$login);

-- server-side (Salesforce computes; one aggregate API call)
CREATE OR REPLACE TEMP TABLE srv AS SELECT * FROM salesforce_aggregate('sf', '$Object',
    'COUNT(Id) n, COUNT($Field) n_f, SUM($Field) s, AVG($Field) a, MIN($Field) mn, MAX($Field) mx',
    '$escPred');

-- local (DuckDB fetches rows in the window, exact DECIMAL arithmetic)
CREATE OR REPLACE TEMP TABLE loc AS
SELECT COUNT(Id) n, COUNT($Field) n_f, SUM($Field) s, AVG($Field) a, MIN($Field) mn, MAX($Field) mx
FROM sf.$Object WHERE $pred;

SELECT
  (SELECT n FROM srv)                    AS srv_rows,
  (SELECT n FROM loc)                    AS loc_rows,
  (SELECT s FROM srv)                    AS srv_sum_raw,
  TRY_CAST((SELECT s FROM srv) AS DOUBLE) AS srv_sum,
  (SELECT s FROM loc)                    AS loc_sum,
  TRY_CAST((SELECT s FROM srv) AS DOUBLE) - (SELECT s FROM loc) AS sum_delta,
  (SELECT a FROM srv)                    AS srv_avg_raw,
  (SELECT mn FROM srv) AS srv_min, (SELECT mx FROM srv) AS srv_max,
  (SELECT mn FROM loc) AS loc_min, (SELECT mx FROM loc) AS loc_max;
"@

Write-Host "Object=$Object Field=$Field Predicate=$pred"
$out = & $duck -batch -c $sql 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL:" -ForegroundColor Red
    Write-Host ($out -join "`n")
    exit 2
}
Write-Host ($out -join "`n")
Write-Host "`nDecision rule (P1.4): sum_delta ~ 0 => transparent SUM is safe; a material delta proves the NO-GO (server SUM is floating point)."
exit 0
