[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$Sha256,
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9-]{1,100}$')][string]$PipeName,
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidatePattern('^ChatGptDevbox-ComputerUse(?:-[A-Za-z0-9-]{1,80})?$')]
    [string]$TaskName = 'ChatGptDevbox-ComputerUse',
    [switch]$Install,
    [switch]$Start
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = [IO.Path]::GetFullPath($ProjectRoot)
$binaryRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'run\bin')).TrimEnd('\') + '\'
$binaryPath = [IO.Path]::GetFullPath($Executable)
if (-not $binaryPath.StartsWith($binaryRoot, [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetDirectoryName($binaryPath) -ne $binaryRoot.TrimEnd('\')) {
    throw 'The desktop worker must use a direct immutable run/bin executable.'
}
if ((Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash -ne $Sha256) {
    throw 'Desktop worker executable hash differs from the pinned candidate.'
}
$build = (& $binaryPath --build-info | Out-String) | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $build.implementation -ne 'cpp' -or $build.sourceDirty -ne $false -or
    $build.binarySha256 -ne $Sha256) {
    throw 'Desktop worker requires a clean, provenance-verified C++ build.'
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if ($Install) {
    if (-not ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Register the desktop worker using the existing elevated Devbox operator account.'
    }
    $shellPath = (Get-Process -Id $PID).Path
    $arguments = '-NoProfile -NonInteractive -WindowStyle Hidden -File "' + $PSCommandPath +
        '" -Executable "' + $binaryPath + '" -Sha256 ' + $Sha256 + ' -PipeName ' + $PipeName +
        ' -ProjectRoot "' + $projectRoot + '" -TaskName ' + $TaskName
    $existing = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    if ($existing) {
        if ($existing.Description -ne ('Devbox native CUA desktop worker: ' + $projectRoot)) {
            throw 'Existing task is not owned by this Devbox checkout.'
        }
        if ($existing.State -eq 'Running') {
            if ($existing.Actions.Execute -eq $shellPath -and $existing.Actions.Arguments -eq $arguments) {
                Write-Output 'The pinned desktop worker task is already running.'
                return
            }
            throw 'Quiesce and gracefully stop the verified previous desktop worker before replacing its task.'
        }
    }
    $action = New-ScheduledTaskAction -Execute $shellPath -Argument $arguments -WorkingDirectory $projectRoot
    $principal = New-ScheduledTaskPrincipal -UserId $identity.Name -LogonType Interactive -RunLevel Highest
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $identity.Name
    $settings = New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew -ExecutionTimeLimit ([TimeSpan]::Zero) `
        -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) -StartWhenAvailable `
        -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -Hidden
    Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Trigger $trigger `
        -Settings $settings -Description ('Devbox native CUA desktop worker: ' + $projectRoot) -Force | Out-Null
    if ($Start) { Start-ScheduledTask -TaskName $taskName }
    Write-Output ('Registered interactive desktop worker for ' + $identity.Name + ', pipe ' + $PipeName)
    return
}
if ((Get-Process -Id $PID).SessionId -eq 0) {
    throw 'Use the Interactive scheduled task; session 0 cannot own the logged-in desktop.'
}
$stateRoot = Join-Path $projectRoot 'run\computer-use'
[void][IO.Directory]::CreateDirectory($stateRoot)
$stdout = Join-Path $stateRoot ($PipeName + '.stdout.log')
$stderr = Join-Path $stateRoot ($PipeName + '.stderr.log')
$statePath = Join-Path $stateRoot ($PipeName + '.json')
$process = Start-Process -FilePath $binaryPath -ArgumentList @('--computer-use-broker', $PipeName) `
    -WorkingDirectory $projectRoot -WindowStyle Hidden -RedirectStandardOutput $stdout `
    -RedirectStandardError $stderr -PassThru
$record = [ordered]@{
    ProcessId = $process.Id
    CreatedAtUtc = $process.StartTime.ToUniversalTime().ToString('o')
    ExecutablePath = $binaryPath
    Sha256 = $Sha256.ToLowerInvariant()
    GitSha = $build.gitSha
    SourceTree = $build.sourceTree
    SessionId = $process.SessionId
    PipeName = $PipeName
    Owner = $identity.Name
    State = 'running'
}
$record | ConvertTo-Json | Set-Content -LiteralPath $statePath -Encoding utf8
$process.WaitForExit()
$record.State = 'exited'
$record['ExitCode'] = $process.ExitCode
$record['FinishedAtUtc'] = [DateTime]::UtcNow.ToString('o')
$record | ConvertTo-Json | Set-Content -LiteralPath $statePath -Encoding utf8
exit $process.ExitCode
