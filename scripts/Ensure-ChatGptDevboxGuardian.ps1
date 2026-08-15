[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [int]$HeartbeatMaxAgeSeconds = 60
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}
$ProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot)

function Get-EnsureMutexName {
    param([Parameter(Mandatory = $true)][string]$Root)
    $bytes = [Text.Encoding]::UTF8.GetBytes($Root.TrimEnd('\').ToLowerInvariant())
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = $sha.ComputeHash($bytes) } finally { $sha.Dispose() }
    $suffix = ([BitConverter]::ToString($hash).Replace('-', '').Substring(0, 16))
    return "Global\ChatGptDevboxGuardianEnsure-$suffix"
}
$ensureMutex = New-Object System.Threading.Mutex($false, (Get-EnsureMutexName -Root $ProjectRoot))
try {
    $ensureMutexHeld = $ensureMutex.WaitOne(0, $false)
} catch [System.Threading.AbandonedMutexException] {
    $ensureMutexHeld = $true
}
if (-not $ensureMutexHeld) { exit 0 }

$powerShellResolver = Join-Path $PSScriptRoot 'Resolve-DevboxPowerShell.ps1'
. $powerShellResolver
$powerShellExe = Resolve-DevboxPowerShellExecutable
function Resolve-DevboxNodeExecutable {
    $configured = [string]$env:NODE_EXE
    if (-not [string]::IsNullOrWhiteSpace($configured) -and (Test-Path -LiteralPath $configured -PathType Leaf)) { return $configured }
    $programFiles = if ([string]::IsNullOrWhiteSpace($env:ProgramFiles)) { 'C:\Program Files' } else { $env:ProgramFiles }
    $installed = Join-Path $programFiles 'nodejs\node.exe'
    if (Test-Path -LiteralPath $installed -PathType Leaf) { return $installed }
    $resolved = Get-Command node -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($resolved) { return [string]$resolved.Source }
    throw 'Node.js is required to run Guardian v2.'
}
$nodeExe = Resolve-DevboxNodeExecutable
$guardianScript = Join-Path $ProjectRoot 'scripts\Watch-ChatGptDevboxGuardian.ps1'
$supervisorScript = Join-Path $ProjectRoot 'scripts\devbox-guardian.mjs'
$guardianDir = Join-Path $ProjectRoot 'run\guardian'
$guardianPidPath = Join-Path $guardianDir 'guardian.pid'
$heartbeatPath = Join-Path $guardianDir 'heartbeat.json'
$ensureLogPath = Join-Path $guardianDir 'ensure.log'
$staleObservationPath = Join-Path $guardianDir 'ensure-stale-observation.json'
$ensureLogMaxBytes = 1MB
$ensureLogRotations = 3
$guardianArtifactRetentionDays = 7
$guardianSourcePaths = @(
    $guardianScript,
    $powerShellResolver,
    (Join-Path $ProjectRoot 'scripts\devbox-guardian.mjs'),
    (Join-Path $ProjectRoot 'src\guardian-core.js')
)

if (-not (Test-Path $guardianDir)) {
    New-Item -ItemType Directory -Path $guardianDir | Out-Null
}

function Rotate-EnsureLogIfNeeded {
    if (-not (Test-Path -LiteralPath $ensureLogPath)) { return }
    $length = (Get-Item -LiteralPath $ensureLogPath -ErrorAction SilentlyContinue).Length
    if ($length -lt $ensureLogMaxBytes) { return }
    $oldest = "$ensureLogPath.$ensureLogRotations"
    Remove-Item -LiteralPath $oldest -Force -ErrorAction SilentlyContinue
    for ($index = $ensureLogRotations - 1; $index -ge 1; $index--) {
        $from = "$ensureLogPath.$index"
        $to = "$ensureLogPath.$($index + 1)"
        if (Test-Path -LiteralPath $from) { Move-Item -LiteralPath $from -Destination $to -Force -ErrorAction SilentlyContinue }
    }
    Move-Item -LiteralPath $ensureLogPath -Destination "$ensureLogPath.1" -Force -ErrorAction SilentlyContinue
}

function Remove-StaleGuardianArtifacts {
    $cutoff = [DateTime]::UtcNow.AddDays(-$guardianArtifactRetentionDays)
    Get-ChildItem -LiteralPath $guardianDir -File -ErrorAction SilentlyContinue |
        Where-Object {
            ($_.Name -like '*.tmp' -or $_.Name -like '*.bak') -and
            $_.LastWriteTimeUtc -lt $cutoff
        } |
        ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }
}

function Write-EnsureLog {
    param(
        [Parameter(Mandatory = $true)][string]$Message,
        [string]$Level = 'INFO'
    )

    Rotate-EnsureLogIfNeeded
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
        $commandLine = [string]$process.CommandLine
        $escapedSupervisorPath = [regex]::Escape($supervisorScript)
        if ($process -and ($commandLine -match 'Watch-ChatGptDevboxGuardian\.ps1' -or $commandLine -match $escapedSupervisorPath)) {
            return $process
        }
    }

    return $null
}

function Get-LiveGuardianSupervisorProcess {
    if (-not (Test-Path $heartbeatPath)) {
        return $null
    }
    try {
        $heartbeat = Get-Content -Path $heartbeatPath -Raw | ConvertFrom-Json
        if (-not $heartbeat.PSObject.Properties['SupervisorPid'] -or ([string]$heartbeat.SupervisorPid) -notmatch '^\d+$') {
            return $null
        }
        $supervisor = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f [int]$heartbeat.SupervisorPid) -ErrorAction SilentlyContinue
        $escapedSupervisorPath = [regex]::Escape($supervisorScript)
        if ($supervisor -and ([string]$supervisor.CommandLine) -match $escapedSupervisorPath -and ([string]$supervisor.CommandLine) -notmatch 'codex\.js|@openai/codex') {
            return $supervisor
        }
    } catch {
    }
    return $null
}

function Test-GuardianSourceFresh {
    param(
        [Parameter(Mandatory = $true)]$Process
    )

    try {
        $startedAt = ([DateTime]$Process.CreationDate).ToUniversalTime()
        $newestSource = $guardianSourcePaths |
            Where-Object { Test-Path -LiteralPath $_ } |
            ForEach-Object { (Get-Item -LiteralPath $_ -ErrorAction Stop).LastWriteTimeUtc } |
            Sort-Object -Descending |
            Select-Object -First 1

        if (-not $newestSource) {
            return $true
        }

        # A small tolerance avoids restart loops on filesystems with coarse timestamps.
        return ($newestSource -le $startedAt.AddSeconds(2))
    }
    catch {
        # Do not kill a healthy guardian solely because metadata inspection failed.
        return $true
    }
}


function Clear-StaleHeartbeatObservation {
    Remove-Item -LiteralPath $staleObservationPath -Force -ErrorAction SilentlyContinue
}

function Test-SecondStaleHeartbeatObservation {
    param(
        [Parameter(Mandatory = $true)]$Process
    )

    $now = [DateTime]::UtcNow
    try {
        if (Test-Path -LiteralPath $staleObservationPath) {
            $previous = Get-Content -LiteralPath $staleObservationPath -Raw -ErrorAction Stop | ConvertFrom-Json
            $samePid = ([int]$previous.ProcessId -eq [int]$Process.ProcessId)
            $observed = if ($previous.ObservedAtUtc -is [DateTime]) {
                $previous.ObservedAtUtc.ToUniversalTime()
            } else {
                [DateTime]::Parse(
                    [string]$previous.ObservedAtUtc,
                    [Globalization.CultureInfo]::InvariantCulture,
                    [Globalization.DateTimeStyles]::RoundtripKind
                ).ToUniversalTime()
            }
            if ($samePid -and (($now - $observed).TotalSeconds -le 180)) {
                return $true
            }
        }
    } catch {
    }

    @{ ProcessId = [int]$Process.ProcessId; ObservedAtUtc = $now.ToString('o') } |
        ConvertTo-Json -Compress |
        Set-Content -LiteralPath $staleObservationPath -Encoding UTF8
    return $false
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

        $observedValue = $heartbeat.ObservedAtUtc
        if ($observedValue -is [DateTime]) {
            $observedAt = $observedValue.ToUniversalTime()
        }
        else {
            $observedAt = [DateTime]::Parse(
                [string]$observedValue,
                [Globalization.CultureInfo]::InvariantCulture,
                [Globalization.DateTimeStyles]::RoundtripKind
            ).ToUniversalTime()
        }
        return (([DateTime]::UtcNow - $observedAt).TotalSeconds -lt $HeartbeatMaxAgeSeconds)
    }
    catch {
        return $false
    }
}

Write-EnsureLog -Message 'ensure invocation start'
Remove-StaleGuardianArtifacts

$existing = Get-LiveGuardianProcess
$heartbeatFresh = Test-GuardianHeartbeatFresh
$sourceFresh = if ($existing) { Test-GuardianSourceFresh -Process $existing } else { $false }
if ($existing -and $heartbeatFresh -and $sourceFresh) {
    Clear-StaleHeartbeatObservation
    exit 0
}

if ($existing -and -not $heartbeatFresh) {
    if (-not (Test-SecondStaleHeartbeatObservation -Process $existing)) {
        Write-EnsureLog -Level 'WARN' -Message ("heartbeat is stale for pid={0}; waiting for a second stale observation before restart" -f $existing.ProcessId)
        exit 0
    }
}

if ($existing) {
    $restartReason = if (-not $heartbeatFresh) { 'heartbeat is stale on two consecutive observations' } elseif (-not $sourceFresh) { 'guardian source is newer than the running process' } else { 'guardian requires restart' }
    Clear-StaleHeartbeatObservation
    Write-EnsureLog -Level 'WARN' -Message ("{0}; restarting pid={1}" -f $restartReason, $existing.ProcessId)
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

if (-not $existing) {
    Clear-StaleHeartbeatObservation
    $orphanSupervisor = Get-LiveGuardianSupervisorProcess
    if ($orphanSupervisor) {
        Write-EnsureLog -Level 'WARN' -Message ("guardian watcher is missing; stopping orphan supervisor pid={0}" -f $orphanSupervisor.ProcessId)
        Stop-Process -Id ([int]$orphanSupervisor.ProcessId) -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 1
    }
}

if (-not (Test-Path $guardianScript)) {
    Write-EnsureLog -Level 'ERROR' -Message ("guardian script missing: {0}" -f $guardianScript)
    exit 1
}

$arguments = @(
    $supervisorScript,
    '--project-root', $ProjectRoot,
    '--direct-owner'
)

Write-EnsureLog -Message 'guardian not running; starting detached Node guardian directly'
$launched = Start-Process -FilePath $nodeExe -ArgumentList $arguments -WorkingDirectory $ProjectRoot -WindowStyle Hidden -PassThru
$started = $null
$startDeadline = [DateTime]::UtcNow.AddSeconds(15)
do {
    Start-Sleep -Milliseconds 250
    $started = Get-LiveGuardianProcess
    if (-not $started -and $launched.HasExited) {
        Write-EnsureLog -Level 'ERROR' -Message ("guardian process exited during startup pid={0} exitCode={1}" -f $launched.Id, $launched.ExitCode)
        exit 1
    }
} while (-not $started -and [DateTime]::UtcNow -lt $startDeadline)
$escapedSupervisorPath = [regex]::Escape($supervisorScript)
if (-not $started -or ([string]$started.CommandLine) -notmatch $escapedSupervisorPath) {
    Write-EnsureLog -Level 'ERROR' -Message ("guardian failed to start persistently launchedPid={0} launchedAlive={1}" -f $launched.Id, (-not $launched.HasExited))
    exit 1
}

Write-EnsureLog -Message ("guardian running pid={0}" -f $started.ProcessId)
exit 0
