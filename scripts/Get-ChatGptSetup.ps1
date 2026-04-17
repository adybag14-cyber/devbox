param()

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$envFile = Join-Path $root ".env"
$portLine = Select-String -Path $envFile -Pattern '^PORT=(.*)$' | Select-Object -First 1
$port = if ($portLine) { $portLine.Matches[0].Groups[1].Value.Trim() } else { "8100" }
$response = Invoke-RestMethod -Uri "http://127.0.0.1:$port/" -TimeoutSec 5
$mcpServerUrl = if ($response.mcp_url) { $response.mcp_url } else { "http://127.0.0.1:$port" }
$authenticationLabel = if ($response.auth_mode -eq 'none') { 'No Authentication' } else { 'OAuth' }

Write-Host "Name: Docker Devbox"
Write-Host "Description: Reproducible Docker devbox shell plus optional Windows host tools"
Write-Host "MCP Server URL: $mcpServerUrl"
Write-Host "Authentication: $authenticationLabel"
if ($response.oauth) {
    Write-Host "OAuth issuer: $($response.oauth.issuer)"
    Write-Host "Protected resource metadata: $($response.oauth.resourceMetadataUrl)"
}
