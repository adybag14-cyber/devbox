[CmdletBinding()]
param(
    [string]$ProjectRoot = 'C:\Users\adyba\docker-chatgpt-devbox',
    [int]$PollSeconds = 5,
    [int]$RepairCooldownSeconds = 30,
    [int]$FailureThreshold = 2
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$powerShellExe = 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'
$startScriptPath = Join-Path $ProjectRoot 'scripts\Start-ChatGptDevboxMcp.ps1'
$envFile = Join-Path $ProjectRoot '.env'
$runtimeEnvFile = Join-Path $ProjectRoot '.env.runtime'
$runDir = Join-Path $ProjectRoot 'run'
$guardianDir = Join-Path $runDir 'guardian'
$repairDir = Join-Path $guardianDir 'repairs'
$pidFile = Join-Path $runDir 'mcp.pid'
$desiredStatePath = Join-Path $runDir 'guardian.desired-state.json'
$settingsPath = Join-Path $runDir 'guardian.settings.json'
$guardianPidPath = Join-Path $guardianDir 'guardian.pid'
$heartbeatPath = Join-Path $guardianDir 'heartbeat.json'
$statePath = Join-Path $guardianDir 'state.json'
$guardianLogPath = Join-Path $guardianDir 'guardian.log'
$lastRepairPath = Join-Path $guardianDir 'last-repair.json'
$mutexName = 'Global\ChatGptDevboxGuardian'

foreach ($path in @($runDir, $guardianDir, $repairDir)) {
    if (-not (Test-Path $path)) {
        New-Item -ItemType Directory -Path $path | Out-Null
    }
}

$script:guardianMutex = New-Object System.Threading.Mutex($false, $mutexName)
$script:guardianMutexHeld = $false

try {
    $script:guardianMutexHeld = $script:guardianMutex.WaitOne(0, $false)
    if (-not $script:guardianMutexHeld) {
        exit 0
    }

    function Write-GuardianLog {
        param(
            [Parameter(Mandatory = $true)][string]$Message,
            [string]$Level = 'INFO'
        )

        $line = '{0} [{1}] {2}' -f ([DateTime]::UtcNow.ToString('o')), $Level.ToUpperInvariant(), $Message
        Add-Content -Path $guardianLogPath -Value $line -Encoding UTF8
    }

    function Write-JsonFile {
        param(
            [Parameter(Mandatory = $true)][string]$Path,
            [Parameter(Mandatory = $true)]$Value
        )

        $Value | ConvertTo-Json -Depth 8 | Set-Content -Path $Path -Encoding UTF8
    }

    function Read-JsonFile {
        param([string]$Path)

        if (-not (Test-Path $Path)) {
            return $null
        }

        try {
            return Get-Content -Path $Path -Raw | ConvertFrom-Json
        }
        catch {
            return $null
        }
    }

    function Get-EnvValue {
        param(
            [string]$FilePath,
            [string]$Name
        )

        if (-not (Test-Path $FilePath)) {
            return ''
        }

        $match = Select-String -Path $FilePath -Pattern ("^{0}=(.*)$" -f [regex]::Escape($Name)) -ErrorAction SilentlyContinue
        if (-not $match) {
            return ''
        }

        return $match.Matches[0].Groups[1].Value.Trim()
    }

    function Test-DockerEngine {
        cmd /c "docker version --format ""{{.Server.Version}}"" >NUL 2>NUL"
        return ($LASTEXITCODE -eq 0)
    }

    function Get-PrimaryEnvFile {
        if (Test-Path $runtimeEnvFile) {
            return $runtimeEnvFile
        }

        if (Test-Path $envFile) {
            return $envFile
        }

        return $null
    }

    function Get-GuardianSettings {
        $settings = Read-JsonFile -Path $settingsPath
        $primaryEnvFile = Get-PrimaryEnvFile

        $port = 8100
        $devboxContainerName = 'chatgpt-devbox-runtime'
        $cloudflaredContainerName = 'chatgpt-devbox-cloudflared'
        $publicBaseUrl = ''
        $authMode = 'none'
        $public = $false
        $oauth = $false

        if ($primaryEnvFile) {
            $portValue = Get-EnvValue -FilePath $primaryEnvFile -Name 'PORT'
            if ($portValue -match '^\d+$') {
                $port = [int]$portValue
            }

            $devboxValue = Get-EnvValue -FilePath $envFile -Name 'DEVBOX_CONTAINER_NAME'
            if ($devboxValue) {
                $devboxContainerName = $devboxValue
            }

            $cloudflaredValue = Get-EnvValue -FilePath $envFile -Name 'CLOUDFLARED_CONTAINER_NAME'
            if ($cloudflaredValue) {
                $cloudflaredContainerName = $cloudflaredValue
            }

            $publicBaseUrl = Get-EnvValue -FilePath $primaryEnvFile -Name 'PUBLIC_BASE_URL'
            if (-not $publicBaseUrl) {
                $publicBaseUrl = Get-EnvValue -FilePath $envFile -Name 'CLOUDFLARED_PUBLIC_HOSTNAME'
            }

            $authMode = Get-EnvValue -FilePath $primaryEnvFile -Name 'MCP_AUTH_MODE'
            if (-not $authMode) {
                $authMode = Get-EnvValue -FilePath $envFile -Name 'MCP_AUTH_MODE'
            }
        }

        if ($settings) {
            if ($null -ne $settings.Public) {
                $public = [bool]$settings.Public
            } else {
                $public = -not [string]::IsNullOrWhiteSpace($publicBaseUrl)
            }

            if ($null -ne $settings.OAuth) {
                $oauth = [bool]$settings.OAuth
            } else {
                $oauth = $authMode -and $authMode -ne 'none'
            }

            if ($settings.Port) {
                $port = [int]$settings.Port
            }

            if ($settings.DevboxContainerName) {
                $devboxContainerName = [string]$settings.DevboxContainerName
            }

            if ($settings.CloudflaredContainerName) {
                $cloudflaredContainerName = [string]$settings.CloudflaredContainerName
            }

            if ($settings.PublicBaseUrl) {
                $publicBaseUrl = [string]$settings.PublicBaseUrl
            }

            if ($settings.AuthMode) {
                $authMode = [string]$settings.AuthMode
            }
        } else {
            $public = -not [string]::IsNullOrWhiteSpace($publicBaseUrl)
            $oauth = $authMode -and $authMode -ne 'none'
        }

        [pscustomobject]@{
            Public = $public
            OAuth = $oauth
            Port = $port
            DevboxContainerName = $devboxContainerName
            CloudflaredContainerName = $cloudflaredContainerName
            PublicBaseUrl = [string]$publicBaseUrl
            AuthMode = [string]$authMode
        }
    }

    function Get-DesiredState {
        $state = Read-JsonFile -Path $desiredStatePath
        if ($state -and $null -ne $state.ShouldRun) {
            return [pscustomobject]@{
                ShouldRun = [bool]$state.ShouldRun
                UpdatedAtUtc = [string]$state.UpdatedAtUtc
                Source = [string]$state.Source
            }
        }

        return [pscustomobject]@{
            ShouldRun = $false
            UpdatedAtUtc = $null
            Source = $null
        }
    }

    function Get-OwnedMcpProcess {
        $ownedProcess = $null
        if (Test-Path $pidFile) {
            $pidText = Get-Content -Path $pidFile -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($pidText -match '^\d+$') {
                $processIdValue = [int]$pidText
                $candidate = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f $processIdValue) -ErrorAction SilentlyContinue
                if ($candidate -and ([string]$candidate.CommandLine) -match 'src[\\/]server\.js') {
                    $ownedProcess = $candidate
                }
            }
        }

        if ($ownedProcess) {
            return $ownedProcess
        }

        return Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
            Where-Object { ([string]$_.CommandLine) -match 'src[\\/]server\.js' -and ([string]$_.CommandLine) -match 'docker-chatgpt-devbox' } |
            Select-Object -First 1
    }

    function Test-ContainerRunning {
        param([string]$ContainerName)

        if ([string]::IsNullOrWhiteSpace($ContainerName)) {
            return $false
        }

        $result = cmd /d /c "docker inspect --type container $ContainerName --format ""{{.State.Running}}"" 2>NUL"
        if ($LASTEXITCODE -ne 0) {
            return $false
        }

        return ($result.Trim() -eq 'true')
    }

    function Test-HealthUrl {
        param([string]$Url)

        if ([string]::IsNullOrWhiteSpace($Url)) {
            return $false
        }

        try {
            $response = Invoke-WebRequest -Uri $Url -UseBasicParsing -TimeoutSec 5
            return ($response.Content -match 'ok')
        }
        catch {
            return $false
        }
    }

    function Get-StackState {
        $settings = Get-GuardianSettings
        $desiredState = Get-DesiredState
        $reasons = New-Object System.Collections.Generic.List[string]
        $dockerReady = Test-DockerEngine
        $devboxRunning = $false
        $cloudflaredRunning = $null
        $localHealthy = $false
        $publicHealthy = $null
        $mcpProcess = Get-OwnedMcpProcess
        $mcpProcessHealthy = $false

        if ($desiredState.ShouldRun) {
            if (-not $dockerReady) {
                $reasons.Add('docker engine not ready')
            } else {
                $devboxRunning = Test-ContainerRunning -ContainerName $settings.DevboxContainerName
                if (-not $devboxRunning) {
                    $reasons.Add("devbox container $($settings.DevboxContainerName) is not running")
                }

                if ($settings.Public) {
                    $cloudflaredRunning = Test-ContainerRunning -ContainerName $settings.CloudflaredContainerName
                    if (-not $cloudflaredRunning) {
                        $reasons.Add("cloudflared container $($settings.CloudflaredContainerName) is not running")
                    }
                }
            }

            if (-not $mcpProcess) {
                $reasons.Add('MCP server process is missing')
            } else {
                $mcpProcessHealthy = $true
            }

            $localHealthy = Test-HealthUrl -Url ("http://127.0.0.1:{0}/healthz" -f $settings.Port)
            if (-not $localHealthy) {
                $reasons.Add("local health check failed on port $($settings.Port)")
            }

            if ($settings.Public) {
                $publicHealthy = Test-HealthUrl -Url ("{0}/healthz" -f $settings.PublicBaseUrl.TrimEnd('/'))
                if (-not $publicHealthy) {
                    $reasons.Add('public health check failed')
                }
            }
        }

        [pscustomobject]@{
            ObservedAtUtc = [DateTime]::UtcNow.ToString('o')
            DesiredState = $desiredState
            Settings = $settings
            DockerReady = $dockerReady
            DevboxRunning = $devboxRunning
            CloudflaredRunning = $cloudflaredRunning
            McpProcessId = if ($mcpProcess) { [int]$mcpProcess.ProcessId } else { $null }
            McpProcessCommandLine = if ($mcpProcess) { [string]$mcpProcess.CommandLine } else { $null }
            McpProcessHealthy = $mcpProcessHealthy
            LocalHealth = $localHealthy
            PublicHealth = $publicHealthy
            IsHealthy = ($desiredState.ShouldRun -eq $false) -or ($reasons.Count -eq 0)
            Reasons = @($reasons)
        }
    }

    function Invoke-GuardianRepair {
        param(
            [Parameter(Mandatory = $true)]$State
        )

        $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmssfff')
        $stdoutPath = Join-Path $repairDir ("$stamp-stdout.log")
        $stderrPath = Join-Path $repairDir ("$stamp-stderr.log")
        $arguments = @(
            '-NoProfile'
            '-NonInteractive'
            '-ExecutionPolicy'
            'Bypass'
            '-File'
            $startScriptPath
        )

        if ([bool]$State.Settings.Public) {
            $arguments += '-Public'
        }
        if ([bool]$State.Settings.OAuth) {
            $arguments += '-OAuth'
        }

        $reasonText = ($State.Reasons -join '; ')
        Write-GuardianLog -Level 'REPAIR' -Message ("repair start: {0}" -f $reasonText)

        $launcher = Start-Process -FilePath $powerShellExe `
            -ArgumentList $arguments `
            -WindowStyle Hidden `
            -PassThru `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath

        $recovered = $false
        $probeState = $null
        for ($i = 0; $i -lt 30; $i++) {
            Start-Sleep -Seconds 3
            $probeState = Get-StackState
            if ($probeState.IsHealthy) {
                $recovered = $true
                break
            }

            if ($launcher.HasExited -and $launcher.ExitCode -ne 0) {
                break
            }
        }

        if (-not $launcher.HasExited) {
            $launcher.Refresh()
        }

        $exitCode = if ($launcher.HasExited) { [int]$launcher.ExitCode } else { $null }
        $stdoutText = if (Test-Path $stdoutPath) { [string](Get-Content -Path $stdoutPath -Raw) } else { '' }
        $stderrText = if (Test-Path $stderrPath) { [string](Get-Content -Path $stderrPath -Raw) } else { '' }
        $result = [pscustomobject]@{
            AttemptedAtUtc = [DateTime]::UtcNow.ToString('o')
            ExitCode = $exitCode
            Reason = $reasonText
            StdoutPath = $stdoutPath
            StderrPath = $stderrPath
            Succeeded = $recovered
            Public = [bool]$State.Settings.Public
            OAuth = [bool]$State.Settings.OAuth
        }

        Write-JsonFile -Path $lastRepairPath -Value $result

        if ($recovered) {
            Write-GuardianLog -Level 'REPAIR' -Message 'repair completed successfully'
            if ($stdoutText.Trim()) {
                Write-GuardianLog -Level 'REPAIR' -Message ($stdoutText.Trim())
            }
            return $true
        }

        if ($null -eq $exitCode) {
            Write-GuardianLog -Level 'ERROR' -Message 'repair did not restore health before timeout'
        } else {
            Write-GuardianLog -Level 'ERROR' -Message ("repair failed with exit code {0}" -f $exitCode)
        }
        if ($stdoutText.Trim()) {
            Write-GuardianLog -Level 'ERROR' -Message ($stdoutText.Trim())
        }
        if ($stderrText.Trim()) {
            Write-GuardianLog -Level 'ERROR' -Message ($stderrText.Trim())
        }

        return $false
    }

    Set-Content -Path $guardianPidPath -Value $PID -Encoding ASCII
    Write-GuardianLog -Message ("guardian boot pid={0}" -f $PID)

    $lastRepairAt = [DateTime]::MinValue
    $unhealthyCount = 0
    $lastReasonText = ''

    while ($true) {
        $state = Get-StackState
        Write-JsonFile -Path $heartbeatPath -Value @{
            ObservedAtUtc = [DateTime]::UtcNow.ToString('o')
            GuardianPid = $PID
            DesiredShouldRun = [bool]$state.DesiredState.ShouldRun
            IsHealthy = [bool]$state.IsHealthy
            McpProcessId = $state.McpProcessId
            Reasons = @($state.Reasons)
        }
        Write-JsonFile -Path $statePath -Value $state

        if (-not $state.DesiredState.ShouldRun) {
            $unhealthyCount = 0
            $lastReasonText = ''
            Start-Sleep -Seconds $PollSeconds
            continue
        }

        if ($state.IsHealthy) {
            $unhealthyCount = 0
            $lastReasonText = ''
            Start-Sleep -Seconds $PollSeconds
            continue
        }

        $unhealthyCount += 1
        $reasonText = $state.Reasons -join '; '
        if ($reasonText -ne $lastReasonText) {
            Write-GuardianLog -Level 'WARN' -Message ("unhealthy: {0}" -f $reasonText)
            $lastReasonText = $reasonText
        }

        if ($unhealthyCount -lt $FailureThreshold) {
            Start-Sleep -Seconds $PollSeconds
            continue
        }

        $secondsSinceRepair = ([DateTime]::UtcNow - $lastRepairAt).TotalSeconds
        if ($secondsSinceRepair -lt $RepairCooldownSeconds) {
            Start-Sleep -Seconds $PollSeconds
            continue
        }

        [void](Invoke-GuardianRepair -State $state)
        $lastRepairAt = [DateTime]::UtcNow
        $unhealthyCount = 0
        $lastReasonText = ''
        Start-Sleep -Seconds $PollSeconds
    }
}
catch {
    try {
        $message = $_.Exception.ToString()
        Add-Content -Path $guardianLogPath -Value ('{0} [FATAL] {1}' -f ([DateTime]::UtcNow.ToString('o')), $message) -Encoding UTF8
    }
    catch {
    }
    throw
}
finally {
    if ($script:guardianMutexHeld) {
        $script:guardianMutex.ReleaseMutex()
    }
    $script:guardianMutex.Dispose()
}
