param()

$ErrorActionPreference = "Stop"

function Get-EnvValue {
    param(
        [string]$FilePath,
        [string]$Name
    )

    $match = Select-String -Path $FilePath -Pattern ("^{0}=(.*)$" -f [regex]::Escape($Name)) -ErrorAction SilentlyContinue
    if (-not $match) {
        return ""
    }

    return $match.Matches[0].Groups[1].Value.Trim()
}

$root = Split-Path -Parent $PSScriptRoot
$envFile = Join-Path $root ".env"
$publicHostname = Get-EnvValue -FilePath $envFile -Name "CLOUDFLARED_PUBLIC_HOSTNAME"
$publicBaseUrl = if ($publicHostname) {
    if ($publicHostname -match '^https?://') { $publicHostname.TrimEnd('/') } else { "https://$($publicHostname.TrimEnd('/'))" }
} else {
    Get-EnvValue -FilePath $envFile -Name "PUBLIC_BASE_URL"
}

if (-not $publicBaseUrl) {
    throw "No public hostname is configured in .env."
}

Write-Host "Cloudflare Access protect URL: $publicBaseUrl/authorize*"
Write-Host "Recommended cookie path: /authorize"
Write-Host "Set .env values after creating the Access app:"
Write-Host "MCP_AUTH_MODE=cloudflare-access"
Write-Host "CLOUDFLARE_ACCESS_TEAM_DOMAIN=https://<your-team>.cloudflareaccess.com"
Write-Host "CLOUDFLARE_ACCESS_AUD=<Access application AUD>"
Write-Host "CLOUDFLARE_ACCESS_JWKS_URL=https://<your-team>.cloudflareaccess.com/cdn-cgi/access/certs"
