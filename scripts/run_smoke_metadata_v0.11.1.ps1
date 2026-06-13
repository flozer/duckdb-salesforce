# LIVE smoke runner for Metadata Engine v2 (ROADMAP v1.6 §17, v0.11.1).
#
# Maintainer-gated manual smoke against a REAL org using the locally-built
# Release shell (build/release/duckdb.exe) — NOT the community extension.
# Credentials are read from the environment ONLY; no secret is printed, logged,
# or written to disk.
#
# PII / data safety: this runner prints SCHEMA METADATA ONLY (object names,
# field names, types, queryable/filterable flags, reference targets, picklist
# values). It NEVER selects or prints record data, and never prints secrets.
#
# What it runs:
#   1. salesforce_metadata_objects('sf')              -> object_name, queryable (LIMIT)
#   2. salesforce_metadata_fields('sf', '<object>')   -> field metadata (LIMIT)
#   3. salesforce_refresh_metadata('sf')              -> invalidation
#   4. salesforce_metadata_objects('sf') again        -> proves it still works post-refresh
#
# Env (required): SF_CLIENT_ID, SF_CLIENT_SECRET, SF_REFRESH_TOKEN
# Env (optional): SF_LOGIN_URL, SF_API_VERSION, SF_METADATA_OBJECT, LIMIT_N,
#                 DUCKDB_SHELL_PATH, SALESFORCE_EXTENSION_PATH
#
# Exit codes: 0 success / BLOCKED-no-object ; 2 setup error ; 3 BLOCKED missing
# credentials ; 4 FAIL a metadata function errored.
#
# Examples:
#   pwsh -File scripts/run_smoke_metadata_v0.11.1.ps1
#   $env:SF_METADATA_OBJECT='Account'; pwsh -File scripts/run_smoke_metadata_v0.11.1.ps1

param(
    [string]$MetadataObject = $env:SF_METADATA_OBJECT,
    [int]$LimitN = $(if ($env:LIMIT_N) { [int]$env:LIMIT_N } else { 10 }),
    [string]$DuckDbShellPath = $env:DUCKDB_SHELL_PATH,
    [string]$ExtensionPath = $env:SALESFORCE_EXTENSION_PATH
)

$ErrorActionPreference = 'Stop'

# DuckDB emits UTF-8. Force UTF-8 on the Windows console so accents render.
try {
    [Console]::InputEncoding  = [System.Text.UTF8Encoding]::new($false)
    [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $OutputEncoding           = [System.Text.UTF8Encoding]::new($false)
    chcp 65001 > $null 2>&1
} catch {
    # non-Windows / console without code-page support — ignore.
}

$root = Split-Path -Parent $PSScriptRoot

# --- resolve shell + (optional) local extension ------------------------------
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

# --- credential preflight (names only, never values) -------------------------
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

# --- evidence header ----------------------------------------------------------
$commit = (& git -C $root rev-parse --short HEAD).Trim()
$tag    = ((& git -C $root tag --points-at HEAD) -join ',')
Write-Host "=== Metadata Engine v2 live smoke (v0.11.1) ===" -ForegroundColor Cyan
Write-Host ("timestamp  : " + (Get-Date -Format o))
Write-Host ("git commit : $commit  tag: $tag")
Write-Host ("shell      : $duck")
Write-Host ("extension  : $extDesc")
Write-Host ("login_url  : $loginUrl")
Write-Host ("limit_n    : $LimitN")
if ($env:SF_API_VERSION) { Write-Host ("api_version: $($env:SF_API_VERSION)") }
Write-Host "note       : schema metadata only — no record data is selected or printed."
Write-Host ""

# Run a SQL block; return @{ Out; Failed }. Failure = duckdb error or non-zero exit.
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

# --- 1. objects: count + queryable breakdown + sample ------------------------
Write-Host "[objects] salesforce_metadata_objects('sf')  (counts + first $LimitN; schema metadata only)" -ForegroundColor Cyan
$objSql = @"
$attach
.mode line
SELECT count(*) AS total_objects,
       count(*) FILTER (WHERE queryable) AS queryable_objects,
       count(*) FILTER (WHERE NOT queryable) AS non_queryable_objects
FROM salesforce_metadata_objects('sf');
.mode csv
.headers on
SELECT object_name, queryable
FROM salesforce_metadata_objects('sf')
ORDER BY queryable DESC, object_name
LIMIT $LimitN;
"@
$o = Invoke-Duck $objSql
if ($o.Failed) { Fail 'salesforce_metadata_objects' $o.Out }
Write-Host $o.Out

# --- choose an object: explicit env, else first queryable --------------------
if (-not $MetadataObject) {
    $d = Invoke-Duck "$attach`n.mode csv`n.headers off`nSELECT object_name FROM salesforce_metadata_objects('sf') WHERE queryable ORDER BY object_name LIMIT 1;"
    if ($d.Failed) { Fail 'salesforce_metadata_objects' $d.Out }
    $MetadataObject = (($d.Out -split "`r?`n") | Where-Object { $_ -ne '' } | Select-Object -First 1).Trim('"').Trim()
}
if (-not $MetadataObject) {
    Write-Host "BLOCKED: no queryable object discovered (org-data block, not an extension failure)" -ForegroundColor Yellow
    exit 0
}
Write-Host ("object     : $MetadataObject")
Write-Host ""

# --- 2. fields for the chosen object -----------------------------------------
Write-Host "[fields] salesforce_metadata_fields('sf', '$MetadataObject')  (first $LimitN; schema metadata only)" -ForegroundColor Cyan
$fldSql = @"
$attach
.mode line
SELECT count(*) AS total_fields,
       count(*) FILTER (WHERE filterable) AS filterable_fields,
       count(*) FILTER (WHERE len(reference_to) > 0) AS reference_fields,
       count(*) FILTER (WHERE len(picklist_values) > 0) AS picklist_fields
FROM salesforce_metadata_fields('sf', '$MetadataObject');
.mode csv
.headers on
SELECT field_name, type, filterable, sortable, relationship_name, reference_to, picklist_values
FROM salesforce_metadata_fields('sf', '$MetadataObject')
ORDER BY field_name
LIMIT $LimitN;
"@
$f = Invoke-Duck $fldSql
if ($f.Failed) { Fail 'salesforce_metadata_fields' $f.Out }
Write-Host $f.Out

# --- 3. refresh (invalidation) -----------------------------------------------
Write-Host "[refresh] salesforce_refresh_metadata('sf')  (drops global + object cache)" -ForegroundColor Cyan
$r = Invoke-Duck "$attach`n.mode line`nSELECT catalog, scope, object FROM salesforce_refresh_metadata('sf');"
if ($r.Failed) { Fail 'salesforce_refresh_metadata' $r.Out }
Write-Host $r.Out

# --- 4. repeat objects query: proves engine still works after invalidation ---
Write-Host "[re-read] salesforce_metadata_objects('sf') after refresh (proves re-fetch works)" -ForegroundColor Cyan
$o2 = Invoke-Duck "$attach`n.mode line`nSELECT count(*) AS total_objects_after_refresh FROM salesforce_metadata_objects('sf');"
if ($o2.Failed) { Fail 'salesforce_metadata_objects(post-refresh)' $o2.Out }
Write-Host $o2.Out

Write-Host "=== PASS: all metadata functions returned without error ===" -ForegroundColor Green
Write-Host "Reminder: these are read-only schema diagnostics; no record data was read or printed."
exit 0
