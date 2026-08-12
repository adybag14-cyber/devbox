[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [int]$PollSeconds = 10,
    [int]$RepairCooldownSeconds = 120,
    [int]$FailureThreshold = 3,
    [int]$LiveMcpFailureThreshold = 6,
    [int]$RepairFailureBackoffSeconds = 300,
    [int]$DockerProbeTimeoutSeconds = 5,
    [int]$DockerBackoffBaseSeconds = 60,
    [int]$DockerBackoffMaxSeconds = 1800,
    [int]$DockerCircuitFailureThreshold = 3,
    [int]$DockerCircuitOpenSeconds = 3600,
    [switch]$Once,
    [switch]$NoRepair
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}
$ProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot)
$supervisorScript = Join-Path $ProjectRoot 'scripts\devbox-guardian.mjs'
$guardianDir = Join-Path $ProjectRoot 'run\guardian'
$guardianLogPath = Join-Path $guardianDir 'guardian.log'
function Get-GuardianMutexName {
    param([Parameter(Mandatory = $true)][string]$Root)
    $bytes = [Text.Encoding]::UTF8.GetBytes($Root.TrimEnd('\').ToLowerInvariant())
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = $sha.ComputeHash($bytes) } finally { $sha.Dispose() }
    $suffix = ([BitConverter]::ToString($hash).Replace('-', '').Substring(0, 16))
    return "Global\ChatGptDevboxGuardian-$suffix"
}
$mutexName = Get-GuardianMutexName -Root $ProjectRoot

if (-not (Test-Path $guardianDir)) {
    New-Item -ItemType Directory -Path $guardianDir | Out-Null
}

function Resolve-NodeExecutable {
    $configured = $env:NODE_EXE
    if (-not [string]::IsNullOrWhiteSpace($configured) -and (Test-Path $configured)) {
        return $configured
    }

    $installed = 'C:\Program Files\nodejs\node.exe'
    if (Test-Path $installed) {
        return $installed
    }

    $command = Get-Command node -ErrorAction SilentlyContinue
    if (-not $command) {
        throw 'Node.js is required to run Guardian v2.'
    }
    if ([string]$command.Source -like '*AppData\Roaming\npm\node*') {
        throw "Resolved node command is an npm shim: $($command.Source)"
    }
    return [string]$command.Source
}

$guardianMutex = New-Object System.Threading.Mutex($false, $mutexName)
$mutexHeld = $false
$previousWrapperPid = $env:DEVBOX_GUARDIAN_WRAPPER_PID

try {
    $mutexHeld = $guardianMutex.WaitOne(0, $false)
    if (-not $mutexHeld) {
        exit 0
    }
    if (-not (Test-Path $supervisorScript)) {
        throw "Guardian v2 supervisor is missing: $supervisorScript"
    }

    $arguments = @(
        $supervisorScript,
        '--project-root', $ProjectRoot,
        '--poll-seconds', [string]$PollSeconds,
        '--repair-cooldown-seconds', [string]$RepairCooldownSeconds,
        '--failure-threshold', [string]$FailureThreshold,
        '--live-mcp-failure-threshold', [string]$LiveMcpFailureThreshold,
        '--repair-failure-backoff-seconds', [string]$RepairFailureBackoffSeconds,
        '--docker-probe-timeout-seconds', [string]$DockerProbeTimeoutSeconds,
        '--docker-backoff-base-seconds', [string]$DockerBackoffBaseSeconds,
        '--docker-backoff-max-seconds', [string]$DockerBackoffMaxSeconds,
        '--docker-circuit-failure-threshold', [string]$DockerCircuitFailureThreshold,
        '--docker-circuit-open-seconds', [string]$DockerCircuitOpenSeconds
    )
    if ($Once) { $arguments += '--once' }
    if ($NoRepair) { $arguments += '--no-repair' }

    $env:DEVBOX_GUARDIAN_WRAPPER_PID = [string]$PID
    & (Resolve-NodeExecutable) @arguments
    exit $LASTEXITCODE
}
catch {
    Add-Content -Path $guardianLogPath -Value ('{0} [FATAL] {1}' -f ([DateTime]::UtcNow.ToString('o')), $_.Exception.ToString()) -Encoding UTF8
    throw
}
finally {
    $env:DEVBOX_GUARDIAN_WRAPPER_PID = $previousWrapperPid
    if ($mutexHeld) {
        $guardianMutex.ReleaseMutex()
    }
    $guardianMutex.Dispose()
}
