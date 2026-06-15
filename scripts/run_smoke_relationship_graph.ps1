# LIVE smoke runner for salesforce_relationship_graph() (ROADMAP v1.6 §18 cut 1/2
# — parents + opt-in child relationships).
#
# Maintainer-gated manual smoke against a REAL org using the locally-built
# Release shell (build/release/duckdb.exe) — NOT the community extension.
# Credentials are read from the environment ONLY; no secret is printed or stored.
#
# PII / data safety: prints SCHEMA METADATA ONLY (object names, relationship
# names, target objects, edge status). No record data is selected or printed.
#
# What it runs:
#   1. pick a queryable object (-Object / SF_REL_OBJECT, else first queryable)
#   2. salesforce_relationship_graph('sf', <object>, <max_depth>) — parent edges
#      with status (resolved/polymorphic/self_reference/cyclic/not_queryable/
#      not_describable). With -IncludeChildren, also lists the object's direct
#      child relationships (direction='child'; status incl. unnamed_child).
#   3. a status summary (counts per direction + status) — metadata only
#
# Flags: -Object <name>  -MaxDepth <1..4>  -IncludeChildren
#
# Env (required): SF_CLIENT_ID, SF_CLIENT_SECRET, SF_REFRESH_TOKEN
# Env (optional): SF_LOGIN_URL, SF_API_VERSION, SF_REL_OBJECT, SF_REL_DEPTH,
#                 LIMIT_N, DUCKDB_SHELL_PATH, SALESFORCE_EXTENSION_PATH
#
# Exit: 0 success / BLOCKED-no-object ; 2 setup error ; 3 missing creds ; 4 FAIL.

param(
    [string]$Object = $env:SF_REL_OBJECT,
    [int]$MaxDepth = $(if ($env:SF_REL_DEPTH) { [int]$env:SF_REL_DEPTH } else { 2 }),
    [switch]$IncludeChildren,
    [int]$LimitN = $(if ($env:LIMIT_N) { [int]$env:LIMIT_N } else { 25 }),
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
    # non-Windows / no code-page support — ignore.
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
Write-Host "=== Relationship graph live smoke (§18 cut 1/2) ===" -ForegroundColor Cyan
Write-Host ("timestamp  : " + (Get-Date -Format o))
Write-Host ("git commit : $commit  tag: $tag")
Write-Host ("shell      : $duck")
Write-Host ("extension  : $extDesc")
Write-Host ("login_url  : $loginUrl")
Write-Host ("max_depth  : $MaxDepth")
Write-Host ("children   : $([bool]$IncludeChildren)  (include_children opt-in)")
Write-Host "note       : schema metadata only — no record data is read or printed."
Write-Host ""

# Opt-in child relationships (§18 cut 2). Default off -> parent-only.
$childArg = if ($IncludeChildren) { ', include_children := true' } else { '' }

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

# --- pick object --------------------------------------------------------------
if (-not $Object) {
    $d = Invoke-Duck "$attach`n.mode csv`n.headers off`nSELECT object_name FROM salesforce_metadata_objects('sf') WHERE queryable ORDER BY object_name LIMIT 1;"
    if ($d.Failed) { Fail 'salesforce_metadata_objects' $d.Out }
    $Object = (($d.Out -split "`r?`n") | Where-Object { $_ -ne '' } | Select-Object -First 1).Trim('"').Trim()
}
if (-not $Object) {
    Write-Host "BLOCKED: no queryable object discovered (org-data block)" -ForegroundColor Yellow
    exit 0
}
Write-Host ("object     : $Object")
Write-Host ""

# --- relationship graph (edges + status) -------------------------------------
Write-Host "[graph] salesforce_relationship_graph('sf', '$Object', $MaxDepth$childArg)  (first $LimitN; schema metadata only)" -ForegroundColor Cyan
$g = Invoke-Duck "$attach`n.mode csv`n.headers on`nSELECT path, relationship_name, target_object, depth_level, direction, status FROM salesforce_relationship_graph('sf', '$Object', $MaxDepth$childArg) ORDER BY direction, path LIMIT $LimitN;"
if ($g.Failed) { Fail 'salesforce_relationship_graph' $g.Out }
Write-Host $g.Out

# --- status summary (counts only, by direction) ------------------------------
Write-Host "[summary] edge count per direction + status" -ForegroundColor Cyan
$s = Invoke-Duck "$attach`n.mode box`nSELECT direction, status, count(*) AS edges FROM salesforce_relationship_graph('sf', '$Object', $MaxDepth$childArg) GROUP BY direction, status ORDER BY direction, status;"
if ($s.Failed) { Fail 'salesforce_relationship_graph' $s.Out }
Write-Host $s.Out

Write-Host "=== PASS: relationship graph returned without error ===" -ForegroundColor Green
Write-Host "Reminder: read-only metadata; no record data was read or printed. Statuses explain"
Write-Host "which parents resolved vs polymorphic/self/cyclic/not-queryable/not-describable."
exit 0
