# LIVE smoke runner for v0.9.0. Runs scripts/smoke_v0.9.sql against a REAL org
# using the statically-linked Release shell. Reads credentials from the
# environment ONLY — nothing is hard-coded, logged, or written to disk.
#
# Usage (env auth):
#   $env:SF_CLIENT_ID     = '...'
#   $env:SF_CLIENT_SECRET = '...'
#   $env:SF_REFRESH_TOKEN = '...'
#   # optional sandbox: $env:SF_LOGIN_URL = 'https://test.salesforce.com'
#   pwsh -File scripts/run_smoke_v0.9.ps1
#
# JWT variant: set SF_JWT_KEY_FILE + edit smoke_v0.9.sql's ATTACH to
#   auth_source 'jwt', client_id '...', username '...'   (Connected App pre-authorized)
#
# The output is ORG data — review before sharing. No secret is ever printed.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$duck = Join-Path $root 'build/release/duckdb.exe'
$sql  = Join-Path $PSScriptRoot 'smoke_v0.9.sql'

if (-not (Test-Path $duck)) {
    Write-Error "shell not found: $duck (build Release first — see docs/INSTALL.md)"
}
if (-not (Test-Path $sql)) {
    Write-Error "smoke script not found: $sql"
}

$haveEnv = $env:SF_CLIENT_ID -and $env:SF_CLIENT_SECRET -and $env:SF_REFRESH_TOKEN
$haveSfdx = [bool]$env:SF_SFDX_AUTH_URL
$haveJwt = $env:SF_JWT_KEY_FILE
if (-not ($haveEnv -or $haveSfdx -or $haveJwt)) {
    Write-Error "no credentials in env. Set SF_CLIENT_ID/SF_CLIENT_SECRET/SF_REFRESH_TOKEN (env auth), or SF_SFDX_AUTH_URL, or SF_JWT_KEY_FILE (jwt). See header."
}

Write-Host "Running live smoke against the configured org (output is org data)..." -ForegroundColor Cyan
# -batch: non-interactive; read SQL from stdin.
Get-Content -Raw $sql | & $duck -batch
$code = $LASTEXITCODE
Write-Host ""
if ($code -eq 0) {
    Write-Host "smoke exit code: 0 (review the output above; nothing was persisted)" -ForegroundColor Green
} else {
    Write-Host "smoke exit code: $code (FAILED)" -ForegroundColor Red
}
exit $code
