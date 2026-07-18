[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [string]$TaskPrefix = 'ChatGptDevboxGuardian'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}
$ProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot)

$runDir = Join-Path $ProjectRoot 'run'
$guardianDir = Join-Path $runDir 'guardian'
$guardianPidPath = Join-Path $guardianDir 'guardian.pid'
$heartbeatPath = Join-Path $guardianDir 'heartbeat.json'
$statePath = Join-Path $guardianDir 'state.json'
$guardianLogPath = Join-Path $guardianDir 'guardian.log'
$ensureLogPath = Join-Path $guardianDir 'ensure.log'
$settingsPath = Join-Path $runDir 'guardian.settings.json'
$desiredStatePath = Join-Path $runDir 'guardian.desired-state.json'
$repairDir = Join-Path $guardianDir 'repairs'
$repairPolicyPath = Join-Path $guardianDir 'repair-policy.json'
$logonTaskName = "$TaskPrefix-Logon"
$keepAliveTaskName = "$TaskPrefix-KeepAlive"

Write-Host 'Scheduled tasks'
Get-ScheduledTask -TaskName $logonTaskName, $keepAliveTaskName -ErrorAction SilentlyContinue |
    Select-Object TaskName, State, Author |
    Format-Table -AutoSize

$taskInfo = @()
if (Get-ScheduledTask -TaskName $logonTaskName -ErrorAction SilentlyContinue) {
    $taskInfo += Get-ScheduledTaskInfo -TaskName $logonTaskName | Select-Object @{Name = 'TaskName'; Expression = { $logonTaskName } }, LastRunTime, NextRunTime, LastTaskResult, NumberOfMissedRuns
}
if (Get-ScheduledTask -TaskName $keepAliveTaskName -ErrorAction SilentlyContinue) {
    $taskInfo += Get-ScheduledTaskInfo -TaskName $keepAliveTaskName | Select-Object @{Name = 'TaskName'; Expression = { $keepAliveTaskName } }, LastRunTime, NextRunTime, LastTaskResult, NumberOfMissedRuns
}

$taskInfo |
    Select-Object TaskName, LastRunTime, NextRunTime, LastTaskResult, NumberOfMissedRuns |
    Format-Table -AutoSize

Write-Host ''
Write-Host 'Guardian PID'
if (Test-Path $guardianPidPath) {
    $guardianPid = Get-Content -Path $guardianPidPath | Select-Object -First 1
    Write-Host $guardianPid
    Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f [int]$guardianPid) -ErrorAction SilentlyContinue |
        Select-Object Name, ProcessId, ParentProcessId, CreationDate, CommandLine |
        Format-List
} else {
    Write-Host 'No guardian PID file present.'
}

Write-Host ''
Write-Host 'Desired state'
if (Test-Path $desiredStatePath) {
    Get-Content -Path $desiredStatePath
} else {
    Write-Host 'No desired-state file present.'
}

Write-Host ''
Write-Host 'Guardian settings'
if (Test-Path $settingsPath) {
    Get-Content -Path $settingsPath
} else {
    Write-Host 'No guardian settings file present.'
}

Write-Host ''
Write-Host 'Heartbeat'
if (Test-Path $heartbeatPath) {
    Get-Content -Path $heartbeatPath
} else {
    Write-Host 'No heartbeat file present.'
}

Write-Host ''
Write-Host 'State'
if (Test-Path $statePath) {
    $state = Get-Content -Path $statePath -Raw | ConvertFrom-Json
    if ($state.PSObject.Properties['Readiness']) {
        Write-Host ("Overall: {0}" -f $state.Readiness.Overall)
        foreach ($summary in @($state.Readiness.Summary)) {
            Write-Host ("- {0}" -f $summary)
        }
        Write-Host ("Runtime: requested={0}, selected={1}, DockerProbePerformed={2}" -f $state.RuntimeMode, $state.SelectedRuntime, $state.DockerProbePerformed)
    }
    $state | ConvertTo-Json -Depth 10
} else {
    Write-Host 'No state file present.'
}

Write-Host ''
Write-Host 'Repair policy'
if (Test-Path $repairPolicyPath) {
    Get-Content -Path $repairPolicyPath
} else {
    Write-Host 'No persistent repair policy present.'
}

Write-Host ''
Write-Host 'Recent guardian log'
if (Test-Path $guardianLogPath) {
    Get-Content -Path $guardianLogPath -Tail 40
} else {
    Write-Host 'No guardian log present.'
}

Write-Host ''
Write-Host 'Recent ensure log'
if (Test-Path $ensureLogPath) {
    Get-Content -Path $ensureLogPath -Tail 40
} else {
    Write-Host 'No ensure log present.'
}

Write-Host ''
Write-Host 'Recent repair artifacts'
Get-ChildItem -Path $repairDir -File -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 10 Name, LastWriteTime, Length |
    Format-Table -AutoSize
