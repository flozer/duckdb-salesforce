# LIVE smoke runner for scan explainability (ROADMAP v1.6 §19, v0.12.0).
#
# Maintainer-gated manual smoke against a REAL org using the locally-built
# Release shell (build/release/duckdb.exe) — NOT the community extension.
# Credentials are read from the environment ONLY; no secret is printed, logged,
# or written to disk.
#
# PII / data safety: prints SCHEMA METADATA + DIAGNOSTICS ONLY (object/field
# names, flags, pushed/residual, transport, counts). The one real query is
# aggregated to a row count + reserved diagnostics — record data is NOT printed.
#
# What it runs:
#   1. salesforce_metadata_objects('sf') LIMIT N
#   2. salesforce_metadata_fields('sf', '<object>') LIMIT N
#   3. a simple real query (count + a filterable WHERE) — no record rows printed
#   4. salesforce_query_cost()       (last-scan cost summary)
#   5. salesforce_query_explain()    (last-scan field-by-field explanation)
#
# Env (required): SF_CLIENT_ID, SF_CLIENT_SECRET, SF_REFRESH_TOKEN
# Env (optional): SF_LOGIN_URL, SF_API_VERSION, SF_METADATA_OBJECT, LIMIT_N,
#                 DUCKDB_SHELL_PATH, SALESFORCE_EXTENSION_PATH
#
# Exit codes: 0 success / BLOCKED-no-object ; 2 setup error ; 3 BLOCKED missing
# credentials ; 4 FAIL a function errored.

param(
    [string]$MetadataObject = $env:SF_METADATA_OBJECT,
    [int]$LimitN = $(if ($env:LIMIT_N) { [int]$env:LIMIT_N } else { 10 }),
    [string]$DuckDbShellPath = $env:DUCKDB_SHELL_PATH,
    [string]$ExtensionPath = $env:SALESFORCE_EXTENSION_PATH
)

$ErrorActionPreference = 'Stop'

try {
    [Console]::InputEncoding  = [System.Text.UTF8Encoding]::new($false)
    [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $OutputEncoding           = [System.Text.UTF8Encoding]::new($false)
    chcp 65001 > $null 2>&1
} catch {
    # non-Windows / console without code-page support — ignore.
}

$root = Split-Path -Parent $PSScriptRoot

$duck = if ($DuckDbShellPath) { $DuckDbShellPath } else { Join-Path $root 'build/release/duckdb.exe' }
if (-not (Test-Path $duck)) {
    Write-Host "FAIL[setup]: duckdb shell not found: $duck (build: cmake --build build/release --target shell)" -ForegroundColor Red
    exit 2
}
$duckArgs = @()
$loadStmt = ''
if ($ExtensionPath) {
    if (-not (Test-Path $ExtensionPath)) {
        Write-Host "FAIL[setup]: SALESFORCE_EXTENSION_PATH not found: $ExtensionPath" -ForegroundColor Red
        exit 2
    }
    $duckArgs = @('-unsigned')
    $loadStmt = "LOAD '$ExtensionPath';`n"
    $extDesc  = "$ExtensionPath (LOAD, -unsigned)"
} else {
    $localExt = Join-Path $root 'build/release/extension/salesforce/salesforce.duckdb_extension'
    $extDesc = "statically linked in local shell (NOT community); local artifact: $localExt"
}

$missing = @()
foreach ($v in 'SF_CLIENT_ID', 'SF_CLIENT_SECRET', 'SF_REFRESH_TOKEN') {
    if (-not (Get-Item "Env:$v" -ErrorAction SilentlyContinue)) { $missing += $v }
}
if ($missing.Count -gt 0) {
    Write-Host "BLOCKED: missing credentials" -ForegroundColor Yellow
    Write-Host ("missing env vars: " + ($missing -join ', '))
    exit 3
}

$loginUrl = if ($env:SF_LOGIN_URL) { $env:SF_LOGIN_URL } else { 'https://login.salesforce.com' }
$apiOpt   = if ($env:SF_API_VERSION) { ", api_version '$($env:SF_API_VERSION)'" } else { '' }
$attach   = "ATTACH 'salesforce://prod' AS sf (TYPE salesforce, auth_source 'env'$apiOpt);"

$commit = (& git -C $root rev-parse --short HEAD).Trim()
$tag    = ((& git -C $root tag --points-at HEAD) -join ',')
Write-Host "=== Scan explainability live smoke (v0.12.0) ===" -ForegroundColor Cyan
Write-Host ("timestamp  : " + (Get-Date -Format o))
Write-Host ("git commit : $commit  tag: $tag")
Write-Host ("shell      : $duck")
Write-Host ("extension  : $extDesc")
Write-Host ("login_url  : $loginUrl")
Write-Host ("limit_n    : $LimitN")
if ($env:SF_API_VERSION) { Write-Host ("api_version: $($env:SF_API_VERSION)") }
Write-Host "note       : metadata + diagnostics only — no record data is printed."
Write-Host ""

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

# --- 1. objects ---------------------------------------------------------------
Write-Host "[objects] salesforce_metadata_objects('sf') (first $LimitN)" -ForegroundColor Cyan
$o = Invoke-Duck "$attach`n.mode csv`n.headers on`nSELECT object_name, queryable FROM salesforce_metadata_objects('sf') ORDER BY queryable DESC, object_name LIMIT $LimitN;"
if ($o.Failed) { Fail 'salesforce_metadata_objects' $o.Out }
Write-Host $o.Out

# --- choose object: explicit env, else first queryable ------------------------
if (-not $MetadataObject) {
    $d = Invoke-Duck "$attach`n.mode csv`n.headers off`nSELECT object_name FROM salesforce_metadata_objects('sf') WHERE queryable ORDER BY object_name LIMIT 1;"
    if ($d.Failed) { Fail 'salesforce_metadata_objects' $d.Out }
    $MetadataObject = (($d.Out -split "`r?`n") | Where-Object { $_ -ne '' } | Select-Object -First 1).Trim('"').Trim()
}
if (-not $MetadataObject) {
    Write-Host "BLOCKED: no queryable object discovered (org-data block)" -ForegroundColor Yellow
    exit 0
}
Write-Host ("object     : $MetadataObject")
Write-Host ""

# --- 2. fields ----------------------------------------------------------------
Write-Host "[fields] salesforce_metadata_fields('sf', '$MetadataObject') (first $LimitN)" -ForegroundColor Cyan
$f = Invoke-Duck "$attach`n.mode csv`n.headers on`nSELECT field_name, type, filterable, sortable, relationship_name, reference_to FROM salesforce_metadata_fields('sf', '$MetadataObject') ORDER BY field_name LIMIT $LimitN;"
if ($f.Failed) { Fail 'salesforce_metadata_fields' $f.Out }
Write-Host $f.Out

# --- 3+4+5. scan + cost + explain in ONE process -----------------------------
# query_cost/query_explain read the LAST-SCAN diagnostic snapshot, which is
# per-process. The scan, the cost view, and the explain view MUST run in the
# same duckdb invocation — a separate process would see no scan (empty).
# PII-safe: the query is aggregated to a count; no record rows are printed.
Write-Host "[scan+cost+explain] SELECT count(*) ... WHERE Id <> '' then query_cost()/query_explain()" -ForegroundColor Cyan
$combined = @"
$attach
.mode line
SELECT count(*) AS matched_rows FROM (SELECT Id FROM sf.$MetadataObject WHERE Id <> '' LIMIT 50) t;
.print ''
.print '--- salesforce_query_cost() ---'
SELECT object, transport, transport_reason, pushed_filters, residual_filters, where_pushed, count_pushdown, query_mode FROM salesforce_query_cost();
.print ''
.print '--- salesforce_query_explain() ---'
.mode csv
.headers on
SELECT object_name, field_name, role, resolved, filterable, pushed, residual, reason FROM salesforce_query_explain() ORDER BY role, field_name;
"@
$r = Invoke-Duck $combined
if ($r.Failed) { Fail 'scan+cost+explain' $r.Out }
Write-Host $r.Out

Write-Host "=== PASS: metadata + cost + explain returned without error ===" -ForegroundColor Green
Write-Host "Reminder: read-only diagnostics; no record data was printed. query_explain shows"
Write-Host "whether Salesforce filtered server-side (pushed) or DuckDB filtered residually."
exit 0
