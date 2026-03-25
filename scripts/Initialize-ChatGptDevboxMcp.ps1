param()

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$envFile = Join-Path $root ".env"
$exampleFile = Join-Path $root ".env.example"

if (Test-Path $envFile) {
    Write-Host ".env already exists at $envFile"
    exit 0
}

Copy-Item -Path $exampleFile -Destination $envFile
Write-Host "Created $envFile from .env.example"
