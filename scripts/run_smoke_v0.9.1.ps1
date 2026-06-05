# Light smoke runner for v0.9.1. Runs scripts/smoke_v0.9.1.sql against a REAL
# org with the statically-linked Release shell, env credentials only. Nothing is
# hard-coded, logged, or persisted.
#
# Usage (env auth):
#   $env:SF_CLIENT_ID = '...'; $env:SF_CLIENT_SECRET = '...'; $env:SF_REFRESH_TOKEN = '...'
#   pwsh -File scripts/run_smoke_v0.9.1.ps1
# (or set SF_SFDX_AUTH_URL, or SF_JWT_KEY_FILE + edit the ATTACH line for jwt.)
#
# Output is org configuration metadata + row counts — review before sharing.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$duck = Join-Path $root 'build/release/duckdb.exe'
$sql  = Join-Path $PSScriptRoot 'smoke_v0.9.1.sql'

if (-not (Test-Path $duck)) {
    Write-Error "shell not found: $duck (build Release first — see docs/INSTALL.md)"
}
if (-not (Test-Path $sql)) {
    Write-Error "smoke script not found: $sql"
}

$haveEnv = $env:SF_CLIENT_ID -and $env:SF_CLIENT_SECRET -and $env:SF_REFRESH_TOKEN
$haveSfdx = [bool]$env:SF_SFDX_AUTH_URL
$haveJwt = [bool]$env:SF_JWT_KEY_FILE
if (-not ($haveEnv -or $haveSfdx -or $haveJwt)) {
    Write-Error "no credentials in env. Set SF_CLIENT_ID/SF_CLIENT_SECRET/SF_REFRESH_TOKEN, or SF_SFDX_AUTH_URL, or SF_JWT_KEY_FILE."
}

Write-Host "Running v0.9.1 light smoke (output is org config metadata + counts)..." -ForegroundColor Cyan
Get-Content -Raw $sql | & $duck -batch
$code = $LASTEXITCODE
Write-Host ""
if ($code -eq 0) {
    Write-Host "smoke exit code: 0 (review output above; nothing persisted)" -ForegroundColor Green
} else {
    Write-Host "smoke exit code: $code (FAILED)" -ForegroundColor Red
}
exit $code
