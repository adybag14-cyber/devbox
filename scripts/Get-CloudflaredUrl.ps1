param()

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$envFile = Join-Path $root ".env"
$publicBaseUrlMatch = Select-String -Path $envFile -Pattern '^CLOUDFLARED_PUBLIC_HOSTNAME=(.*)$' | Select-Object -First 1
$configuredPublicValue = if ($publicBaseUrlMatch) { $publicBaseUrlMatch.Matches[0].Groups[1].Value.Trim() } else { "" }

if ($configuredPublicValue) {
    if ($configuredPublicValue -match '^https?://') {
        Write-Host "$($configuredPublicValue.TrimEnd('/'))/mcp"
    } else {
        Write-Host "https://$($configuredPublicValue.TrimEnd('/'))/mcp"
    }
    exit 0
}

$match = Select-String -Path $envFile -Pattern '^CLOUDFLARED_CONTAINER_NAME=(.*)$' | Select-Object -First 1
$containerName = if ($match) { $match.Matches[0].Groups[1].Value.Trim() } else { "chatgpt-devbox-cloudflared" }

$logs = docker logs $containerName 2>&1
$urlMatch = [regex]::Match(($logs | Out-String), 'https://[a-z0-9-]+\.trycloudflare\.com')
if (-not $urlMatch.Success) {
    throw "Could not find a trycloudflare URL in $containerName logs."
}

Write-Host "$($urlMatch.Value.TrimEnd('/'))/mcp"
