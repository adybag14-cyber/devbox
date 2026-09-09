[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Load only the readiness helper; never execute the managed service fixture.
$scriptPath = Join-Path $PSScriptRoot 'run-windows-runtime-e2e.ps1'
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'Lifecycle fixture has PowerShell parse errors.' }
$helper = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Wait-CiGuardianHeartbeat' }, $true)
if (-not $helper) { throw 'Guardian heartbeat helper is missing.' }
. ([scriptblock]::Create($helper.Extent.Text))

$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixtureRoot = [IO.Path]::GetFullPath((Join-Path $temporaryRoot ('devbox-heartbeat-test-' + [Guid]::NewGuid().ToString('N'))))
if (-not $fixtureRoot.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture path escaped the temporary directory.' }
[void](New-Item -ItemType Directory -Path $fixtureRoot)
$heartbeatPath = Join-Path $fixtureRoot 'heartbeat.json'
$currentProcess = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f $PID)
$writer = $null

function Assert-Rejected {
    param([scriptblock]$Action, [string]$Expected)
    try { & $Action | Out-Null } catch {
        if ($_.Exception.Message -notlike "*$Expected*") { throw }
        return
    }
    throw "Expected rejection: $Expected"
}

try {
    # Reproduce the observed race: process exists before its first heartbeat.
    $writer = Start-ThreadJob -ArgumentList $heartbeatPath, $PID -ScriptBlock {
        param($targetPath, $ownerPid)
        Start-Sleep -Milliseconds 500
        $temporaryPath = $targetPath + '.tmp'
        [IO.File]::WriteAllText($temporaryPath, (@{ GuardianPid = $ownerPid; ObservedAtUtc = [DateTime]::UtcNow.ToString('o') } | ConvertTo-Json))
        Move-Item -LiteralPath $temporaryPath -Destination $targetPath
    }
    $first = Wait-CiGuardianHeartbeat -Path $heartbeatPath -GuardianProcess $currentProcess -TimeoutMilliseconds 10000
    if ([int]$first.GuardianPid -ne $PID) { throw 'Delayed heartbeat returned the wrong owner.' }
    Receive-Job -Job $writer -Wait -ErrorAction Stop | Out-Null
    Remove-Job -Job $writer
    $writer = $null

    Remove-Item -LiteralPath $heartbeatPath -Force
    Assert-Rejected { Wait-CiGuardianHeartbeat -Path $heartbeatPath -GuardianProcess $currentProcess -TimeoutMilliseconds 150 } 'did not publish its first heartbeat'
    [IO.File]::WriteAllText($heartbeatPath, '{"GuardianPid":0}')
    Assert-Rejected { Wait-CiGuardianHeartbeat -Path $heartbeatPath -GuardianProcess $currentProcess } 'not associated with the verified Guardian'
    $changedProcess = $currentProcess | Select-Object ProcessId, CreationDate, ParentProcessId, ExecutablePath, CommandLine
    $changedProcess.CommandLine = 'different process instance'
    Assert-Rejected { Wait-CiGuardianHeartbeat -Path $heartbeatPath -GuardianProcess $changedProcess } 'process changed before its first heartbeat'
    Write-Host 'Guardian heartbeat readiness regression checks passed: delayed creation, bounded timeout, wrong owner, changed process.'
} finally {
    if ($writer) {
        Stop-Job -Job $writer -ErrorAction SilentlyContinue
        Remove-Job -Job $writer -Force -ErrorAction SilentlyContinue
    }
    if ([IO.Path]::GetFullPath($fixtureRoot) -ne $fixtureRoot -or
        -not $fixtureRoot.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing cleanup outside the verified fixture.' }
    Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
}
