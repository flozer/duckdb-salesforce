# LIVE smoke runner for the Report Bridge (ROADMAP §16).
#
# Maintainer-gated manual smoke against a REAL org using the locally-built
# Release shell (build/release/duckdb.exe) — NOT the community extension.
# Credentials are read from the environment ONLY; no secret is printed, logged,
# or written to disk. Output below the header is ORG data — review before sharing.
#
# Modes:
#   (default)        discover reports, pick the first TABULAR, sample + report_soql.
#   -ListReports     list accessible report definitions only (no run).
#   -ReportId '00O…' run sample + report_soql against exactly that report id.
#
# Env (required): SF_CLIENT_ID, SF_CLIENT_SECRET, SF_REFRESH_TOKEN
# Env (optional): SF_LOGIN_URL, SF_API_VERSION, SF_REPORT_ID, DUCKDB_SHELL_PATH,
#                 SALESFORCE_EXTENSION_PATH
#
# Exit codes: 0 success / BLOCKED-no-tabular ; 2 setup error ; 3 BLOCKED missing
# credentials ; 4 FAIL a Report Bridge function errored.
#
# Examples:
#   pwsh -File scripts/run_smoke_report_bridge.ps1                       # auto
#   pwsh -File scripts/run_smoke_report_bridge.ps1 -ListReports          # list all
#   pwsh -File scripts/run_smoke_report_bridge.ps1 -ListReports -Format TABULAR
#   pwsh -File scripts/run_smoke_report_bridge.ps1 -ReportId '00O...'    # explicit

param(
    [switch]$ListReports,
    [string]$ReportId = $env:SF_REPORT_ID,
    [string]$Format = "",
    [string]$DuckDbShellPath = $env:DUCKDB_SHELL_PATH,
    [string]$ExtensionPath = $env:SALESFORCE_EXTENSION_PATH
)

$ErrorActionPreference = 'Stop'

# DuckDB emits UTF-8. Force UTF-8 on the Windows console/capture so accents and
# any table glyphs render correctly instead of CP437/CP850 mojibake
# (e.g. "Relatórios" not "Relat├│rios").
try {
    [Console]::InputEncoding  = [System.Text.UTF8Encoding]::new($false)
    [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $OutputEncoding           = [System.Text.UTF8Encoding]::new($false)
    chcp 65001 > $null 2>&1
} catch {
    # non-Windows / console without code-page support — ignore.
}

$root = Split-Path -Parent $PSScriptRoot

# --- mode resolution ----------------------------------------------------------
if ($ListReports -and $ReportId) {
    Write-Host "FAIL[setup]: choose one mode — either -ListReports or -ReportId, not both." -ForegroundColor Red
    exit 2
}
$mode = if ($ListReports) { 'list' } elseif ($ReportId) { 'explicit' } else { 'auto' }

# --- resolve shell + (optional) local extension ------------------------------
$duck = if ($DuckDbShellPath) { $DuckDbShellPath } else { Join-Path $root 'build/release/duckdb.exe' }
if (-not (Test-Path $duck)) {
    Write-Host "FAIL[setup]: duckdb shell not found: $duck (build: cmake --build build/release)" -ForegroundColor Red
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
Write-Host "=== Report Bridge live smoke ===" -ForegroundColor Cyan
Write-Host ("timestamp  : " + (Get-Date -Format o))
Write-Host ("git commit : $commit  tag: $tag")
Write-Host ("shell      : $duck")
Write-Host ("extension  : $extDesc")
Write-Host ("mode       : $mode")
Write-Host ("login_url  : $loginUrl")
if ($env:SF_API_VERSION) { Write-Host ("api_version: $($env:SF_API_VERSION)") }
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

# --- LIST mode ----------------------------------------------------------------
if ($mode -eq 'list') {
    $where = if ($Format) { "WHERE upper(Format) = upper('$Format')" } else { '' }
    Write-Host ("[list] salesforce_reports('sf') " + $(if ($Format) { "(Format = $Format)" } else { '(all)' })) -ForegroundColor Cyan
    # CSV (not the Unicode-border table): readable, copyable, log-friendly evidence.
    $sql = @"
$attach
.mode csv
.headers on
SELECT Id, Name, DeveloperName, FolderName, Format
FROM salesforce_reports('sf') $where
ORDER BY (upper(Format) = 'TABULAR') DESC, Name;
.headers off
SELECT count(*) AS total_reports FROM salesforce_reports('sf') $where;
"@
    $r = Invoke-Duck $sql
    if ($r.Failed) { Fail 'salesforce_reports' $r.Out }
    Write-Host $r.Out
    Write-Host "=== list complete (definitions only; no report executed) ===" -ForegroundColor Green
    exit 0
}

# --- choose report id (explicit or auto-discover first TABULAR) ---------------
$reportNm = ''
if ($mode -eq 'explicit') {
    Write-Host "[discovery] looking up name for SF_REPORT_ID=$ReportId (evidence only)" -ForegroundColor Cyan
    $d = Invoke-Duck "$attach`n.mode csv`n.headers off`nSELECT Name FROM salesforce_reports('sf') WHERE Id = '$ReportId' LIMIT 1;"
    if (-not $d.Failed) { $reportNm = (($d.Out -split "`r?`n") | Where-Object { $_ -ne '' } | Select-Object -First 1) }
} else {
    Write-Host "[discovery] salesforce_reports('sf') -> first TABULAR report" -ForegroundColor Cyan
    $d = Invoke-Duck "$attach`n.mode csv`n.headers off`nSELECT Id, Name FROM salesforce_reports('sf') WHERE upper(Format) = 'TABULAR' LIMIT 1;"
    if ($d.Failed) { Fail 'salesforce_reports' $d.Out }
    $row = (($d.Out -split "`r?`n") | Where-Object { $_ -match '^00O' } | Select-Object -First 1)
    if (-not $row) {
        Write-Host "BLOCKED: no accessible TABULAR report (org-data block, not an extension failure)" -ForegroundColor Yellow
        Write-Host "Inspect:  pwsh -File scripts/run_smoke_report_bridge.ps1 -ListReports"
        exit 0
    }
    $parts    = $row -split ',', 2
    $ReportId = $parts[0].Trim('"').Trim()
    $reportNm = if ($parts.Count -gt 1) { $parts[1].Trim('"').Trim() } else { '' }
}
Write-Host ("report_id   : $ReportId")
Write-Host ("report_name : $reportNm")
Write-Host ""

# --- D. sample / oracle (PII-safe: count + reserved columns only, no rows) ----
Write-Host "[sample] salesforce_report('sf', '$ReportId') LIMIT 5  (proving row count + __sf_report_* presence; report data rows NOT printed)" -ForegroundColor Cyan
$smp = Invoke-Duck "$attach`n.mode line`nSELECT count(*) AS sample_rows, any_value(__sf_report_truncated) AS truncated, any_value(__sf_report_all_data) AS all_data, any_value(__sf_report_max_rows) AS max_rows FROM (SELECT * FROM salesforce_report('sf', '$ReportId') LIMIT 5) t;"
if ($smp.Failed) { Fail 'salesforce_report' $smp.Out }
Write-Host $smp.Out

# --- E. candidate SOQL (structured diagnostics; no report data rows) ----------
Write-Host "[candidate soql] salesforce_report_soql('sf', '$ReportId')  (one row; candidate, not contract)" -ForegroundColor Cyan
$sq = Invoke-Duck "$attach`n.mode line`nSELECT base_object, base_object_resolved_by, translation_status, blocked_by, translatable, soql, unresolved_columns, unresolved_filters, confidence, caveats FROM salesforce_report_soql('sf', '$ReportId');"
if ($sq.Failed) { Fail 'salesforce_report_soql' $sq.Out }
Write-Host $sq.Out

Write-Host "=== PASS: all functions returned without error ===" -ForegroundColor Green
Write-Host "Reminder: report() is a sample/oracle (<=2000 rows), not extraction. If translatable=true,"
Write-Host "manually run the candidate soql via sf.<Object> LIMIT 5 and compare to the sample."
exit 0
