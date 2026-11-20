[CmdletBinding()]
param([Parameter(Mandatory)][string]$EvidenceDirectory)
$ErrorActionPreference = 'Stop'
$fixture = Join-Path ([IO.Path]::GetFullPath($EvidenceDirectory)) ('fixture-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
$taskName = 'DevboxNative-RecoveryTest-' + [guid]::NewGuid().ToString('N')
$program = (Get-Process -Id $PID).Path
$actionFile = Join-Path $fixture 'action.ps1'
@'
param([string]$StateDirectory)
$ErrorActionPreference = 'Stop'
$record = @{pid=$PID;startedAtUtc=[datetime]::UtcNow.ToString('o');creationTimeUtc=(Get-Process -Id $PID).StartTime.ToUniversalTime().ToString('o')}
$attempts = Join-Path $StateDirectory 'attempts.jsonl'
$record | ConvertTo-Json -Compress | Add-Content -LiteralPath $attempts
if (@(Get-Content -LiteralPath $attempts).Count -eq 1) { exit 1 }
$deadline = [datetime]::UtcNow.AddSeconds(105)
while (-not (Test-Path -LiteralPath (Join-Path $StateDirectory 'release')) -and [datetime]::UtcNow -lt $deadline) {
    Start-Sleep -Milliseconds 200
}
exit 0
'@ | Set-Content -LiteralPath $actionFile
$arguments = '-NoProfile -NonInteractive -WindowStyle Hidden -File "' + $actionFile + '" -StateDirectory "' + $fixture + '"'
$action = New-ScheduledTaskAction -Execute $program -Argument $arguments -WorkingDirectory $fixture
$principal = New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType S4U -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew -ExecutionTimeLimit ([timespan]::Zero) `
    -DisallowHardTerminate -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable
$installer = Join-Path $PSScriptRoot '..\Enable-DevboxNativeTaskRecovery.ps1'
$attempts = Join-Path $fixture 'attempts.jsonl'
$registered = $false
try {
    Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Settings $settings | Out-Null
    $registered = $true
    $parameters = @{
        TaskName=$taskName; ExpectedExecutable=$program; ExpectedArguments=$arguments
        ExpectedSha256=(Get-FileHash -LiteralPath $program -Algorithm SHA256).Hash
        EvidenceDirectory=$fixture; StartDelaySeconds=5
    }
    # Identity failures must not change the live fixture task.
    $original = Export-ScheduledTask -TaskName $taskName
    $mismatchRejected = $false
    $wrongIdentity = $parameters.Clone()
    $wrongIdentity.ExpectedSha256 = '0' * 64
    try { & $installer @wrongIdentity -Apply } catch { $mismatchRejected = $true }
    if (-not $mismatchRejected -or (Export-ScheduledTask -TaskName $taskName) -ne $original) {
        throw 'Wrong executable digest was not refused without mutation.'
    }
    $wrongCase = $parameters.Clone()
    $wrongCase.ExpectedArguments = $arguments.ToUpperInvariant()
    $rejected = $false
    try { & $installer @wrongCase -Apply } catch { $rejected = $_.Exception.Message -match 'action identity' }
    if (-not $rejected) { throw 'Argument spelling mismatch was accepted.' }
    Disable-ScheduledTask -TaskName $taskName | Out-Null
    $rejected = $false
    try { & $installer @parameters -Apply } catch { $rejected = $_.Exception.Message -match 'enabled IgnoreNew' }
    if (-not $rejected) { throw 'Disabled fixture task was accepted.' }
    Enable-ScheduledTask -TaskName $taskName | Out-Null
    & $installer @parameters -Apply | Set-Content -LiteralPath (Join-Path $fixture 'installed.json')
    $installed = Export-ScheduledTask -TaskName $taskName
    & $installer @parameters -Apply | Set-Content -LiteralPath (Join-Path $fixture 'reapplied.json')
    if ((Export-ScheduledTask -TaskName $taskName) -ne $installed) { throw 'Recovery installation was not idempotent.' }
    $deadline = [datetime]::UtcNow.AddSeconds(95)
    $sawFailure = $false
    do {
        Start-Sleep -Milliseconds 400
        $records = if (Test-Path -LiteralPath $attempts) { @(Get-Content -LiteralPath $attempts | ConvertFrom-Json) } else { @() }
        if ($records.Count -eq 1 -and (Get-ScheduledTaskInfo -TaskName $taskName).LastTaskResult -eq 1) { $sawFailure = $true }
    } while ($records.Count -lt 2 -and [datetime]::UtcNow -lt $deadline)
    if (-not $sawFailure -or $records.Count -ne 2) { throw 'The failed task did not recover on the next trigger.' }
    $second = $records[1]
    # Cross a further repetition while the second invocation remains running.
    $checkAt = ([datetime]$second.startedAtUtc).ToUniversalTime().AddSeconds(65)
    while ([datetime]::UtcNow -lt $checkAt) { Start-Sleep -Milliseconds 400 }
    $records = @(Get-Content -LiteralPath $attempts | ConvertFrom-Json)
    $live = Get-CimInstance Win32_Process -Filter "ProcessId=$($second.pid)"
    if ($records.Count -ne 2 -or -not $live -or
        [math]::Abs(($live.CreationDate.ToUniversalTime() - ([datetime]$second.creationTimeUtc).ToUniversalTime()).TotalMilliseconds) -gt 1 -or
        $live.ExecutablePath -ne $program -or -not $live.CommandLine.Contains($actionFile)) {
        throw 'IgnoreNew did not preserve the exact second fixture process.'
    }
    [ordered]@{ok=$true;failureExitCode=1;attempts=$records;recoveredAutomatically=$true;noDuplicateAfterNextTrigger=$true;idempotent=$true;identityMismatchRefused=$true;fixture=$fixture} |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'scheduler-recovery-test.json')
} finally {
    if ($registered) {
        Disable-ScheduledTask -TaskName $taskName | Out-Null
        New-Item -ItemType File -Path (Join-Path $fixture 'release') -Force | Out-Null
        $deadline = [datetime]::UtcNow.AddSeconds(15)
        do {
            $remaining = @()
            if (Test-Path -LiteralPath $attempts) {
                foreach ($record in @(Get-Content -LiteralPath $attempts | ConvertFrom-Json)) {
                    $process = Get-CimInstance Win32_Process -Filter "ProcessId=$($record.pid)"
                    if ($process -and [math]::Abs(($process.CreationDate.ToUniversalTime() - ([datetime]$record.creationTimeUtc).ToUniversalTime()).TotalMilliseconds) -le 1) { $remaining += $process }
                }
            }
            if ($remaining.Count) { Start-Sleep -Milliseconds 200 }
        } while ($remaining.Count -and [datetime]::UtcNow -lt $deadline)
        if ($remaining.Count) { throw 'Fixture did not exit cooperatively; retained task and process evidence.' }
        Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
    }
}
Get-Content -LiteralPath (Join-Path $EvidenceDirectory 'scheduler-recovery-test.json')
