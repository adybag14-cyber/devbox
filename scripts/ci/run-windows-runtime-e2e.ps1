[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ($env:GITHUB_ACTIONS -ne 'true' -and $env:DEVBOX_E2E_ISOLATED_CHECKOUT -ne '1') {
    throw 'Run managed lifecycle certification only in an explicitly isolated checkout.'
}
if (-not $env:DEVBOX_MCP_TEST_BINARY -or -not (Test-Path -LiteralPath $env:DEVBOX_MCP_TEST_BINARY)) {
    throw 'Supply DEVBOX_MCP_TEST_BINARY for the verified C++ candidate.'
}
$testStartedAt = Get-Date
$ownedGuardianScript = Join-Path $root 'scripts\devbox-guardian.mjs'
function Wait-CiGuardianHeartbeat {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)]$GuardianProcess,
        [ValidateRange(1, 60000)][int]$TimeoutMilliseconds = 30000
    )
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        $live = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f $GuardianProcess.ProcessId) -ErrorAction SilentlyContinue
        if (-not $live -or $live.CreationDate -ne $GuardianProcess.CreationDate -or
            $live.ParentProcessId -ne $GuardianProcess.ParentProcessId -or
            $live.ExecutablePath -ne $GuardianProcess.ExecutablePath -or
            $live.CommandLine -cne $GuardianProcess.CommandLine) {
            throw 'Verified Guardian process changed before its first heartbeat.'
        }
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            $heartbeat = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop | ConvertFrom-Json
            if (-not $heartbeat.PSObject.Properties['GuardianPid'] -or
                [int]$heartbeat.GuardianPid -ne $GuardianProcess.ProcessId) {
                throw 'First heartbeat is not associated with the verified Guardian process.'
            }
            return $heartbeat
        }
        $remaining = $TimeoutMilliseconds - $timer.ElapsedMilliseconds
        if ($remaining -le 0) { break }
        Start-Sleep -Milliseconds ([Math]::Min(100, $remaining))
    } while ($timer.ElapsedMilliseconds -lt $TimeoutMilliseconds)
    throw "Verified Guardian did not publish its first heartbeat within $TimeoutMilliseconds ms."
}
function Stop-OwnedCiGuardian {
    param([int]$TargetProcessId)
    if ($TargetProcessId -le 0) { return }
    $owned = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f $TargetProcessId) -ErrorAction SilentlyContinue
    if (-not $owned) { return }
    if ([string]$owned.CommandLine -match 'codex\.js|@openai/codex') { throw 'Protected host cannot be stopped.' }
    if (([string]$owned.CommandLine).IndexOf($ownedGuardianScript, [StringComparison]::OrdinalIgnoreCase) -lt 0 -or
        $owned.CreationDate -lt $testStartedAt) { throw 'Guardian cleanup could not verify this test process.' }
    Stop-Process -Id $TargetProcessId -Force -ErrorAction Stop
    if (Get-Process -Id $TargetProcessId -ErrorAction SilentlyContinue) { throw 'Owned test Guardian did not exit.' }
}
$taskPrefix = 'ChatGptDevboxCi-{0}' -f ([Guid]::NewGuid().ToString('N').Substring(0, 12))
$envPath = Join-Path $root '.env'
$runtimeEnv = Join-Path $root '.env.runtime'
$hadEnv = Test-Path $envPath
$envBackup = if ($hadEnv) { Get-Content $envPath -Raw } else { $null }
if ((Test-Path -LiteralPath $envPath) -or (Test-Path -LiteralPath $runtimeEnv)) {
    throw 'Lifecycle fixture requires a fresh checkout without existing runtime configuration.'
}
$port = if ($env:DEVBOX_E2E_PORT) { [int]$env:DEVBOX_E2E_PORT } else { 18184 }
$isolatedEnvNames = @(
    'CPP_MCP_EXE', 'DEVBOX_MCP_IMPLEMENTATION', 'DEVBOX_RUNTIME_MODE', 'PORT', 'HOST', 'MCP_AUTH_MODE',
    'PUBLIC_BASE_URL',
    'CLOUDFLARED_PUBLIC_HOSTNAME',
    'CLOUDFLARED_TUNNEL_TOKEN',
    'CLOUDFLARED_TUNNEL_TOKEN_FILE',
    'CLOUDFLARED_TUNNEL_ID',
    'CLOUDFLARED_TUNNEL_NAME'
)
$previousProcessEnvironment = @{}
foreach ($name in $isolatedEnvNames) {
    $previousProcessEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    [Environment]::SetEnvironmentVariable($name, $null, 'Process')
}

function Remove-CiTasks {
    foreach ($name in @("$taskPrefix-Startup", "$taskPrefix-Logon", "$taskPrefix-KeepAlive", "$taskPrefix-McpElevatedStart")) {
        if (Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue) {
            Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
        }
    }
}

try {
    $env:CPP_MCP_EXE = $env:DEVBOX_MCP_TEST_BINARY
    $config = @("PORT=$port", 'HOST=127.0.0.1', 'DEVBOX_RUNTIME_MODE=host', 'DEVBOX_MCP_IMPLEMENTATION=cpp', "CPP_MCP_EXE=$env:DEVBOX_MCP_TEST_BINARY", 'MCP_AUTH_MODE=none', 'PUBLIC_BASE_URL=', 'ENABLE_HOST_EXEC=true', 'HOST_PROGRAM_ALLOWLIST=powershell,pwsh,cmd,git,gh,node,npm,npx,python,py,pip,rg,curl', 'DEVBOX_PROGRAM_ALLOWLIST=powershell,pwsh,cmd,git,gh,node,npm,npx,python,py,pip,rg,curl') -join "`n"
    [IO.File]::WriteAllText($envPath, "$config`n", [Text.UTF8Encoding]::new($false))
    $env:DEVBOX_MCP_IMPLEMENTATION = 'cpp'
    $env:DEVBOX_RUNTIME_MODE = 'host'
    $env:PORT = [string]$port
    $env:HOST = '127.0.0.1'
    $env:MCP_AUTH_MODE = 'none'
    $env:PUBLIC_BASE_URL = ''

    & (Join-Path $root 'scripts\Start-ChatGptDevboxMcp.ps1') -Runtime host
    $health = Invoke-WebRequest -Uri "http://127.0.0.1:$port/readyz" -UseBasicParsing -TimeoutSec 5
    if ($health.StatusCode -ne 200 -or $health.Content -notmatch 'ok') { throw 'C++ MCP health gate failed.' }

    $workspace = Join-Path $root 'run\ci-windows-workspace'
    New-Item -ItemType Directory -Path $workspace -Force | Out-Null
    & node (Join-Path $root 'scripts\ci\platform-runtime-e2e.mjs') --url "http://127.0.0.1:$port/" --workspace $workspace --expect-platform windows
    if ($LASTEXITCODE -ne 0) { throw 'Managed Windows MCP SDK gate failed.' }

    $trackedDirty = ((& git -C $root status --porcelain --untracked-files=no | Out-String).Trim()).Length -gt 0
    if (-not $trackedDirty) {
        $currentCppManifestPath = Join-Path $root 'run\bin\current-cpp.json'
        $firstManifest = Get-Content $currentCppManifestPath -Raw -ErrorAction Stop | ConvertFrom-Json
        Start-Sleep -Seconds 1
        & (Join-Path $root 'scripts\Start-ChatGptDevboxMcp.ps1') -Runtime host
        $secondManifest = Get-Content $currentCppManifestPath -Raw -ErrorAction Stop | ConvertFrom-Json
        if ([string]$secondManifest.PromotedAtUtc -ne [string]$firstManifest.PromotedAtUtc) {
            throw 'Restarting the same immutable C++ candidate changed PromotedAtUtc.'
        }
        if ([string]$secondManifest.FirstPromotedAtUtc -ne [string]$firstManifest.FirstPromotedAtUtc) {
            throw 'Restarting the same immutable C++ candidate changed FirstPromotedAtUtc.'
        }
        if ([DateTime]$secondManifest.LastStartedAtUtc -le [DateTime]$firstManifest.LastStartedAtUtc) {
            throw 'Restarting the same immutable C++ candidate did not advance LastStartedAtUtc.'
        }
        $health = Invoke-WebRequest -Uri "http://127.0.0.1:$port/readyz" -UseBasicParsing -TimeoutSec 5
        if ($health.StatusCode -ne 200) { throw 'C++ MCP readiness failed after same-candidate restart.' }

    } else {
        Write-Host 'Skipping same-candidate restart provenance assertion because the local checkout has tracked modifications.'
    }

    $guardianDir = Join-Path $root 'run\guardian'
    New-Item -ItemType Directory -Path $guardianDir -Force | Out-Null
    $staleGuardianArtifact = Join-Path $guardianDir 'ci-stale-artifact.tmp'
    [IO.File]::WriteAllText($staleGuardianArtifact, 'stale', [Text.UTF8Encoding]::new($false))
    (Get-Item $staleGuardianArtifact).LastWriteTimeUtc = [DateTime]::UtcNow.AddDays(-8)
    $ensureLog = Join-Path $guardianDir 'ensure.log'
    [IO.File]::WriteAllText($ensureLog, ('x' * (1MB + 1024)), [Text.UTF8Encoding]::new($false))

    try {
        & (Join-Path $root 'scripts\Install-ChatGptDevboxGuardian.ps1') -Runtime host -TaskPrefix $taskPrefix | Out-Host
    } catch {
        Write-Host '--- Guardian Ensure diagnostic tail ---'
        Get-Content $ensureLog -Tail 80 -ErrorAction SilentlyContinue | Out-Host
        Write-Host '--- Guardian supervisor diagnostic tail ---'
        Get-Content (Join-Path $guardianDir 'guardian.log') -Tail 80 -ErrorAction SilentlyContinue | Out-Host
        throw
    }
    if (Test-Path $staleGuardianArtifact) { throw 'Guardian Ensure did not prune stale atomic-write debris.' }
    if (-not (Test-Path "$ensureLog.1")) { throw 'Guardian Ensure did not rotate the oversized auxiliary log.' }
    $startupTask = Get-ScheduledTask -TaskName "$taskPrefix-Startup"
    $logonTask = Get-ScheduledTask -TaskName "$taskPrefix-Logon"
    $keepAliveTask = Get-ScheduledTask -TaskName "$taskPrefix-KeepAlive"
    $startupTriggerTypes = @($startupTask.Triggers | ForEach-Object { $_.CimClass.CimClassName })
    $logonTriggerTypes = @($logonTask.Triggers | ForEach-Object { $_.CimClass.CimClassName })
    if ($startupTriggerTypes -notcontains 'MSFT_TaskBootTrigger') { throw 'Guardian startup task is missing AtStartup trigger.' }
    if ($logonTriggerTypes -notcontains 'MSFT_TaskLogonTrigger') { throw 'Guardian logon task is missing AtLogon trigger.' }
    if ([string]$startupTask.Principal.LogonType -ne 'S4U') { throw "Guardian startup task must be non-interactive S4U, got $($startupTask.Principal.LogonType)." }
    if ([string]$keepAliveTask.Principal.LogonType -ne 'S4U') { throw "Guardian keepalive task must be non-interactive S4U, got $($keepAliveTask.Principal.LogonType)." }
    if ([string]$logonTask.Principal.LogonType -ne 'Interactive') { throw "Guardian logon task must remain Interactive, got $($logonTask.Principal.LogonType)." }
    foreach ($guardianTask in @($startupTask, $logonTask, $keepAliveTask)) {
        if ([string]$guardianTask.Principal.RunLevel -ne 'Highest') { throw 'Guardian CI task is not Highest run level.' }
        if ($guardianTask.Settings.RestartCount -lt 3) { throw 'Guardian CI task restart policy is missing.' }
        if ([string]$guardianTask.Actions[0].Execute -match 'wscript\.exe$') { throw 'Guardian supervision task still uses the VBS/wscript hop.' }
    }
    if ([string]$startupTask.Actions[0].Execute -notmatch 'node\.exe$') { throw 'Guardian AtStartup task must launch Node directly on the boot-critical path.' }
    if ([string]$startupTask.Actions[0].Arguments -notmatch 'devbox-guardian\.mjs') { throw 'Guardian AtStartup task is not launching the Guardian supervisor directly.' }
    if ([string]$startupTask.Actions[0].Arguments -notmatch '--direct-owner') { throw 'Guardian AtStartup task is not forcing direct process ownership.' }
    foreach ($guardianTask in @($logonTask, $keepAliveTask)) {
        if ([string]$guardianTask.Actions[0].Execute -notmatch 'pwsh\.exe$|powershell\.exe$') { throw 'Guardian recovery task is not invoking PowerShell Ensure directly.' }
    }
    if ([string]$keepAliveTask.Triggers[0].Repetition.Interval -ne 'PT10M') { throw 'Guardian keepalive task is not repeating on the 10-minute recovery cadence.' }

    $guardianPidPath = Join-Path $root 'run\guardian\guardian.pid'
    $heartbeatPath = Join-Path $root 'run\guardian\heartbeat.json'
    $guardianPid = [int](Get-Content $guardianPidPath -ErrorAction Stop | Select-Object -First 1)
    $guardianProcess = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f $guardianPid) -ErrorAction SilentlyContinue
    if (-not $guardianProcess -or ([string]$guardianProcess.CommandLine) -notmatch 'devbox-guardian\.mjs') {
        throw 'Guardian installer did not leave the persistent direct Node Guardian running.'
    }
    $firstHeartbeat = Wait-CiGuardianHeartbeat -Path $heartbeatPath -GuardianProcess $guardianProcess
    Start-Sleep -Seconds 12
    $secondHeartbeat = Get-Content $heartbeatPath -Raw -ErrorAction Stop | ConvertFrom-Json
    $guardianProcess = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f $guardianPid) -ErrorAction SilentlyContinue
    if (-not $guardianProcess -or ([string]$guardianProcess.CommandLine) -notmatch 'devbox-guardian\.mjs') {
        throw 'Direct Node Guardian did not remain running.'
    }
    if (-not $secondHeartbeat.PSObject.Properties['GuardianPid'] -or [int]$secondHeartbeat.GuardianPid -ne $guardianPid) {
        throw 'Heartbeat is not associated with the persistent Guardian process.'
    }
    if ([DateTime]$secondHeartbeat.ObservedAtUtc -le [DateTime]$firstHeartbeat.ObservedAtUtc) {
        throw 'Guardian heartbeat did not advance after installation.'
    }

    & node (Join-Path $root 'scripts\devbox-guardian.mjs') --project-root $root --once --no-repair | Out-Host
    $state = Get-Content (Join-Path $root 'run\guardian\state.json') -Raw | ConvertFrom-Json
    if (-not $state.IsHealthy) { throw "Guardian did not classify native C++ runtime healthy: $($state.Reasons -join '; ')" }

    $metadata = Invoke-RestMethod -Uri "http://127.0.0.1:$port/" -TimeoutSec 5
    if (-not $metadata.build.gitSha -or -not $metadata.build.binarySha256) { throw 'Build provenance was not exposed.' }
    Write-Host "Windows native lifecycle E2E passed: PID=$($state.McpProcessId) git=$($metadata.build.gitSha)"
} finally {
    Remove-CiTasks
    $guardianPidPath = Join-Path $root 'run\guardian\guardian.pid'
    try {
        if (Test-Path $guardianPidPath) {
            $guardianPid = [int](Get-Content $guardianPidPath -ErrorAction Stop | Select-Object -First 1)
            if ($guardianPid -gt 0) {
                $guardianProcess = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f $guardianPid) -ErrorAction SilentlyContinue
                if ($guardianProcess) { Stop-OwnedCiGuardian -TargetProcessId $guardianPid }
            }
        }
    } catch {
        Write-Warning ("Guardian wrapper cleanup failed: {0}" -f $_.Exception.Message)
    }

    $heartbeatPath = Join-Path $root 'run\guardian\heartbeat.json'
    try {
        if (Test-Path $heartbeatPath) {
            $heartbeat = Get-Content $heartbeatPath -Raw -ErrorAction Stop | ConvertFrom-Json
            $supervisorPid = [int]$heartbeat.SupervisorPid
            if ($supervisorPid -gt 0) {
                $supervisorProcess = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f $supervisorPid) -ErrorAction SilentlyContinue
                if ($supervisorProcess) { Stop-OwnedCiGuardian -TargetProcessId $supervisorPid }
            }
        }
    } catch {
        Write-Warning ("Guardian supervisor cleanup failed: {0}" -f $_.Exception.Message)
    }

    foreach ($stalePath in @((Join-Path $root 'run\guardian\guardian.lock'), $guardianPidPath)) {
        try {
            Remove-Item $stalePath -Force -ErrorAction Stop
        } catch [System.Management.Automation.ItemNotFoundException] {
        } catch {
            Write-Warning ("Guardian state cleanup failed for {0}: {1}" -f $stalePath, $_.Exception.Message)
        }
    }

    try {
        & (Join-Path $root 'scripts\Stop-ChatGptDevboxMcp.ps1') -ErrorAction Stop | Out-Null
    } catch {
        Write-Warning ("MCP cleanup failed: {0}" -f $_.Exception.Message)
    }
    if ($hadEnv) {
        [IO.File]::WriteAllText($envPath, $envBackup, [Text.UTF8Encoding]::new($false))
    } else {
        Remove-Item $envPath -Force -ErrorAction SilentlyContinue
    }
    Remove-Item $runtimeEnv -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $root 'run\guardian\ci-stale-artifact.tmp') -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $root 'run\guardian\ensure.log.1') -Force -ErrorAction SilentlyContinue
    foreach ($name in $isolatedEnvNames) {
        [Environment]::SetEnvironmentVariable($name, $previousProcessEnvironment[$name], 'Process')
    }
}
