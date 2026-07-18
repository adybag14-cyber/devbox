[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [int]$HeartbeatMaxAgeSeconds = 20
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}
$ProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot)

$powerShellExe = 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'
$guardianScript = Join-Path $ProjectRoot 'scripts\Watch-ChatGptDevboxGuardian.ps1'
$guardianDir = Join-Path $ProjectRoot 'run\guardian'
$guardianPidPath = Join-Path $guardianDir 'guardian.pid'
$heartbeatPath = Join-Path $guardianDir 'heartbeat.json'
$ensureLogPath = Join-Path $guardianDir 'ensure.log'
$hiddenLauncher = Join-Path $PSScriptRoot 'Run-ChatGptDevboxGuardian.vbs'

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

    if (Test-Path $heartbeatPath) {
        try {
            $heartbeat = Get-Content -Path $heartbeatPath -Raw | ConvertFrom-Json
            if ($heartbeat.PSObject.Properties['SupervisorPid'] -and ([string]$heartbeat.SupervisorPid) -match '^\d+$') {
                $supervisor = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f [int]$heartbeat.SupervisorPid) -ErrorAction SilentlyContinue
                if ($supervisor -and ([string]$supervisor.CommandLine) -match 'devbox-guardian\.mjs') {
                    return $supervisor
                }
            }
        } catch {
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
        if (-not $heartbeat.PSObject.Properties['GuardianVersion'] -or [int]$heartbeat.GuardianVersion -lt 2) {
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
    $supervisorPid = $null
    try {
        $heartbeat = Get-Content -Path $heartbeatPath -Raw -ErrorAction Stop | ConvertFrom-Json
        if ($heartbeat.PSObject.Properties['SupervisorPid'] -and ([string]$heartbeat.SupervisorPid) -match '^\d+$') {
            $supervisorPid = [int]$heartbeat.SupervisorPid
        }
    } catch {
        $supervisorPid = $null
    }
    Stop-Process -Id ([int]$existing.ProcessId) -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    if ($supervisorPid) {
        $supervisor = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f $supervisorPid) -ErrorAction SilentlyContinue
        if ($supervisor -and ([string]$supervisor.CommandLine) -match 'devbox-guardian\.mjs' -and ([string]$supervisor.CommandLine) -notmatch 'codex\.js|@openai/codex') {
            Write-EnsureLog -Level 'WARN' -Message ("stopping stale guardian supervisor pid={0}" -f $supervisorPid)
            Stop-Process -Id $supervisorPid -Force -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 1
        }
    }
}

if (-not (Test-Path $guardianScript)) {
    Write-EnsureLog -Level 'ERROR' -Message ("guardian script missing: {0}" -f $guardianScript)
    exit 1
}

if (-not (Test-Path $hiddenLauncher)) {
    Write-EnsureLog -Level 'ERROR' -Message ("hidden launcher missing: {0}" -f $hiddenLauncher)
    exit 1
}

$wscriptExe = Join-Path $env:WINDIR 'System32\wscript.exe'
$arguments = @('//B', '//NoLogo', $hiddenLauncher)

Write-EnsureLog -Message 'guardian not running; starting detached guardian process'
Start-Process -FilePath $wscriptExe -ArgumentList $arguments -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds 4

$started = Get-LiveGuardianProcess
if (-not $started) {
    Write-EnsureLog -Level 'ERROR' -Message 'guardian failed to start'
    exit 1
}

Write-EnsureLog -Message ("guardian running pid={0}" -f $started.ProcessId)
exit 0
