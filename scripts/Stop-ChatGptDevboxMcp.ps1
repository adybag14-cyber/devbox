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
$settingsPath = Join-Path $runDir 'guardian.settings.json'
$settings = if (Test-Path $settingsPath) { Get-Content $settingsPath -Raw | ConvertFrom-Json } else { $null }
$selectedRuntime = if ($settings -and $settings.PSObject.Properties['SelectedRuntime'] -and ([string]$settings.SelectedRuntime).ToLowerInvariant() -in @('host', 'docker')) {
    ([string]$settings.SelectedRuntime).ToLowerInvariant()
} else {
    $configuredRuntime = if (Test-Path $envFile) { (Get-EnvValue -FilePath $envFile -Name 'DEVBOX_RUNTIME_MODE').ToLowerInvariant() } else { '' }
    if ($configuredRuntime -eq 'host') { 'host' } else { 'docker' }
}
Write-GuardianDesiredState -RunDir $runDir -ShouldRun $false -Source "Stop-ChatGptDevboxMcp.ps1"

if (Test-Path $pidFile) {
    $ownedPid = [int](Get-Content $pidFile | Select-Object -First 1)
    $commandLine = Get-CommandLineForPid -ProcessId $ownedPid
    if ($commandLine -and $commandLine -match 'src[\\/]server\.js\b' -and $commandLine -match '--env-file(?:=|\s+)[^"\s]*\.env\.runtime\b') {
        Stop-Process -Id $ownedPid -Force
        Start-Sleep -Seconds 1
    }

    Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
}

if ((Test-Path $envFile) -and ($Tunnel -or $All)) {
    $hostTunnelPidFile = Join-Path $runDir 'host-cloudflared.pid'
    if (Test-Path $hostTunnelPidFile) {
        $tunnelPidText = Get-Content $hostTunnelPidFile -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($tunnelPidText -match '^\d+$') {
            $tunnelProcess = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f [int]$tunnelPidText) -ErrorAction SilentlyContinue
            if ($tunnelProcess -and ([string]$tunnelProcess.CommandLine) -match 'cloudflared(?:\.exe)?.*host-cloudflared\.tunnel-token\.txt') {
                Stop-Process -Id ([int]$tunnelProcess.ProcessId) -Force
            }
        }
        Remove-Item $hostTunnelPidFile -Force -ErrorAction SilentlyContinue
    }
    if ($selectedRuntime -eq 'docker') {
        $cloudflaredContainerName = Get-EnvValue -FilePath $envFile -Name "CLOUDFLARED_CONTAINER_NAME"
        if (-not $cloudflaredContainerName) { $cloudflaredContainerName = 'chatgpt-devbox-cloudflared' }
        docker inspect --type container $cloudflaredContainerName *> $null
        if ($LASTEXITCODE -eq 0) {
            docker rm -f $cloudflaredContainerName *> $null
        }
    }
}

if ((Test-Path $envFile) -and $All -and $selectedRuntime -eq 'docker') {
    $containerName = Get-EnvValue -FilePath $envFile -Name "DEVBOX_CONTAINER_NAME"
    if (-not $containerName) { $containerName = 'chatgpt-devbox-runtime' }
    docker inspect --type container $containerName *> $null
    if ($LASTEXITCODE -eq 0) {
        docker stop $containerName *> $null
    }
}
} finally {
    Exit-ChatGptDevboxLifecycleMutex
}
