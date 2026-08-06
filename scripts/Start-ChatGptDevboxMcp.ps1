param(
    [switch]$Public,
    [switch]$OAuth,
    [switch]$RebuildRuntime,
    [switch]$TunnelOnly,
    [ValidateSet('auto', 'host', 'docker')]
    [string]$Runtime = 'auto'
)

$ErrorActionPreference = "Stop"
$ProgressPreference = 'SilentlyContinue'
$InformationPreference = 'SilentlyContinue'
$script:dockerExe = $null
$script:dockerConfiguredPath = $null

function Resolve-DockerExecutable {
    param([string]$ConfiguredPath)

    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($candidate in @(
            $ConfiguredPath,
            $env:DOCKER_EXE,
            $(if ($env:ProgramW6432) { Join-Path $env:ProgramW6432 "Docker\\Docker\\resources\\bin\\docker.exe" } else { $null }),
            $(if ($env:ProgramFiles) { Join-Path $env:ProgramFiles "Docker\\Docker\\resources\\bin\\docker.exe" } else { $null }),
            "C:\Program Files\Docker\Docker\resources\bin\docker.exe"
        )) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and -not $candidates.Contains($candidate)) {
            $candidates.Add($candidate)
        }
    }

    $dockerCommand = Get-Command docker -ErrorAction SilentlyContinue
    if ($dockerCommand -and -not [string]::IsNullOrWhiteSpace($dockerCommand.Source) -and -not $candidates.Contains($dockerCommand.Source)) {
        $candidates.Add([string]$dockerCommand.Source)
    }

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    throw "Docker CLI was not found. Install Docker Desktop or set DOCKER_EXE in .env to docker.exe."
}

function Get-DockerExecutable {
    if (-not $script:dockerExe) {
        $script:dockerExe = Resolve-DockerExecutable -ConfiguredPath $script:dockerConfiguredPath
    }

    return $script:dockerExe
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

function Test-IsCurrentProcessElevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-IsProcessElevated {
    param([Parameter(Mandatory = $true)][int]$ProcessId)

    $code = @'
using System;
using System.Runtime.InteropServices;
public static class DevboxStartElev {
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint a, bool b, int c);
  [DllImport("advapi32.dll", SetLastError=true)] public static extern bool OpenProcessToken(IntPtr p, uint a, out IntPtr t);
  [DllImport("advapi32.dll", SetLastError=true)] public static extern bool GetTokenInformation(IntPtr t, int c, IntPtr i, int l, out int r);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
'@
    if (-not ('DevboxStartElev' -as [type])) {
        Add-Type -TypeDefinition $code -ErrorAction Stop
    }

    $hProc = [DevboxStartElev]::OpenProcess(0x1000, $false, $ProcessId)
    if ($hProc -eq [IntPtr]::Zero) {
        return $false
    }

    $hTok = [IntPtr]::Zero
    try {
        if (-not [DevboxStartElev]::OpenProcessToken($hProc, 0x0008, [ref]$hTok)) {
            return $false
        }
        $len = 0
        [void][DevboxStartElev]::GetTokenInformation($hTok, 20, [IntPtr]::Zero, 0, [ref]$len)
        $ptr = [Runtime.InteropServices.Marshal]::AllocHGlobal($len)
        try {
            $ok = [DevboxStartElev]::GetTokenInformation($hTok, 20, $ptr, $len, [ref]$len)
            if (-not $ok) {
                return $false
            }
            return [Runtime.InteropServices.Marshal]::ReadInt32($ptr) -ne 0
        } finally {
            [Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
        }
    } finally {
        if ($hTok -ne [IntPtr]::Zero) {
            [void][DevboxStartElev]::CloseHandle($hTok)
        }
        [void][DevboxStartElev]::CloseHandle($hProc)
    }
}

function Ensure-WindowsHostStartIsElevated {
    param(
        [string]$SelectedRuntime,
        [string]$ProjectRoot,
        [int]$Port = 8100
    )

    if ($SelectedRuntime -ne 'host') {
        return
    }
    if (Test-IsCurrentProcessElevated) {
        return
    }

    $taskName = 'ChatGptDevboxMcp-ElevatedStart'
    $task = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    if (-not $task) {
        throw @"
Start-ChatGptDevboxMcp.ps1 must run elevated on Windows host mode so host_exec never pops UAC.
This process is not elevated, and scheduled task '$taskName' is missing.
Fix: re-run scripts\Install-ChatGptDevboxGuardian.ps1 from an elevated PowerShell, or start MCP only via elevated Guardian repair.
"@
    }

    Write-Host "Current shell is not elevated; re-launching MCP via scheduled task '$taskName' (RunLevel Highest, no UAC)..."
    Start-ScheduledTask -TaskName $taskName

    $localUrl = "http://127.0.0.1:$Port"
    for ($i = 0; $i -lt 45; $i++) {
        Start-Sleep -Seconds 2
        try {
            $response = Invoke-WebRequest -Uri "$localUrl/healthz" -UseBasicParsing -TimeoutSec 5
            if ($response.Content -match 'ok') {
                Write-Host "Elevated MCP is healthy at $localUrl"
                return
            }
        } catch {
        }
    }

    throw "Scheduled task '$taskName' was started but MCP did not become healthy on port $Port."
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

function Invoke-Docker {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$IgnoreExitCode,
        [int]$TimeoutSeconds = 120
    )

    $dockerExe = Get-DockerExecutable
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

        if ($timedOut -and $Arguments.Count -gt 1 -and $Arguments[0] -eq 'run' -and $Arguments -contains '-d' -and $text -match '(?m)^[0-9a-f]{12,64}$') {
            $timedOut = $false
            $exitCode = 0
        }
        if ($timedOut -and $Arguments.Count -gt 1 -and $Arguments[0] -eq 'start' -and $text.Trim()) {
            $timedOut = $false
            $exitCode = 0
        }
    } finally {
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

function Get-DockerLogsText {
    param([string]$ContainerName)

    return (Invoke-Docker -Arguments @('logs', $ContainerName) -IgnoreExitCode).Text
}

function Enter-ChatGptDevboxLifecycleMutex {
    $script:lifecycleMutex = New-Object System.Threading.Mutex($false, 'Global\ChatGptDevboxMcpLifecycle')
    $script:lifecycleMutexHeld = $script:lifecycleMutex.WaitOne(300000, $false)
    if (-not $script:lifecycleMutexHeld) {
        throw "Timed out waiting for another ChatGPT Devbox lifecycle action to finish."
    }
}

function Exit-ChatGptDevboxLifecycleMutex {
    if ($script:lifecycleMutexHeld) {
        $script:lifecycleMutex.ReleaseMutex()
        $script:lifecycleMutexHeld = $false
    }

    if ($script:lifecycleMutex) {
        $script:lifecycleMutex.Dispose()
        $script:lifecycleMutex = $null
    }
}

function Test-DockerEngine {
    param([int]$TimeoutSeconds = 45)

    $result = Invoke-Docker -Arguments @('version', '--format', '{{.Server.Version}}') -IgnoreExitCode -TimeoutSeconds $TimeoutSeconds
    return ($result.ExitCode -eq 0)
}

function Start-DockerDesktopIfNeeded {
    if (Test-DockerEngine -TimeoutSeconds 5) {
        return $true
    }

    $dockerDesktop = "C:\Program Files\Docker\Docker\Docker Desktop.exe"
    if (-not (Test-Path $dockerDesktop)) {
        throw "Docker Desktop is not installed at $dockerDesktop"
    }

    Write-Host "Docker engine is not ready. Starting Docker Desktop..."
    Start-Process -FilePath $dockerDesktop -WindowStyle Hidden | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(120)
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Seconds 2
        if (Test-DockerEngine -TimeoutSeconds 5) {
            return $true
        }
    }

    Write-Warning "Docker engine did not become ready within 120 seconds; starting MCP in degraded mode."
    return $false
}

function Get-EnvValue {
    param(
        [string]$FilePath,
        [string]$Name
    )

    $match = Select-String -Path $FilePath -Pattern ("^{0}=(.*)$" -f [regex]::Escape($Name)) -ErrorAction SilentlyContinue
    if (-not $match) {
        return ""
    }

    return $match.Matches[0].Groups[1].Value.Trim()
}

function Write-RuntimeEnvFile {
    param(
        [string]$SourceEnvFile,
        [string]$RuntimeEnvFile,
        [hashtable]$Overrides
    )

    $lines = Get-Content $SourceEnvFile
    $output = New-Object System.Collections.Generic.List[string]
    $seen = @{}

    foreach ($line in $lines) {
        if ($line -match '^\s*#' -or $line -notmatch '=') {
            $output.Add($line)
            continue
        }

        $name = $line.Substring(0, $line.IndexOf('='))
        if ($Overrides.ContainsKey($name)) {
            $output.Add("$name=$($Overrides[$name])")
            $seen[$name] = $true
        } else {
            $output.Add($line)
        }
    }

    foreach ($name in $Overrides.Keys) {
        if (-not $seen.ContainsKey($name)) {
            $output.Add("$name=$($Overrides[$name])")
        }
    }

    Set-Content -Path $RuntimeEnvFile -Value $output
}

function Get-CommandLineForPid {
    param([int]$ProcessId)

    $proc = Get-CimInstance Win32_Process -Filter "ProcessId=$ProcessId" -ErrorAction SilentlyContinue
    if (-not $proc) {
        return $null
    }

    return $proc.CommandLine
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

function Find-OwnedServerProcess {
    param([string]$PidFile)

    if (Test-Path $PidFile) {
        $pidText = (Get-Content $PidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
        if ($pidText -match '^\d+$') {
            $candidate = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f [int]$pidText) -ErrorAction SilentlyContinue
            if ($candidate -and (Test-IsOwnedServerCommandLine -CommandLine ([string]$candidate.CommandLine))) {
                return $candidate
            }
        }
    }

    return Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object { Test-IsOwnedServerCommandLine -CommandLine ([string]$_.CommandLine) } |
        Sort-Object CreationDate -Descending |
        Select-Object -First 1
}

function Stop-ExistingServerIfOwned {
    param(
        [string]$PidFile,
        [string]$ProjectRoot
    )

    $ownedProcess = Find-OwnedServerProcess -PidFile $PidFile
    if (-not $ownedProcess) {
        Remove-Item $PidFile -Force -ErrorAction SilentlyContinue
        return
    }

    $ownedPid = [int]$ownedProcess.ProcessId
    Stop-Process -Id $ownedPid -Force -ErrorAction SilentlyContinue
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 500
        $remaining = Get-CimInstance Win32_Process -Filter "ProcessId=$ownedPid" -ErrorAction SilentlyContinue
        if (-not $remaining) {
            break
        }
        if (-not (Test-IsOwnedServerCommandLine -CommandLine ([string]$remaining.CommandLine))) {
            break
        }
    }

    $stillOwned = Get-CimInstance Win32_Process -Filter "ProcessId=$ownedPid" -ErrorAction SilentlyContinue
    if ($stillOwned -and (Test-IsOwnedServerCommandLine -CommandLine ([string]$stillOwned.CommandLine))) {
        throw "Failed to stop owned MCP server PID $ownedPid. CommandLine: $($stillOwned.CommandLine)"
    }

    Remove-Item $PidFile -Force -ErrorAction SilentlyContinue
}

function Ensure-Directory {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
}

function Write-JsonStateFile {
    param(
        [string]$Path,
        [object]$Value
    )

    $directory = Split-Path -Parent $Path
    if ($directory) {
        Ensure-Directory -Path $directory
    }

    $json = $Value | ConvertTo-Json -Depth 8
    $encoding = [System.Text.UTF8Encoding]::new($false)
    $tempPath = Join-Path $directory ("{0}.{1}.tmp" -f [System.IO.Path]::GetFileName($Path), [System.Guid]::NewGuid().ToString('N'))
    $backupPath = $null

    try {
        [System.IO.File]::WriteAllText($tempPath, $json, $encoding)
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
    } finally {
        if ($tempPath -and (Test-Path $tempPath)) {
            Remove-Item -LiteralPath $tempPath -Force -ErrorAction SilentlyContinue
        }
        if ($backupPath -and (Test-Path $backupPath)) {
            Remove-Item -LiteralPath $backupPath -Force -ErrorAction SilentlyContinue
        }
    }
}

function Write-GuardianDesiredState {
    param(
        [string]$RunDir,
        [bool]$ShouldRun,
        [string]$Source
    )

    $statePath = Join-Path $RunDir "guardian.desired-state.json"
    Write-JsonStateFile -Path $statePath -Value @{
        ShouldRun = $ShouldRun
        UpdatedAtUtc = [DateTime]::UtcNow.ToString('o')
        Source = $Source
    }
}

function Write-GuardianSettings {
    param(
        [string]$RunDir,
        [hashtable]$Settings
    )

    $settingsPath = Join-Path $RunDir "guardian.settings.json"
    $payload = @{}
    foreach ($key in $Settings.Keys) {
        $payload[$key] = $Settings[$key]
    }
    $payload.UpdatedAtUtc = [DateTime]::UtcNow.ToString('o')
    $payload.Source = "Start-ChatGptDevboxMcp.ps1"

    Write-JsonStateFile -Path $settingsPath -Value $payload
}

function Resolve-NodeExecutable {
    param([string]$ConfiguredPath)

    if (-not [string]::IsNullOrWhiteSpace($ConfiguredPath)) {
        if (-not (Test-Path $ConfiguredPath)) {
            throw "Node executable not found at $ConfiguredPath"
        }

        return $ConfiguredPath
    }

    $programFilesNode = "C:\Program Files\nodejs\node.exe"
    if (Test-Path $programFilesNode) {
        return $programFilesNode
    }

    $nodeCommand = Get-Command node -ErrorAction SilentlyContinue
    if (-not $nodeCommand) {
        throw "Node.js was not found. Set NODE_EXE in .env or install Node.js."
    }

    $resolvedPath = $nodeCommand.Source
    if ($resolvedPath -like "*AppData\Roaming\npm\node*") {
        throw "Resolved node command points to an npm shim at $resolvedPath. Set NODE_EXE in .env to the real node.exe path."
    }

    return $resolvedPath
}

function Test-DockerObjectExists {
    param(
        [string]$Type,
        [string]$Name
    )

    $result = Invoke-Docker -Arguments @('inspect', '--type', $Type, $Name) -IgnoreExitCode
    if ($result.ExitCode -eq 0) {
        return $true
    }
    $text = [string]$result.Text
    if ($text -match 'No such (object|container|image)') {
        return $false
    }
    throw "docker inspect failed; refusing to create a conflicting object named $Name. Output:`n$text"
}

function Remove-DockerContainerIfPresent {
    param([string]$ContainerName)

    [void](Invoke-Docker -Arguments @('rm', '-f', $ContainerName) -IgnoreExitCode)
}

function Resolve-CloudflaredExecutable {
    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($candidate in @(
            $env:CLOUDFLARED_EXE,
            $(if ($env:ProgramFiles) { Join-Path ${env:ProgramFiles(x86)} 'cloudflared\cloudflared.exe' } else { $null }),
            'C:\Program Files (x86)\cloudflared\cloudflared.exe'
        )) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and -not $candidates.Contains($candidate)) {
            $candidates.Add($candidate)
        }
    }

    $cloudflaredCommand = Get-Command cloudflared -ErrorAction SilentlyContinue
    if ($cloudflaredCommand -and -not [string]::IsNullOrWhiteSpace($cloudflaredCommand.Source) -and -not $candidates.Contains($cloudflaredCommand.Source)) {
        $candidates.Add([string]$cloudflaredCommand.Source)
    }

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    throw "cloudflared.exe was not found. Install cloudflared or set CLOUDFLARED_EXE."
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

function Stop-ExistingHostCloudflared {
    param([string]$PidFile)

    if (-not (Test-Path $PidFile)) {
        return
    }

    $pidText = Get-Content -Path $PidFile -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($pidText -match '^\d+$') {
        $candidate = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f [int]$pidText) -ErrorAction SilentlyContinue
        if ($candidate -and (Test-IsOwnedHostCloudflaredCommandLine -CommandLine ([string]$candidate.CommandLine))) {
            Stop-Process -Id ([int]$candidate.ProcessId) -Force -ErrorAction SilentlyContinue
            for ($i = 0; $i -lt 20; $i++) {
                Start-Sleep -Milliseconds 250
                if (-not (Get-Process -Id ([int]$candidate.ProcessId) -ErrorAction SilentlyContinue)) {
                    break
                }
            }
        }
    }

    Remove-Item -LiteralPath $PidFile -Force -ErrorAction SilentlyContinue
}

function Get-DockerContainerRunningState {
    param([string]$ContainerName)

    $result = Invoke-Docker -Arguments @('inspect', '--type', 'container', $ContainerName, '--format', '{{.State.Running}}') -IgnoreExitCode
    if ($result.ExitCode -eq 0) {
        return $result.Text.Trim()
    }
    $text = [string]$result.Text
    if ($text -match 'No such (object|container)') {
        return $null
    }
    throw "docker inspect failed for $ContainerName. Output:`n$text"
}

function New-DevboxContainer {
    param(
        [string]$ContainerName,
        [string]$ImageName,
        [string]$HostWorkspace,
        [string]$DevboxWorkspace
    )

    $result = Invoke-Docker -Arguments @('run', '-d', '--name', $ContainerName, '--init', '-w', $DevboxWorkspace, '-v', "${HostWorkspace}:${DevboxWorkspace}", $ImageName, 'sleep', 'infinity') -IgnoreExitCode
    if ($result.ExitCode -eq 0) {
        Ensure-DockerContainerStarted -ContainerName $ContainerName
        return
    }

    $text = [string]$result.Text
    if ($text -match 'Conflict.*container name') {
        $running = Get-DockerContainerRunningState -ContainerName $ContainerName
        if ($null -ne $running) {
            $started = Invoke-Docker -Arguments @('start', $ContainerName) -IgnoreExitCode -TimeoutSeconds 30
            if ($started.ExitCode -eq 0) {
                return
            }
        }
    }
    throw "Failed to create devbox container $ContainerName. Output:`n$text"
}

function Ensure-DockerContainerStarted {
    param([string]$ContainerName)

    for ($i = 0; $i -lt 3; $i++) {
        $running = Get-DockerContainerRunningState -ContainerName $ContainerName
        if ($running -eq "true") {
            return
        }

        if ($null -eq $running) {
            throw "Docker container $ContainerName does not exist after creation."
        }

        [void](Invoke-Docker -Arguments @('start', $ContainerName) -IgnoreExitCode -TimeoutSeconds 30)
        Start-Sleep -Seconds 2
    }

    $finalRunning = Get-DockerContainerRunningState -ContainerName $ContainerName
    if ($finalRunning -ne "true") {
        throw "Docker container $ContainerName was created but did not reach running state."
    }
}

function Ensure-RuntimeImage {
    param(
        [string]$Root,
        [string]$ImageName,
        [switch]$ForceRebuild
    )

    $inspectResult = Invoke-Docker -Arguments @('image', 'inspect', $ImageName) -IgnoreExitCode
    $exists = ($inspectResult.ExitCode -eq 0)
    if ($exists -and -not $ForceRebuild) {
        return
    }

    [void](Invoke-Docker -Arguments @('build', '-f', (Join-Path $Root "runtime.Dockerfile"), '-t', $ImageName, $Root))
}

function Ensure-DevboxContainer {
    param(
        [string]$ContainerName,
        [string]$ImageName,
        [string]$HostWorkspace,
        [string]$DevboxWorkspace,
        [switch]$Recreate
    )

    $exists = Test-DockerObjectExists -Type container -Name $ContainerName
    if ($exists -and $Recreate) {
        Remove-DockerContainerIfPresent -ContainerName $ContainerName
        $exists = $false
    }

    if (-not $exists) {
        New-DevboxContainer -ContainerName $ContainerName -ImageName $ImageName -HostWorkspace $HostWorkspace -DevboxWorkspace $DevboxWorkspace
        return
    }

    $running = Get-DockerContainerRunningState -ContainerName $ContainerName
    if ($running.Trim() -ne "true") {
        $started = Invoke-Docker -Arguments @('start', $ContainerName) -IgnoreExitCode -TimeoutSeconds 30
        if ($started.ExitCode -ne 0) {
            Remove-DockerContainerIfPresent -ContainerName $ContainerName
            New-DevboxContainer -ContainerName $ContainerName -ImageName $ImageName -HostWorkspace $HostWorkspace -DevboxWorkspace $DevboxWorkspace
        } else {
            Ensure-DockerContainerStarted -ContainerName $ContainerName
        }
    }
}

function Start-CloudflaredQuickTunnel {
    param(
        [string]$ContainerName,
        [int]$Port
    )

    if (Test-DockerObjectExists -Type container -Name $ContainerName) {
        Remove-DockerContainerIfPresent -ContainerName $ContainerName
    }

    [void](Invoke-Docker -Arguments @('run', '-d', '--name', $ContainerName, 'cloudflare/cloudflared:latest', 'tunnel', '--no-autoupdate', '--url', "http://host.docker.internal:$Port"))
    Ensure-DockerContainerStarted -ContainerName $ContainerName

    for ($i = 0; $i -lt 120; $i++) {
        Start-Sleep -Seconds 2
        $logs = Get-DockerLogsText -ContainerName $ContainerName
        $match = [regex]::Match($logs, 'https://[a-z0-9-]+\.trycloudflare\.com')
        if ($match.Success) {
            return $match.Value.TrimEnd("/")
        }
    }

    $logs = Get-DockerLogsText -ContainerName $ContainerName
    throw "Cloudflare quick tunnel did not publish a URL. Logs:`n$logs"
}

function Normalize-PublicBaseUrl {
    param([string]$Value)

    $rawValue = if ($null -eq $Value) { "" } else { $Value }
    $trimmed = $rawValue.Trim().TrimEnd("/")
    if (-not $trimmed) {
        return ""
    }

    if ($trimmed -match '^https?://') {
        return $trimmed
    }

    return "https://$trimmed"
}

function Wait-ForHealthyPublicEndpoint {
    param(
        [string]$ContainerName,
        [string]$PublicBaseUrl,
        [string]$HostCloudflaredPidFile = ''
    )

    $healthUrl = "$($PublicBaseUrl.TrimEnd('/'))/healthz"
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Seconds 2
        if ($HostCloudflaredPidFile) {
            $hostPid = if (Test-Path $HostCloudflaredPidFile) { Get-Content $HostCloudflaredPidFile -ErrorAction SilentlyContinue | Select-Object -First 1 } else { '' }
            if ($hostPid -notmatch '^\d+$' -or -not (Get-Process -Id ([int]$hostPid) -ErrorAction SilentlyContinue)) {
                break
            }
        } else {
            $running = Get-DockerContainerRunningState -ContainerName $ContainerName
            if ($null -eq $running -or $running -ne "true") {
                break
            }
        }

        try {
            $response = Invoke-WebRequest -Uri $healthUrl -UseBasicParsing -TimeoutSec 5
            if ($response.Content -match "ok") {
                return
            }
        } catch {
        }
    }

    $logs = if ($HostCloudflaredPidFile) {
        $hostLog = Join-Path (Split-Path -Parent $HostCloudflaredPidFile) 'host-cloudflared.stderr.log'
        if (Test-Path $hostLog) { Get-Content $hostLog -Tail 120 | Out-String } else { 'host cloudflared log not found' }
    } else {
        Get-DockerLogsText -ContainerName $ContainerName
    }
    throw "The Cloudflare tunnel did not expose a healthy public endpoint at $healthUrl. Logs:`n$logs"
}

function Start-CloudflaredNamedTunnel {
    param(
        [string]$ContainerName,
        [string]$TunnelToken,
        [string]$PublicHostname,
        [int]$Port,
        [string]$RunDir,
        [bool]$DockerReady = $true
    )

    if (-not $TunnelToken) {
        throw "CLOUDFLARED_TUNNEL_TOKEN is required for the named Cloudflare tunnel."
    }

    $publicBaseUrl = Normalize-PublicBaseUrl -Value $PublicHostname
    if (-not $publicBaseUrl) {
        throw "CLOUDFLARED_PUBLIC_HOSTNAME is required for the named Cloudflare tunnel."
    }

    $cloudflaredExe = Resolve-CloudflaredExecutable
    $pidFile = Join-Path $RunDir 'host-cloudflared.pid'
    $tokenFile = Join-Path $RunDir 'host-cloudflared.tunnel-token.txt'
    $configFile = Join-Path $RunDir 'host-cloudflared-config.yml'
    $stdoutLog = Join-Path $RunDir 'host-cloudflared.stdout.log'
    $stderrLog = Join-Path $RunDir 'host-cloudflared.stderr.log'
    $metricsUrlFile = Join-Path $RunDir 'host-cloudflared.metrics-url.txt'
    $metricsUrl = if (-not [string]::IsNullOrWhiteSpace($env:CLOUDFLARED_METRICS_URL)) { [string]$env:CLOUDFLARED_METRICS_URL } else { 'http://127.0.0.1:20241/metrics' }
    $metricsUri = [Uri]$metricsUrl
    if ($metricsUri.Scheme -ne 'http' -or -not $metricsUri.IsLoopback -or $metricsUri.Port -lt 1) {
        throw "CLOUDFLARED_METRICS_URL must be a loopback http URL such as http://127.0.0.1:20241/metrics."
    }
    $metricsAddress = "{0}:{1}" -f $metricsUri.Host, $metricsUri.Port
    [System.IO.File]::WriteAllText($metricsUrlFile, $metricsUrl, [System.Text.UTF8Encoding]::new($false))

    Stop-ExistingHostCloudflared -PidFile $pidFile
    [System.IO.File]::WriteAllText($tokenFile, $TunnelToken, [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText($configFile, "url: http://127.0.0.1:$Port`nloglevel: info`n", [System.Text.UTF8Encoding]::new($false))
    if ($DockerReady -and (Test-DockerObjectExists -Type container -Name $ContainerName)) {
        Remove-DockerContainerIfPresent -ContainerName $ContainerName
    }

    if (Test-Path $stdoutLog) { Remove-Item $stdoutLog -Force -ErrorAction SilentlyContinue }
    if (Test-Path $stderrLog) { Remove-Item $stderrLog -Force -ErrorAction SilentlyContinue }

    $process = Start-Process -FilePath $cloudflaredExe `
        -ArgumentList @('tunnel', '--config', $configFile, '--no-autoupdate', '--loglevel', 'info', '--metrics', $metricsAddress, 'run', '--token-file', $tokenFile) `
        -WorkingDirectory $RunDir `
        -RedirectStandardOutput $stdoutLog `
        -RedirectStandardError $stderrLog `
        -PassThru `
        -WindowStyle Hidden

    Set-Content -Path $pidFile -Value $process.Id -Encoding ASCII

    Start-Sleep -Seconds 5
    $process.Refresh()
    if ($process.HasExited) {
        $stderr = if (Test-Path $stderrLog) { Get-Content $stderrLog -Raw } else { '' }
        throw "The named Cloudflare tunnel process exited early with code $($process.ExitCode). Logs:`n$stderr"
    }

    return $publicBaseUrl
}

function Start-CloudflaredPublicTunnel {
    param(
        [string]$ContainerName,
        [string]$TunnelToken,
        [string]$PublicHostname,
        [int]$Port,
        [ValidateSet('host', 'docker')]
        [string]$SelectedRuntime,
        [string]$RunDir,
        [bool]$DockerReady = $false
    )

    $hasToken = -not [string]::IsNullOrWhiteSpace($TunnelToken)
    $hasHostname = -not [string]::IsNullOrWhiteSpace($PublicHostname)

    if ($hasToken -or $hasHostname) {
        if (-not ($hasToken -and $hasHostname)) {
            throw "CLOUDFLARED_TUNNEL_TOKEN and CLOUDFLARED_PUBLIC_HOSTNAME must both be set to use the named Cloudflare tunnel."
        }

        return Start-CloudflaredNamedTunnel -ContainerName $ContainerName -TunnelToken $TunnelToken -PublicHostname $PublicHostname -Port $Port -RunDir $RunDir -DockerReady:$DockerReady
    }

    if ($SelectedRuntime -eq 'host') {
        throw 'Docker is required for Cloudflare quick tunnels. Configure a named tunnel for host mode.'
    }
    if (-not $DockerReady) {
        throw 'Docker is not ready for the configured Cloudflare quick tunnel.'
    }
    return Start-CloudflaredQuickTunnel -ContainerName $ContainerName -Port $Port
}

$root = Split-Path -Parent $PSScriptRoot
$envFile = Join-Path $root ".env"
$runtimeEnvFile = Join-Path $root ".env.runtime"
$runDir = Join-Path $root "run"
$pidFile = Join-Path $runDir "mcp.pid"
$stdoutLog = Join-Path $runDir "mcp.stdout.log"
$stderrLog = Join-Path $runDir "mcp.stderr.log"

if (-not (Test-Path $envFile)) {
    & (Join-Path $PSScriptRoot "Initialize-ChatGptDevboxMcp.ps1")
}

# Resolve runtime before taking the lifecycle mutex so an unelevated host-mode
# start can hand off to ChatGptDevboxMcp-ElevatedStart without deadlocking.
$envRuntimeEarly = (Get-EnvValue -FilePath $envFile -Name 'DEVBOX_RUNTIME_MODE').ToLowerInvariant()
$runtimeModeEarly = if ($PSBoundParameters.ContainsKey('Runtime')) {
    $Runtime.ToLowerInvariant()
} elseif ($envRuntimeEarly -in @('auto', 'host', 'docker')) {
    $envRuntimeEarly
} else {
    'auto'
}
$selectedRuntimeEarly = if ($runtimeModeEarly -eq 'auto') { 'docker' } else { $runtimeModeEarly }
$portValueEarly = Get-EnvValue -FilePath $envFile -Name "PORT"
$portEarly = if ([string]::IsNullOrWhiteSpace($portValueEarly)) { 8100 } else { [int]$portValueEarly }
if ($selectedRuntimeEarly -eq 'host') {
    Ensure-WindowsHostStartIsElevated -SelectedRuntime $selectedRuntimeEarly -ProjectRoot $root -Port $portEarly
    if (-not (Test-IsCurrentProcessElevated)) {
        # Elevated scheduled task owns the real start.
        exit 0
    }
}

Enter-ChatGptDevboxLifecycleMutex
try {
Ensure-Directory -Path $runDir
Write-GuardianDesiredState -RunDir $runDir -ShouldRun $true -Source "Start-ChatGptDevboxMcp.ps1"

$nodeExe = Resolve-NodeExecutable -ConfiguredPath (Get-EnvValue -FilePath $envFile -Name "NODE_EXE")
$envRuntime = (Get-EnvValue -FilePath $envFile -Name 'DEVBOX_RUNTIME_MODE').ToLowerInvariant()
$runtimeMode = if ($PSBoundParameters.ContainsKey('Runtime')) {
    $Runtime.ToLowerInvariant()
} elseif ($envRuntime -in @('auto', 'host', 'docker')) {
    $envRuntime
} else {
    'auto'
}
$selectedRuntime = if ($runtimeMode -eq 'auto') { 'docker' } else { $runtimeMode }
$script:dockerConfiguredPath = Get-EnvValue -FilePath $envFile -Name "DOCKER_EXE"
if ($selectedRuntime -eq 'docker') {
    [void](Get-DockerExecutable)
}

$portValue = Get-EnvValue -FilePath $envFile -Name "PORT"
$port = if ([string]::IsNullOrWhiteSpace($portValue)) { 8100 } else { [int]$portValue }
$configuredAuthMode = Get-EnvValue -FilePath $envFile -Name "MCP_AUTH_MODE"
$imageName = Get-EnvValue -FilePath $envFile -Name "DEVBOX_IMAGE_NAME"
if ([string]::IsNullOrWhiteSpace($imageName)) {
    $imageName = "chatgpt-devbox-runtime:local"
}
$containerName = Get-EnvValue -FilePath $envFile -Name "DEVBOX_CONTAINER_NAME"
if ([string]::IsNullOrWhiteSpace($containerName)) {
    $containerName = "chatgpt-devbox-runtime"
}
$hostWorkspace = Get-EnvValue -FilePath $envFile -Name "HOST_WORKSPACE_PATH"
if ([string]::IsNullOrWhiteSpace($hostWorkspace)) {
    $hostWorkspace = Join-Path $root "workspace"
}
$devboxWorkspace = Get-EnvValue -FilePath $envFile -Name "DEVBOX_WORKSPACE_PATH"
if ([string]::IsNullOrWhiteSpace($devboxWorkspace)) {
    $devboxWorkspace = "/workspace"
}
$cloudflaredContainerName = Get-EnvValue -FilePath $envFile -Name "CLOUDFLARED_CONTAINER_NAME"
if ([string]::IsNullOrWhiteSpace($cloudflaredContainerName)) {
    $cloudflaredContainerName = "chatgpt-devbox-cloudflared"
}
$cloudflaredTunnelToken = Get-EnvValue -FilePath $envFile -Name "CLOUDFLARED_TUNNEL_TOKEN"
$cloudflaredPublicHostname = Get-EnvValue -FilePath $envFile -Name "CLOUDFLARED_PUBLIC_HOSTNAME"
$configuredPublicBaseUrl = Normalize-PublicBaseUrl -Value (Get-EnvValue -FilePath $envFile -Name "PUBLIC_BASE_URL")
if (-not $configuredPublicBaseUrl) {
    $configuredPublicBaseUrl = Normalize-PublicBaseUrl -Value $cloudflaredPublicHostname
}

$dockerReady = $false
Ensure-Directory -Path $hostWorkspace
if ($selectedRuntime -eq 'docker') {
    if ($TunnelOnly) {
        $dockerReady = Test-DockerEngine -TimeoutSeconds 5
    } else {
        $dockerReady = Start-DockerDesktopIfNeeded
        if ($dockerReady) {
            Ensure-RuntimeImage -Root $root -ImageName $imageName -ForceRebuild:$RebuildRuntime
            Ensure-DevboxContainer -ContainerName $containerName -ImageName $imageName -HostWorkspace $hostWorkspace -DevboxWorkspace $devboxWorkspace -Recreate:$RebuildRuntime
        } else {
            Write-Warning "Skipping Docker image/container checks because Docker is not ready."
        }
    }
}

$publicBaseUrl = $configuredPublicBaseUrl
if ($TunnelOnly -and -not $Public) {
    throw '-TunnelOnly requires -Public.'
}
if ($Public) {
    $publicBaseUrl = Start-CloudflaredPublicTunnel -ContainerName $cloudflaredContainerName -TunnelToken $cloudflaredTunnelToken -PublicHostname $cloudflaredPublicHostname -Port $port -SelectedRuntime $selectedRuntime -RunDir $runDir -DockerReady:$dockerReady
}
if ($TunnelOnly) {
    $hostTunnelPidFile = if ($selectedRuntime -eq 'host') { Join-Path $runDir 'host-cloudflared.pid' } else { '' }
    Wait-ForHealthyPublicEndpoint -ContainerName $cloudflaredContainerName -PublicBaseUrl $publicBaseUrl -HostCloudflaredPidFile $hostTunnelPidFile
    Write-Host "Public MCP URL: $publicBaseUrl"
    Write-Host "Selected runtime: $selectedRuntime (tunnel-only repair)"
    return
}

if (-not $configuredAuthMode) {
    $configuredAuthMode = if ($OAuth -or $Public) { "demo-oauth" } else { "none" }
}

$authMode = if ($OAuth -or $Public) {
    if ($configuredAuthMode -eq "none") { "demo-oauth" } else { $configuredAuthMode }
} else {
    $configuredAuthMode
}

$effectivePublic = -not [string]::IsNullOrWhiteSpace($publicBaseUrl)
$effectiveOAuth = -not [string]::IsNullOrWhiteSpace($authMode) -and $authMode -ne "none"

Write-GuardianSettings -RunDir $runDir -Settings @{
    Public = [bool]$effectivePublic
    OAuth = [bool]$effectiveOAuth
    Port = $port
    DevboxContainerName = $containerName
    CloudflaredContainerName = $cloudflaredContainerName
    PublicBaseUrl = $publicBaseUrl
    AuthMode = $authMode
    RuntimeMode = $runtimeMode
    SelectedRuntime = $selectedRuntime
}

$overrides = @{
    "MCP_AUTH_MODE" = $authMode
    "PUBLIC_BASE_URL" = $publicBaseUrl
    "DEVBOX_RUNTIME_MODE" = $selectedRuntime
}
if ($selectedRuntime -eq 'host') {
    # Migrate the legacy Docker default so host tools do not try to use /workspace
    # as a Windows path. The source .env remains untouched.
    $overrides['DEVBOX_WORKSPACE_PATH'] = $hostWorkspace
    $configuredDevboxUser = Get-EnvValue -FilePath $envFile -Name 'DEVBOX_DEFAULT_USER'
    if (-not $configuredDevboxUser -or $configuredDevboxUser -eq 'root') {
        $overrides['DEVBOX_DEFAULT_USER'] = $env:USERNAME
    }
}

Write-RuntimeEnvFile -SourceEnvFile $envFile -RuntimeEnvFile $runtimeEnvFile -Overrides $overrides
Stop-ExistingServerIfOwned -PidFile $pidFile -ProjectRoot $root

if (Test-Path $stdoutLog) { Remove-Item $stdoutLog -Force -ErrorAction SilentlyContinue }
if (Test-Path $stderrLog) { Remove-Item $stderrLog -Force -ErrorAction SilentlyContinue }

$process = Start-Process -FilePath $nodeExe `
    -ArgumentList @("--env-file=.env.runtime", "src/server.js") `
    -WorkingDirectory $root `
    -RedirectStandardOutput $stdoutLog `
    -RedirectStandardError $stderrLog `
    -PassThru `
    -WindowStyle Hidden

$localUrl = "http://127.0.0.1:$port"
for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Seconds 2
    try {
        $response = Invoke-WebRequest -Uri "$localUrl/healthz" -UseBasicParsing -TimeoutSec 5
        if ($response.Content -match "ok") {
            break
        }
    } catch {
    }
}

try {
    $health = Invoke-WebRequest -Uri "$localUrl/healthz" -UseBasicParsing -TimeoutSec 5
    if ($health.Content -notmatch "ok") {
        throw "Health probe did not return ok."
    }
} catch {
    $stderr = if (Test-Path $stderrLog) { Get-Content $stderrLog -Raw } else { "" }
    throw "The MCP server failed to become healthy. stderr:`n$stderr"
}

$process.Refresh()
if ($process.HasExited) {
    $stderr = if (Test-Path $stderrLog) { Get-Content $stderrLog -Raw } else { "" }
    throw "The MCP server process exited before health validation completed. stderr:`n$stderr"
}

if ($selectedRuntime -eq 'host' -and -not (Test-IsProcessElevated -ProcessId ([int]$process.Id))) {
    try { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue } catch {}
    throw "MCP server PID $($process.Id) started without elevation. Host mode refuses medium-integrity MCP so host_exec cannot spam UAC. Start via elevated Guardian or ChatGptDevboxMcp-ElevatedStart."
}

Set-Content -Path $pidFile -Value $process.Id

if ($Public) {
    $hostTunnelPidFile = if ($selectedRuntime -eq 'host') { Join-Path $runDir 'host-cloudflared.pid' } else { '' }
    Wait-ForHealthyPublicEndpoint -ContainerName $cloudflaredContainerName -PublicBaseUrl $publicBaseUrl -HostCloudflaredPidFile $hostTunnelPidFile
}

Write-Host "Local MCP URL: $localUrl"
if ($Public) {
    Write-Host "Public MCP URL: $publicBaseUrl"
    Write-Host "Legacy MCP URL: $publicBaseUrl/mcp"
}
Write-Host "Authentication mode: $(if ($authMode -eq 'none') { 'No Authentication' } else { 'OAuth' })"
Write-Host "Selected runtime: $selectedRuntime (requested: $runtimeMode)"
} finally {
    Exit-ChatGptDevboxLifecycleMutex
}
