param(
    [switch]$Tunnel,
    [switch]$All
)

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

function Get-CommandLineForPid {
    param([int]$ProcessId)

    $proc = Get-CimInstance Win32_Process -Filter "ProcessId=$ProcessId" -ErrorAction SilentlyContinue
    if (-not $proc) {
        return $null
    }

    return $proc.CommandLine
}

$root = Split-Path -Parent $PSScriptRoot
$envFile = Join-Path $root ".env"
$runDir = Join-Path $root "run"
$pidFile = Join-Path $runDir "mcp.pid"

if (Test-Path $pidFile) {
    $ownedPid = [int](Get-Content $pidFile | Select-Object -First 1)
    $commandLine = Get-CommandLineForPid -ProcessId $ownedPid
    if ($commandLine -and $commandLine -like "*docker-chatgpt-devbox*" -and $commandLine -like "*src\\server.js*") {
        Stop-Process -Id $ownedPid -Force
        Start-Sleep -Seconds 1
    }

    Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
}

if ((Test-Path $envFile) -and ($Tunnel -or $All)) {
    $cloudflaredContainerName = Get-EnvValue -FilePath $envFile -Name "CLOUDFLARED_CONTAINER_NAME"
    docker inspect --type container $cloudflaredContainerName *> $null
    if ($LASTEXITCODE -eq 0) {
        docker rm -f $cloudflaredContainerName *> $null
    }
}

if ((Test-Path $envFile) -and $All) {
    $containerName = Get-EnvValue -FilePath $envFile -Name "DEVBOX_CONTAINER_NAME"
    docker inspect --type container $containerName *> $null
    if ($LASTEXITCODE -eq 0) {
        docker stop $containerName *> $null
    }
}
