# package_dist_windows.ps1 — package the Windows x64 Salesforce extension.

param(
    [string]$BuildDir = "build\release",
    [string]$OutDir = "dist",
    [string]$Version = ""
)

$ErrorActionPreference = 'Continue'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
Set-Location $root

if (-not $Version -and $env:GITHUB_REF_NAME -and $env:GITHUB_REF_NAME.StartsWith('v')) {
    $Version = $env:GITHUB_REF_NAME.Substring(1)
}
if (-not $Version) {
    $line = Select-String -Path 'docs/community/description.yml' -Pattern '^\s+version:' | Select-Object -First 1
    if ($line) { $Version = (($line.Line -split ':', 2)[1]).Trim() }
}
if (-not $Version) { $Version = 'unknown' }

$ext = Join-Path $root "$BuildDir\extension\salesforce\salesforce.duckdb_extension"
if (-not (Test-Path $ext)) {
    throw "extension not found at $ext"
}

$stage = Join-Path $root "$OutDir\duckdb-salesforce-$Version-windows-x64"
$zip = "$stage.zip"
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

Copy-Item -Path $ext -Destination (Join-Path $stage 'salesforce.duckdb_extension') -Force

$readme = Get-Content 'scripts\dist_README.template.txt' -Raw
$readme = $readme -replace '@@VERSION@@', $Version
$readme = $readme -replace '@@PLATFORM@@', 'Windows x64'
Set-Content -Path (Join-Path $stage 'README.txt') -Value $readme -Encoding utf8

Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force

Write-Host ''
Write-Host '--- packaged ---'
Write-Host "Stage dir: $stage"
Write-Host "ZIP:       $zip"
Get-ChildItem $stage | Select-Object Name, Length
Write-Host ''
Write-Host 'SHA-256:'
Write-Host ((Get-FileHash $zip -Algorithm SHA256).Hash)

exit 0
