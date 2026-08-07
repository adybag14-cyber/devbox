$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$startPath = Join-Path $root 'scripts\Start-ChatGptDevboxMcp.ps1'
$installPath = Join-Path $root 'scripts\Install-ChatGptDevboxGuardian.ps1'
$vbsPath = Join-Path $root 'scripts\Run-Start-ChatGptDevboxMcp.vbs'

function Assert-Contains {
    param([string]$Text, [string]$Needle, [string]$Message)
    if (-not $Text.Contains($Needle)) { throw $Message }
}

function Assert-Before {
    param([string]$Text, [string]$First, [string]$Second, [string]$Message)
    $a = $Text.IndexOf($First, [StringComparison]::Ordinal)
    $b = $Text.IndexOf($Second, [StringComparison]::Ordinal)
    if ($a -lt 0 -or $b -lt 0 -or $a -ge $b) { throw $Message }
}

$start = Get-Content -LiteralPath $startPath -Raw
$install = Get-Content -LiteralPath $installPath -Raw
$vbs = Get-Content -LiteralPath $vbsPath -Raw

Assert-Contains $start '$script:lifecycleMutex.WaitOne(0, $false)' 'Lifecycle mutex must reject concurrent starts instead of queueing them.'
Assert-Contains $start "Join-Path `$RunDir 'startup-state.json'" 'Startup phase journal is missing.'
Assert-Contains $start "Write-StartupPhase -Phase 'waiting-local-health'" 'Local-health phase journaling is missing.'
Assert-Contains $start "Write-StartupPhase -Phase 'waiting-public-health'" 'Public-health phase journaling is missing.'
Assert-Contains $start 'Assert-StartupDeadline' 'Startup deadline enforcement is missing.'
Assert-Contains $start '$script:startupMcpPid = [int]$process.Id' 'Spawned MCP PID ownership tracking is missing.'
Assert-Contains $start 'Stop-Process -Id ([int]$script:startupMcpPid) -Force' 'Failed startup must clean up the spawned MCP process.'
Assert-Before $start 'Set-Content -Path $pidFile -Value $process.Id -Encoding ASCII' '$localUrl = "http://127.0.0.1:$port"' 'MCP PID must be persisted before health validation begins.'
Assert-Contains $vbs 'shell.Run(command, 0, True)' 'Scheduled-task VBS must wait for the real startup child and propagate its exit code.'
Assert-Contains $install '-ExecutionTimeLimit (New-TimeSpan -Minutes 10)' 'Elevated startup task must have a bounded execution time.'

foreach ($file in @($startPath, $installPath)) {
    $tokens = $null
    $errors = $null
    [System.Management.Automation.Language.Parser]::ParseFile($file, [ref]$tokens, [ref]$errors) | Out-Null
    if ($errors.Count -gt 0) {
        throw "PowerShell parser errors in $file`n$($errors | Out-String)"
    }
}

$legacy = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
if (Test-Path $legacy) {
    $escapedStart = $startPath.Replace("'", "''")
    $escapedInstall = $installPath.Replace("'", "''")
    $command = @"
`$ErrorActionPreference='Stop'; foreach(`$f in @('$escapedStart','$escapedInstall')) { `$t=`$null; `$e=`$null; [System.Management.Automation.Language.Parser]::ParseFile(`$f,[ref]`$t,[ref]`$e) | Out-Null; if(`$e.Count -gt 0) { throw (`$e | Out-String) } }
"@
    & $legacy -NoLogo -NoProfile -NonInteractive -Command $command
    if ($LASTEXITCODE -ne 0) { throw "Windows PowerShell 5.1 parse validation failed with exit code $LASTEXITCODE." }
}

Write-Host 'Windows Devbox startup lifecycle contract checks passed.'
