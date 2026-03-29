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

function Enter-ChatGptDevboxLifecycleMutex {
    $script:lifecycleMutex = New-Object System.Threading.Mutex($false, 'Global\ChatGptDevboxMcpLifecycle')
    $script:lifecycleMutexHeld = $script:lifecycleMutex.WaitOne(300000, $false)
    if (-not $script:lifecycleMutexHeld) {
        throw "Timed out waiting for another ChatGPT Devbox lifecycle action to finish."
    }
}

function Exit-ChatGptDevboxLifecycleMutex {
    if ($script:lifecycleMutexHeld) {
        $script:lifecycleMutex.ReleaseMutex()
        $script:lifecycleMutexHeld = $false
    }

    if ($script:lifecycleMutex) {
        $script:lifecycleMutex.Dispose()
        $script:lifecycleMutex = $null
    }
}

function Ensure-Directory {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
}

function Write-JsonStateFile {
    param(
        [string]$Path,
        [object]$Value
    )

    $directory = Split-Path -Parent $Path
    if ($directory) {
        Ensure-Directory -Path $directory
    }

    $Value | ConvertTo-Json -Depth 8 | Set-Content -Path $Path -Encoding UTF8
}

function Write-GuardianDesiredState {
    param(
        [string]$RunDir,
        [bool]$ShouldRun,
        [string]$Source
    )

    $statePath = Join-Path $RunDir "guardian.desired-state.json"
    Write-JsonStateFile -Path $statePath -Value @{
        ShouldRun = $ShouldRun
        UpdatedAtUtc = [DateTime]::UtcNow.ToString('o')
        Source = $Source
    }
}

Enter-ChatGptDevboxLifecycleMutex
try {
$root = Split-Path -Parent $PSScriptRoot
$envFile = Join-Path $root ".env"
$runDir = Join-Path $root "run"
$pidFile = Join-Path $runDir "mcp.pid"
Write-GuardianDesiredState -RunDir $runDir -ShouldRun $false -Source "Stop-ChatGptDevboxMcp.ps1"

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
} finally {
    Exit-ChatGptDevboxLifecycleMutex
}
