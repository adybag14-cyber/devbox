[CmdletBinding()]
param(
    [string]$ProjectRoot = 'C:\Users\adyba\docker-chatgpt-devbox',
    [int]$PollSeconds = 10,
    [int]$RepairCooldownSeconds = 120,
    [int]$DockerRepairCooldownSeconds = 900,
    [int]$FailureThreshold = 3,
    [int]$DockerProbeTimeoutSeconds = 5,
    [int]$DockerProbeCooldownSeconds = 60
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
$hostCloudflaredPidPath = Join-Path $runDir 'host-cloudflared.pid'
$mutexName = 'Global\ChatGptDevboxGuardian'
$script:dockerExe = $null
$script:lastDockerProbeAt = [DateTime]::MinValue
$script:lastDockerReady = $false

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

    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class ChatGptDevboxGuardianTokenProbe {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(UInt32 access, bool inheritHandle, UInt32 processId);

    [DllImport("advapi32.dll", SetLastError = true)]
    public static extern bool OpenProcessToken(IntPtr processHandle, UInt32 desiredAccess, out IntPtr tokenHandle);

    [DllImport("advapi32.dll", SetLastError = true)]
    public static extern bool GetTokenInformation(IntPtr tokenHandle, int tokenInfoClass, IntPtr tokenInfo, int tokenInfoLength, out int returnLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr handle);
}
"@

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

        $directory = Split-Path -Parent $Path
        if ($directory -and -not (Test-Path $directory)) {
            New-Item -ItemType Directory -Path $directory | Out-Null
        }

        $json = $Value | ConvertTo-Json -Depth 8
        $encoding = [System.Text.UTF8Encoding]::new($false)
        $tempPath = Join-Path $directory ("{0}.{1}.tmp" -f [System.IO.Path]::GetFileName($Path), [System.Guid]::NewGuid().ToString('N'))
        $backupPath = $null

        try {
            [System.IO.File]::WriteAllText($tempPath, $json, $encoding)
            for ($attempt = 0; $attempt -lt 5; $attempt++) {
                try {
                    if (Test-Path $Path) {
                        $backupPath = Join-Path $directory ("{0}.{1}.bak" -f [System.IO.Path]::GetFileName($Path), [System.Guid]::NewGuid().ToString('N'))
                        [System.IO.File]::Replace($tempPath, $Path, $backupPath, $true)
                        if (Test-Path $backupPath) {
                            Remove-Item -LiteralPath $backupPath -Force -ErrorAction SilentlyContinue
                        }
                        $backupPath = $null
                    } else {
                        Move-Item -LiteralPath $tempPath -Destination $Path -Force
                    }
                    $tempPath = $null
                    return
                }
                catch [System.IO.IOException] {
                    if ($attempt -ge 4) {
                        throw
                    }
                    Start-Sleep -Milliseconds 200
                }
            }
        }
        finally {
            if ($tempPath -and (Test-Path $tempPath)) {
                Remove-Item -LiteralPath $tempPath -Force -ErrorAction SilentlyContinue
            }
            if ($backupPath -and (Test-Path $backupPath)) {
                Remove-Item -LiteralPath $backupPath -Force -ErrorAction SilentlyContinue
            }
        }
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

    function Resolve-DockerExecutable {
        param([switch]$AllowMissing)

        if ($script:dockerExe -and (Test-Path $script:dockerExe)) {
            return $script:dockerExe
        }

        $candidates = New-Object System.Collections.Generic.List[string]
        foreach ($candidate in @(
                $(if ($env:DOCKER_EXE) { $env:DOCKER_EXE } else { $null }),
                $(if ($env:ProgramW6432) { Join-Path $env:ProgramW6432 'Docker\Docker\resources\bin\docker.exe' } else { $null }),
                $(if ($env:ProgramFiles) { Join-Path $env:ProgramFiles 'Docker\Docker\resources\bin\docker.exe' } else { $null }),
                'C:\Program Files\Docker\Docker\resources\bin\docker.exe'
            )) {
            if (-not [string]::IsNullOrWhiteSpace($candidate) -and -not $candidates.Contains($candidate)) {
                $candidates.Add($candidate)
            }
        }

        foreach ($filePath in @($runtimeEnvFile, $envFile)) {
            $configuredPath = Get-EnvValue -FilePath $filePath -Name 'DOCKER_EXE'
            if (-not [string]::IsNullOrWhiteSpace($configuredPath) -and -not $candidates.Contains($configuredPath)) {
                $candidates.Add($configuredPath)
            }
        }

        $dockerCommand = Get-Command docker -ErrorAction SilentlyContinue
        if ($dockerCommand -and -not [string]::IsNullOrWhiteSpace($dockerCommand.Source) -and -not $candidates.Contains($dockerCommand.Source)) {
            $candidates.Add([string]$dockerCommand.Source)
        }

        foreach ($candidate in $candidates) {
            if (Test-Path $candidate) {
                $script:dockerExe = $candidate
                return $script:dockerExe
            }
        }

        if ($AllowMissing) {
            return $null
        }

        throw 'Docker CLI was not found. Install Docker Desktop or set DOCKER_EXE in .env to docker.exe.'
    }

    function Invoke-Docker {
        param(
            [Parameter(Mandatory = $true)][string[]]$Arguments,
            [switch]$IgnoreExitCode,
            [switch]$AllowMissing,
            [int]$TimeoutSeconds = 60
        )

        $dockerExe = Resolve-DockerExecutable -AllowMissing:$AllowMissing
        if (-not $dockerExe) {
            return [pscustomobject]@{
                ExitCode = 127
                Output = @()
                Text = ''
            }
        }

        $process = $null
        $stdoutText = ''
        $stderrText = ''
        $timedOut = $false

        try {
            $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
            $startInfo.FileName = $dockerExe
            $startInfo.UseShellExecute = $false
            $startInfo.CreateNoWindow = $true
            $startInfo.RedirectStandardOutput = $true
            $startInfo.RedirectStandardError = $true
            Add-ProcessArguments -StartInfo $startInfo -Arguments $Arguments

            $process = [System.Diagnostics.Process]::new()
            $process.StartInfo = $startInfo
            [void]$process.Start()
            $stdoutTask = $process.StandardOutput.ReadToEndAsync()
            $stderrTask = $process.StandardError.ReadToEndAsync()

            $timeoutMs = [Math]::Max(1, $TimeoutSeconds) * 1000
            $timedOut = -not $process.WaitForExit($timeoutMs)
            if ($timedOut) {
                try {
                    $process.Kill()
                } catch {
                }
                [void]$process.WaitForExit(2000)
            }

            if (-not $timedOut) {
                try {
                    $stdoutText = [string]$stdoutTask.GetAwaiter().GetResult()
                } catch {
                    $stdoutText = ''
                }
                try {
                    $stderrText = [string]$stderrTask.GetAwaiter().GetResult()
                } catch {
                    $stderrText = ''
                }
            }

            $exitCode = if ($timedOut) { 124 } else { [int]$process.ExitCode }
            $text = (($stdoutText, $stderrText) -join '').Trim()
        }
        finally {
            if ($process) {
                $process.Dispose()
            }
        }

        $displayArguments = Format-DockerArgumentsForLog -Arguments $Arguments
        if ($timedOut) {
            if (-not $IgnoreExitCode) {
                throw "docker $displayArguments timed out after $TimeoutSeconds seconds. Output:`n$text"
            }

            return [pscustomobject]@{
                ExitCode = $exitCode
                Output = $text
                Text = $text
            }
        }

        if (-not $IgnoreExitCode -and $exitCode -ne 0) {
            $trimmedText = $text.Trim()
            if ($trimmedText) {
                throw "docker $displayArguments failed with exit code $exitCode. Output:`n$trimmedText"
            }

            throw "docker $displayArguments failed with exit code $exitCode."
        }

        return [pscustomobject]@{
            ExitCode = $exitCode
            Output = $text
            Text = $text
        }
    }

    function Test-DockerEngine {
        $now = [DateTime]::UtcNow
        if (($now - $script:lastDockerProbeAt).TotalSeconds -lt $DockerProbeCooldownSeconds) {
            return $script:lastDockerReady
        }

        $script:lastDockerProbeAt = $now
        $result = Invoke-Docker -Arguments @('version', '--format', '{{.Server.Version}}') -IgnoreExitCode -AllowMissing -TimeoutSeconds $DockerProbeTimeoutSeconds
        $script:lastDockerReady = ($result.ExitCode -eq 0)
        return $script:lastDockerReady
    }

    function Format-DockerArgumentsForLog {
        param([string[]]$Arguments)

        $redacted = New-Object System.Collections.Generic.List[string]
        for ($i = 0; $i -lt $Arguments.Count; $i++) {
            if ($i -gt 0 -and $Arguments[$i - 1] -eq '--token') {
                $redacted.Add('<redacted>')
                continue
            }

            $redacted.Add($Arguments[$i])
        }

        return ($redacted -join ' ')
    }

    function ConvertTo-WindowsProcessArgument {
        param([string]$Argument)

        $value = [string]$Argument
        if ($value.Length -gt 0 -and $value -notmatch '[\s"]') {
            return $value
        }

        return '"' + ($value.Replace('\', '\\').Replace('"', '\"')) + '"'
    }

    function Add-ProcessArguments {
        param(
            [Parameter(Mandatory = $true)][System.Diagnostics.ProcessStartInfo]$StartInfo,
            [Parameter(Mandatory = $true)][string[]]$Arguments
        )

        if ($StartInfo | Get-Member -Name ArgumentList -MemberType Property) {
            foreach ($argument in $Arguments) {
                [void]$StartInfo.ArgumentList.Add($argument)
            }
            return
        }

        $StartInfo.Arguments = (($Arguments | ForEach-Object { ConvertTo-WindowsProcessArgument -Argument $_ }) -join ' ')
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

    function Test-IsOwnedServerCommandLine {
        param([string]$CommandLine)

        if ([string]::IsNullOrWhiteSpace($CommandLine)) {
            return $false
        }

        return (
            ([string]$CommandLine -match 'src[\\/]server\.js\b') -and
            ([string]$CommandLine -match '--env-file(?:=|\s+)[^"\s]*\.env\.runtime\b')
        )
    }

    function Get-OwnedMcpProcess {
        $ownedProcess = $null
        if (Test-Path $pidFile) {
            $pidText = Get-Content -Path $pidFile -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($pidText -match '^\d+$') {
                $processIdValue = [int]$pidText
                $candidate = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f $processIdValue) -ErrorAction SilentlyContinue
                if ($candidate -and (Test-IsOwnedServerCommandLine -CommandLine ([string]$candidate.CommandLine))) {
                    $ownedProcess = $candidate
                }
            }
        }

        if ($ownedProcess) {
            return $ownedProcess
        }

        return Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
            Where-Object { Test-IsOwnedServerCommandLine -CommandLine ([string]$_.CommandLine) } |
            Sort-Object CreationDate -Descending |
            Select-Object -First 1
    }

    function Test-IsOwnedHostCloudflaredCommandLine {
        param([string]$CommandLine)

        if ([string]::IsNullOrWhiteSpace($CommandLine)) {
            return $false
        }

        return (
            ([string]$CommandLine -match 'cloudflared(?:\.exe)?') -and
            ([string]$CommandLine -match 'host-cloudflared\.tunnel-token\.txt')
        )
    }

    function Get-HostCloudflaredProcess {
        if (-not (Test-Path $hostCloudflaredPidPath)) {
            return $null
        }

        $pidText = Get-Content -Path $hostCloudflaredPidPath -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($pidText -notmatch '^\d+$') {
            return $null
        }

        $candidate = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f [int]$pidText) -ErrorAction SilentlyContinue
        if ($candidate -and (Test-IsOwnedHostCloudflaredCommandLine -CommandLine ([string]$candidate.CommandLine))) {
            return $candidate
        }

        return $null
    }

    function Get-ProcessElevationInfo {
        param([Parameter(Mandatory = $true)][uint32]$ProcessId)

        $PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        $TOKEN_QUERY = 0x0008
        $TokenElevation = 20
        $TokenIntegrityLevel = 25

        $processHandle = [ChatGptDevboxGuardianTokenProbe]::OpenProcess($PROCESS_QUERY_LIMITED_INFORMATION, $false, $ProcessId)
        if ($processHandle -eq [IntPtr]::Zero) {
            return $null
        }

        try {
            $tokenHandle = [IntPtr]::Zero
            if (-not [ChatGptDevboxGuardianTokenProbe]::OpenProcessToken($processHandle, $TOKEN_QUERY, [ref]$tokenHandle)) {
                return $null
            }

            try {
                $returnLength = 0
                $elevationBuffer = [Runtime.InteropServices.Marshal]::AllocHGlobal(4)
                try {
                    if (-not [ChatGptDevboxGuardianTokenProbe]::GetTokenInformation($tokenHandle, $TokenElevation, $elevationBuffer, 4, [ref]$returnLength)) {
                        return $null
                    }

                    $isElevated = ([Runtime.InteropServices.Marshal]::ReadInt32($elevationBuffer) -ne 0)
                }
                finally {
                    [Runtime.InteropServices.Marshal]::FreeHGlobal($elevationBuffer)
                }

                $integrityLength = 0
                [void][ChatGptDevboxGuardianTokenProbe]::GetTokenInformation($tokenHandle, $TokenIntegrityLevel, [IntPtr]::Zero, 0, [ref]$integrityLength)
                $integrityBuffer = [Runtime.InteropServices.Marshal]::AllocHGlobal($integrityLength)
                try {
                    if (-not [ChatGptDevboxGuardianTokenProbe]::GetTokenInformation($tokenHandle, $TokenIntegrityLevel, $integrityBuffer, $integrityLength, [ref]$returnLength)) {
                        return $null
                    }

                    $sidPtr = [Runtime.InteropServices.Marshal]::ReadIntPtr($integrityBuffer)
                    $subAuthorityCount = [Runtime.InteropServices.Marshal]::ReadByte($sidPtr, 1)
                    $ridOffset = 8 + 4 * ($subAuthorityCount - 1)
                    $rid = [Runtime.InteropServices.Marshal]::ReadInt32($sidPtr, $ridOffset)
                    $integrity = if ($rid -ge 0x4000) {
                        'System'
                    } elseif ($rid -ge 0x3000) {
                        'High'
                    } elseif ($rid -ge 0x2000) {
                        'Medium'
                    } else {
                        'Low'
                    }

                    return [pscustomobject]@{
                        IsElevated = $isElevated
                        Integrity = $integrity
                    }
                }
                finally {
                    [Runtime.InteropServices.Marshal]::FreeHGlobal($integrityBuffer)
                }
            }
            finally {
                if ($tokenHandle -ne [IntPtr]::Zero) {
                    [void][ChatGptDevboxGuardianTokenProbe]::CloseHandle($tokenHandle)
                }
            }
        }
        finally {
            [void][ChatGptDevboxGuardianTokenProbe]::CloseHandle($processHandle)
        }
    }

    function Test-ContainerRunning {
        param([string]$ContainerName)

        if ([string]::IsNullOrWhiteSpace($ContainerName)) {
            return $false
        }

        $result = Invoke-Docker -Arguments @('inspect', '--type', 'container', $ContainerName, '--format', '{{.State.Running}}') -IgnoreExitCode -AllowMissing
        if ($result.ExitCode -ne 0) {
            return $false
        }

        return ($result.Text.Trim() -eq 'true')
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
        $hostCloudflaredProcess = Get-HostCloudflaredProcess
        $hostCloudflaredRunning = [bool]$hostCloudflaredProcess
        $effectiveCloudflaredRunning = $hostCloudflaredRunning
        $localHealthy = $false
        $publicHealthy = $null
        $mcpProcess = Get-OwnedMcpProcess
        $mcpProcessHealthy = $false
        $mcpProcessElevated = $null
        $mcpProcessIntegrity = $null

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
                    $effectiveCloudflaredRunning = (($cloudflaredRunning -eq $true) -or $hostCloudflaredRunning)
                    if (-not $effectiveCloudflaredRunning) {
                        $reasons.Add("cloudflared tunnel is not running")
                    }
                }
            }

            if (-not $mcpProcess) {
                $reasons.Add('MCP server process is missing')
            } else {
                $elevationInfo = Get-ProcessElevationInfo -ProcessId ([uint32]$mcpProcess.ProcessId)
                if ($elevationInfo) {
                    $mcpProcessElevated = [bool]$elevationInfo.IsElevated
                    $mcpProcessIntegrity = [string]$elevationInfo.Integrity
                }

                if ($mcpProcessElevated -ne $true) {
                    $reasons.Add('MCP server process is not elevated')
                } else {
                    $mcpProcessHealthy = $true
                }
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
            CloudflaredRunning = $effectiveCloudflaredRunning
            CloudflaredContainerRunning = $cloudflaredRunning
            HostCloudflaredProcessId = if ($hostCloudflaredProcess) { [int]$hostCloudflaredProcess.ProcessId } else { $null }
            McpProcessId = if ($mcpProcess) { [int]$mcpProcess.ProcessId } else { $null }
            McpProcessCommandLine = if ($mcpProcess) { [string]$mcpProcess.CommandLine } else { $null }
            McpProcessHealthy = $mcpProcessHealthy
            McpProcessElevated = $mcpProcessElevated
            McpProcessIntegrity = $mcpProcessIntegrity
            LocalHealth = $localHealthy
            PublicHealth = $publicHealthy
            IsHealthy = ($desiredState.ShouldRun -eq $false) -or ($reasons.Count -eq 0)
            NeedsRepair = ($desiredState.ShouldRun -eq $true) -and (
                ($mcpProcessHealthy -ne $true) -or
                ($localHealthy -ne $true) -or
                ($settings.Public -and ($effectiveCloudflaredRunning -ne $true)) -or
                ($dockerReady -and (-not $devboxRunning))
            )
            Reasons = @($reasons)
        }
    }

    function Get-RepairCooldownForState {
        param([Parameter(Mandatory = $true)]$State)

        $reasons = @($State.Reasons)
        if ($reasons.Count -eq 1 -and $reasons[0] -eq 'docker engine not ready') {
            return [Math]::Max($RepairCooldownSeconds, $DockerRepairCooldownSeconds)
        }

        return $RepairCooldownSeconds
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
            Write-JsonFile -Path $heartbeatPath -Value @{
                ObservedAtUtc = [DateTime]::UtcNow.ToString('o')
                GuardianPid = $PID
                DesiredShouldRun = [bool]$probeState.DesiredState.ShouldRun
                IsHealthy = [bool]$probeState.IsHealthy
                McpProcessId = $probeState.McpProcessId
                Reasons = @($probeState.Reasons)
                RepairInProgress = $true
                RepairReason = $reasonText
            }
            Write-JsonFile -Path $statePath -Value $probeState
            if ($probeState.IsHealthy) {
                $recovered = $true
                break
            }

            if ($launcher.HasExited -and $launcher.ExitCode -ne 0) {
                break
            }
        }

        if (-not $launcher.HasExited) {
            if ($recovered) {
                if (-not $launcher.WaitForExit(10000)) {
                    Write-GuardianLog -Level 'WARN' -Message ("repair launcher pid={0} stayed alive after health recovered; stopping it" -f $launcher.Id)
                    Stop-Process -Id $launcher.Id -Force -ErrorAction SilentlyContinue
                    Start-Sleep -Milliseconds 500
                }
            } else {
                Write-GuardianLog -Level 'WARN' -Message ("repair launcher pid={0} did not restore health before timeout; stopping it" -f $launcher.Id)
                Stop-Process -Id $launcher.Id -Force -ErrorAction SilentlyContinue
                Start-Sleep -Milliseconds 500
            }
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
            if (-not [string]::IsNullOrWhiteSpace($stdoutText)) {
                Write-GuardianLog -Level 'REPAIR' -Message ($stdoutText.Trim())
            }
            return $true
        }

        if ($null -eq $exitCode) {
            Write-GuardianLog -Level 'ERROR' -Message 'repair did not restore health before timeout'
        } elseif ($exitCode -eq 0) {
            Write-GuardianLog -Level 'ERROR' -Message 'repair process exited successfully but health was not restored'
        } else {
            Write-GuardianLog -Level 'ERROR' -Message ("repair failed with exit code {0}" -f $exitCode)
        }
        if (-not [string]::IsNullOrWhiteSpace($stdoutText)) {
            Write-GuardianLog -Level 'ERROR' -Message ($stdoutText.Trim())
        }
        if (-not [string]::IsNullOrWhiteSpace($stderrText)) {
            Write-GuardianLog -Level 'ERROR' -Message ($stderrText.Trim())
        }

        return $false
    }

    Set-Content -Path $guardianPidPath -Value $PID -Encoding ASCII
    Write-GuardianLog -Message ("guardian boot pid={0}" -f $PID)
    Write-JsonFile -Path $heartbeatPath -Value @{
        ObservedAtUtc = [DateTime]::UtcNow.ToString('o')
        GuardianPid = $PID
        DesiredShouldRun = $null
        IsHealthy = $null
        McpProcessId = $null
        Reasons = @('starting')
    }

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

        if (-not $state.NeedsRepair) {
            Start-Sleep -Seconds $PollSeconds
            continue
        }

        if ($unhealthyCount -lt $FailureThreshold) {
            Start-Sleep -Seconds $PollSeconds
            continue
        }

        $secondsSinceRepair = ([DateTime]::UtcNow - $lastRepairAt).TotalSeconds
        $effectiveRepairCooldownSeconds = Get-RepairCooldownForState -State $state
        if ($secondsSinceRepair -lt $effectiveRepairCooldownSeconds) {
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
