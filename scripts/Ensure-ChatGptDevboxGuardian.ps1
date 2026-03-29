[CmdletBinding()]
param(
    [string]$ProjectRoot = 'C:\Users\adyba\docker-chatgpt-devbox',
    [int]$HeartbeatMaxAgeSeconds = 20
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$powerShellExe = 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'
$guardianScript = Join-Path $ProjectRoot 'scripts\Watch-ChatGptDevboxGuardian.ps1'
$guardianDir = Join-Path $ProjectRoot 'run\guardian'
$guardianPidPath = Join-Path $guardianDir 'guardian.pid'
$heartbeatPath = Join-Path $guardianDir 'heartbeat.json'
$ensureLogPath = Join-Path $guardianDir 'ensure.log'

if (-not (Test-Path $guardianDir)) {
    New-Item -ItemType Directory -Path $guardianDir | Out-Null
}

function Write-EnsureLog {
    param(
        [Parameter(Mandatory = $true)][string]$Message,
        [string]$Level = 'INFO'
    )

    $line = '{0} [{1}] {2}' -f ([DateTime]::UtcNow.ToString('o')), $Level.ToUpperInvariant(), $Message
    Add-Content -Path $ensureLogPath -Value $line -Encoding UTF8
}

function Get-LiveGuardianProcess {
    $candidatePid = $null
    if (Test-Path $guardianPidPath) {
        try {
            $candidatePid = [int](Get-Content -Path $guardianPidPath -ErrorAction Stop | Select-Object -First 1)
        }
        catch {
            $candidatePid = $null
        }
    }

    if ($candidatePid) {
        $process = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f $candidatePid) -ErrorAction SilentlyContinue
        if ($process -and ([string]$process.CommandLine) -match 'Watch-ChatGptDevboxGuardian\.ps1') {
            return $process
        }
    }

    $escapedScriptPath = [regex]::Escape($guardianScript)
    return Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object { ([string]$_.CommandLine) -match $escapedScriptPath } |
        Select-Object -First 1
}

function Test-GuardianHeartbeatFresh {
    if (-not (Test-Path $heartbeatPath)) {
        return $false
    }

    try {
        $heartbeat = Get-Content -Path $heartbeatPath -Raw | ConvertFrom-Json
        if (-not $heartbeat.ObservedAtUtc) {
            return $false
        }

        $observedAt = [DateTime]::Parse([string]$heartbeat.ObservedAtUtc).ToUniversalTime()
        return (([DateTime]::UtcNow - $observedAt).TotalSeconds -lt $HeartbeatMaxAgeSeconds)
    }
    catch {
        return $false
    }
}

$existing = Get-LiveGuardianProcess
if ($existing -and (Test-GuardianHeartbeatFresh)) {
    exit 0
}

if ($existing) {
    Write-EnsureLog -Level 'WARN' -Message ("guardian heartbeat is stale; restarting pid={0}" -f $existing.ProcessId)
    Stop-Process -Id ([int]$existing.ProcessId) -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}

if (-not (Test-Path $guardianScript)) {
    Write-EnsureLog -Level 'ERROR' -Message ("guardian script missing: {0}" -f $guardianScript)
    exit 1
}

$arguments = @(
    '-NoProfile'
    '-NonInteractive'
    '-ExecutionPolicy'
    'Bypass'
    '-WindowStyle'
    'Hidden'
    '-File'
    $guardianScript
)

Write-EnsureLog -Message 'guardian not running; starting detached guardian process'
Start-Process -FilePath $powerShellExe -ArgumentList $arguments -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds 4

$started = Get-LiveGuardianProcess
if (-not $started) {
    Write-EnsureLog -Level 'ERROR' -Message 'guardian failed to start'
    exit 1
}

Write-EnsureLog -Message ("guardian running pid={0}" -f $started.ProcessId)
exit 0
