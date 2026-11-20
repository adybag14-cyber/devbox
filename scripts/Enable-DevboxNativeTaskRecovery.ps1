[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$TaskName,
    [string]$TaskPath = '\',
    [Parameter(Mandatory)][string]$ExpectedExecutable,
    [Parameter(Mandatory)][string]$ExpectedArguments,
    [Parameter(Mandatory)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$ExpectedSha256,
    [Parameter(Mandatory)][string]$EvidenceDirectory,
    [ValidateRange(5, 60)][int]$StartDelaySeconds = 60,
    [switch]$Apply,
    [switch]$EnableTaskHistory
)

$ErrorActionPreference = 'Stop'
$before = Export-ScheduledTask -TaskName $TaskName -TaskPath $TaskPath
$task = Get-ScheduledTask -TaskName $TaskName -TaskPath $TaskPath
$actions = @($task.Actions)
if ($actions.Count -ne 1 -or $actions[0].Execute -ne $ExpectedExecutable -or
    $actions[0].Arguments -cne $ExpectedArguments) {
    throw 'Task action identity changed; refusing to modify recovery.'
}
if ((Get-FileHash -LiteralPath $ExpectedExecutable -Algorithm SHA256).Hash -ne $ExpectedSha256) {
    throw 'Task executable digest changed; refusing to modify recovery.'
}
if ($task.Settings.MultipleInstances -ne 'IgnoreNew' -or
    $task.Settings.ExecutionTimeLimit -ne 'PT0S' -or
    $task.Settings.AllowHardTerminate -or $task.State -eq 'Disabled' -or -not $task.Settings.Enabled) {
    throw 'Recovery requires an enabled IgnoreNew task without hard termination or a runtime limit.'
}

$beforeInfo = $task | Get-ScheduledTaskInfo
$recoveryId = 'DevboxNativeRecovery'
$existing = @($task.Triggers | Where-Object Id -EQ $recoveryId)
if ($existing.Count -gt 1) { throw 'Duplicate native recovery triggers.' }
$changed = $existing.Count -eq 0
if ($existing.Count -eq 1) {
    $trigger = $existing[0]
    if ($trigger.CimClass.CimClassName -ne 'MSFT_TaskTimeTrigger' -or
        $trigger.Repetition.Interval -ne 'PT1M' -or $trigger.Repetition.Duration -or
        $trigger.Repetition.StopAtDurationEnd -or $trigger.EndBoundary -or -not $trigger.Enabled) {
        throw 'Existing recovery trigger has unexpected semantics; inspect it before changing it.'
    }
} else {
    $trigger = New-ScheduledTaskTrigger -Once -At (Get-Date).AddSeconds($StartDelaySeconds) `
        -RepetitionInterval (New-TimeSpan -Minutes 1)
    $trigger.Id = $recoveryId
    $trigger.Repetition.StopAtDurationEnd = $false
}

$receipt = [ordered]@{
    taskName = $TaskName; taskPath = $TaskPath; applied = [bool]$Apply
    changed = $changed; interval = 'PT1M'; startBoundary = $trigger.StartBoundary
    originalLastRunTime = $beforeInfo.LastRunTime; originalLastTaskResult = $beforeInfo.LastTaskResult
    executableSha256 = $ExpectedSha256.ToLowerInvariant(); observedAtUtc = [datetime]::UtcNow.ToString('o')
}
New-Item -ItemType Directory -Path $EvidenceDirectory -Force | Out-Null
$prefix = Join-Path $EvidenceDirectory ('task-recovery-' + [guid]::NewGuid().ToString('N'))
[IO.File]::WriteAllText($prefix + '.before.xml', $before)
if ($Apply) {
    # Updating only triggers preserves the running instance, principal, action,
    # restart policy, and intentional native desired=false maintenance stops.
    # Native manage run reads desired state before starting any frontend.
    if ($changed) {
        $current = Export-ScheduledTask -TaskName $TaskName -TaskPath $TaskPath
        if ($current -cne $before) { throw 'Task definition changed during preparation.' }
        $triggers = @($task.Triggers | Where-Object { $null -ne $_ }) + @($trigger)
        Set-ScheduledTask -TaskName $TaskName -TaskPath $TaskPath -Trigger $triggers | Out-Null
    }
    $after = Export-ScheduledTask -TaskName $TaskName -TaskPath $TaskPath
    [IO.File]::WriteAllText($prefix + '.after.xml', $after)
    [xml]$beforeXml = $before
    [xml]$afterXml = $after
    foreach ($section in 'Actions', 'Principals', 'Settings') {
        if ($beforeXml.Task.$section.OuterXml -ne $afterXml.Task.$section.OuterXml) {
            throw "Task $section changed unexpectedly; inspect the saved definitions."
        }
    }
    $observed = Get-ScheduledTask -TaskName $TaskName -TaskPath $TaskPath
    $saved = @($observed.Triggers | Where-Object Id -EQ $recoveryId)
    if ($saved.Count -ne 1 -or $saved[0].Repetition.Interval -ne 'PT1M' -or
        $saved[0].Repetition.Duration -or $saved[0].Repetition.StopAtDurationEnd -or
        $saved[0].EndBoundary -or -not $saved[0].Enabled) {
        throw 'Recovery trigger readback failed.'
    }
    if ($EnableTaskHistory) {
        & "$env:SystemRoot\System32\wevtutil.exe" sl Microsoft-Windows-TaskScheduler/Operational /e:true
        if ($LASTEXITCODE -ne 0) { throw 'Could not enable Task Scheduler operational history.' }
        $receipt.taskHistoryEnabled = (Get-WinEvent -ListLog Microsoft-Windows-TaskScheduler/Operational).IsEnabled
        if (-not $receipt.taskHistoryEnabled) { throw 'Task history enablement readback failed.' }
    }
    $receipt.nextRunTime = ($observed | Get-ScheduledTaskInfo).NextRunTime
}
$receipt.evidencePrefix = $prefix
$receipt | ConvertTo-Json | Tee-Object -FilePath ($prefix + '.json')
