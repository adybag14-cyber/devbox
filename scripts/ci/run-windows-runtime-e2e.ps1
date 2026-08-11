[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$taskPrefix = 'ChatGptDevboxCi-{0}' -f ([Guid]::NewGuid().ToString('N').Substring(0, 12))
$envPath = Join-Path $root '.env'
$runtimeEnv = Join-Path $root '.env.runtime'
$hadEnv = Test-Path $envPath
$envBackup = if ($hadEnv) { Get-Content $envPath -Raw } else { $null }
$port = 18184
$isolatedEnvNames = @(
    'PUBLIC_BASE_URL',
    'CLOUDFLARED_PUBLIC_HOSTNAME',
    'CLOUDFLARED_TUNNEL_TOKEN',
    'CLOUDFLARED_TUNNEL_TOKEN_FILE',
    'CLOUDFLARED_TUNNEL_ID',
    'CLOUDFLARED_TUNNEL_NAME'
)
$previousProcessEnvironment = @{}
foreach ($name in $isolatedEnvNames) {
    $previousProcessEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    [Environment]::SetEnvironmentVariable($name, $null, 'Process')
}

function Remove-CiTasks {
    foreach ($name in @("$taskPrefix-Logon", "$taskPrefix-KeepAlive", "$taskPrefix-McpElevatedStart")) {
        if (Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue) {
            Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
        }
    }
}

try {
    $config = @("PORT=$port", 'HOST=127.0.0.1', 'DEVBOX_RUNTIME_MODE=host', 'DEVBOX_MCP_IMPLEMENTATION=rust', 'MCP_AUTH_MODE=none', 'PUBLIC_BASE_URL=', 'ENABLE_HOST_EXEC=true', 'HOST_PROGRAM_ALLOWLIST=powershell,pwsh,cmd,git,gh,node,npm,npx,python,py,pip,rg,curl', 'DEVBOX_PROGRAM_ALLOWLIST=powershell,pwsh,cmd,git,gh,node,npm,npx,python,py,pip,rg,curl') -join "`n"
    [IO.File]::WriteAllText($envPath, "$config`n", [Text.UTF8Encoding]::new($false))
    $env:DEVBOX_MCP_IMPLEMENTATION = 'rust'
    $env:DEVBOX_RUNTIME_MODE = 'host'
    $env:PORT = [string]$port
    $env:HOST = '127.0.0.1'
    $env:MCP_AUTH_MODE = 'none'
    $env:PUBLIC_BASE_URL = ''

    & (Join-Path $root 'scripts\Start-ChatGptDevboxMcp.ps1') -Runtime host
    $health = Invoke-WebRequest -Uri "http://127.0.0.1:$port/healthz" -UseBasicParsing -TimeoutSec 5
    if ($health.StatusCode -ne 200 -or $health.Content -notmatch 'ok') { throw 'Rust MCP health gate failed.' }

    & (Join-Path $root 'scripts\Install-ChatGptDevboxGuardian.ps1') -Runtime host -TaskPrefix $taskPrefix | Out-Host
    $guardianTask = Get-ScheduledTask -TaskName "$taskPrefix-Logon"
    $triggerTypes = @($guardianTask.Triggers | ForEach-Object { $_.CimClass.CimClassName })
    if ($triggerTypes -notcontains 'MSFT_TaskBootTrigger') { throw 'Guardian CI task is missing AtStartup trigger.' }
    if ($triggerTypes -notcontains 'MSFT_TaskLogonTrigger') { throw 'Guardian CI task is missing AtLogon trigger.' }
    if ([string]$guardianTask.Principal.RunLevel -ne 'Highest') { throw 'Guardian CI task is not Highest run level.' }
    if ($guardianTask.Settings.RestartCount -lt 3) { throw 'Guardian CI task restart policy is missing.' }

    & node (Join-Path $root 'scripts\devbox-guardian.mjs') --project-root $root --once --no-repair | Out-Host
    $state = Get-Content (Join-Path $root 'run\guardian\state.json') -Raw | ConvertFrom-Json
    if (-not $state.IsHealthy) { throw "Guardian did not classify native Rust runtime healthy: $($state.Reasons -join '; ')" }

    $metadata = Invoke-RestMethod -Uri "http://127.0.0.1:$port/" -TimeoutSec 5
    if (-not $metadata.build.gitSha -or -not $metadata.build.binarySha256) { throw 'Build provenance was not exposed.' }
    Write-Host "Windows native lifecycle E2E passed: PID=$($state.McpProcessId) git=$($metadata.build.gitSha)"
} finally {
    Remove-CiTasks
    try { & (Join-Path $root 'scripts\Stop-ChatGptDevboxMcp.ps1') -ErrorAction SilentlyContinue | Out-Null } catch {}
    if ($hadEnv) {
        [IO.File]::WriteAllText($envPath, $envBackup, [Text.UTF8Encoding]::new($false))
    } else {
        Remove-Item $envPath -Force -ErrorAction SilentlyContinue
    }
    Remove-Item $runtimeEnv -Force -ErrorAction SilentlyContinue
    foreach ($name in $isolatedEnvNames) {
        [Environment]::SetEnvironmentVariable($name, $previousProcessEnvironment[$name], 'Process')
    }
}
