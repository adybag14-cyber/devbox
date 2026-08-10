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
$script:lifecycleMutex = $null
$script:lifecycleMutexHeld = $false
$script:startupStatePath = $null
$script:startupAttemptId = $null
$script:startupStartedAtUtc = $null
$script:startupDeadlineUtc = $null
$script:startupMcpPid = $null
$script:startupSucceeded = $false

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

function Write-StartupPhase {
    param(
        [Parameter(Mandatory = $true)][string]$Phase,
        [ValidateSet('running', 'ready', 'failed', 'timed-out')]
        [string]$Status = 'running',
        [string]$Details = '',
        [hashtable]$Extra = @{}
    )

    if (-not $script:startupStatePath) {
        return
    }

    $payload = [ordered]@{
        AttemptId = $script:startupAttemptId
        ProcessId = $PID
        Phase = $Phase
        Status = $Status
        StartedAtUtc = $script:startupStartedAtUtc
        UpdatedAtUtc = [DateTime]::UtcNow.ToString('o')
        DeadlineUtc = if ($script:startupDeadlineUtc) { $script:startupDeadlineUtc.ToString('o') } else { $null }
        Details = $Details
    }
    foreach ($key in $Extra.Keys) {
        $payload[$key] = $Extra[$key]
    }
    Write-JsonStateFile -Path $script:startupStatePath -Value $payload
}

function Assert-StartupDeadline {
    param([string]$Phase = 'startup')

    if ($script:startupDeadlineUtc -and [DateTime]::UtcNow -gt $script:startupDeadlineUtc) {
        $details = "Startup deadline exceeded while in phase '$Phase'."
        Write-StartupPhase -Phase $Phase -Status 'timed-out' -Details $details
        throw $details
    }
}

function Enter-ChatGptDevboxLifecycleMutex {
    param(
        [Parameter(Mandatory = $true)][string]$RunDir,
        [Parameter(Mandatory = $true)][string]$SelectedRuntime,
        [int]$TimeoutSeconds = 180
    )

    Ensure-Directory -Path $RunDir
    $script:startupStatePath = Join-Path $RunDir 'startup-state.json'
    $script:startupAttemptId = [System.Guid]::NewGuid().ToString('N')
    $script:startupStartedAtUtc = [DateTime]::UtcNow.ToString('o')
    $script:startupDeadlineUtc = [DateTime]::UtcNow.AddSeconds([Math]::Max(30, $TimeoutSeconds))
    $script:startupSucceeded = $false
    $script:startupMcpPid = $null

    $script:lifecycleMutex = New-Object System.Threading.Mutex($false, 'Global\ChatGptDevboxMcpLifecycle')
    try {
        $script:lifecycleMutexHeld = $script:lifecycleMutex.WaitOne(0, $false)
    } catch [System.Threading.AbandonedMutexException] {
        $script:lifecycleMutexHeld = $true
    }

    if (-not $script:lifecycleMutexHeld) {
        $ownerSummary = ''
        if (Test-Path $script:startupStatePath) {
            try {
                $owner = Get-Content $script:startupStatePath -Raw | ConvertFrom-Json
                $ownerSummary = " Existing owner PID=$($owner.ProcessId), phase=$($owner.Phase), status=$($owner.Status), updated=$($owner.UpdatedAtUtc)."
            } catch {
            }
        }
        $script:lifecycleMutex.Dispose()
        $script:lifecycleMutex = $null
        throw "Another ChatGPT Devbox lifecycle action is already running; refusing to queue a concurrent start.$ownerSummary"
    }

    Write-StartupPhase -Phase 'lifecycle-lock-acquired' -Extra @{
        SelectedRuntime = $SelectedRuntime
        TimeoutSeconds = [Math]::Max(30, $TimeoutSeconds)
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

function Read-RuntimeEnvValues {
    param([Parameter(Mandatory = $true)][string]$FilePath)

    $values = @{}
    foreach ($rawLine in (Get-Content -LiteralPath $FilePath -ErrorAction Stop)) {
        $line = [string]$rawLine
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $trimmed = $line.Trim()
        if ($trimmed.StartsWith('#')) { continue }
        if ($trimmed.StartsWith('export ')) { $trimmed = $trimmed.Substring(7).TrimStart() }
        $equals = $trimmed.IndexOf('=')
        if ($equals -le 0) { continue }
        $name = $trimmed.Substring(0, $equals).Trim()
        if ($name -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') { continue }
        $value = $trimmed.Substring($equals + 1).Trim()
        if ($value.Length -ge 2) {
            $first = $value[0]
            $last = $value[$value.Length - 1]
            if (($first -eq '"' -and $last -eq '"') -or ($first -eq "'" -and $last -eq "'")) {
                $value = $value.Substring(1, $value.Length - 2)
            }
        }
        $values[$name] = $value
    }
    return $values
}

function Set-TemporaryProcessEnvironment {
    param([Parameter(Mandatory = $true)][hashtable]$Values)

    $previous = @{}
    foreach ($name in $Values.Keys) {
        $existing = [Environment]::GetEnvironmentVariable([string]$name, 'Process')
        $previous[$name] = [pscustomobject]@{
            Existed = $null -ne $existing
            Value = $existing
        }
        [Environment]::SetEnvironmentVariable([string]$name, [string]$Values[$name], 'Process')
    }
    return $previous
}

function Restore-TemporaryProcessEnvironment {
    param([Parameter(Mandatory = $true)][hashtable]$Previous)

    foreach ($name in $Previous.Keys) {
        $entry = $Previous[$name]
        $value = if ($entry.Existed) { [string]$entry.Value } else { $null }
        [Environment]::SetEnvironmentVariable([string]$name, $value, 'Process')
    }
}

function Get-CommandLineForPid {
    param([int]$ProcessId)

    $proc = Get-CimInstance Win32_Process -Filter "ProcessId=$ProcessId" -ErrorAction SilentlyContinue
    if (-not $proc) {
        return $null
    }

    return $proc.CommandLine
}

function Split-WindowsCommandLine {
    param([Parameter(Mandatory = $true)][string]$CommandLine)

    if (-not ('DevboxMcpCommandLineNativeMethods' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class DevboxMcpCommandLineNativeMethods {
    [DllImport("shell32.dll", SetLastError = true)]
    private static extern IntPtr CommandLineToArgvW(
        [MarshalAs(UnmanagedType.LPWStr)] string commandLine,
        out int argumentCount
    );

    [DllImport("kernel32.dll")]
    private static extern IntPtr LocalFree(IntPtr memory);

    public static string[] Split(string commandLine) {
        int count;
        IntPtr argv = CommandLineToArgvW(commandLine, out count);
        if (argv == IntPtr.Zero) {
            return Array.Empty<string>();
        }
        try {
            var arguments = new List<string>(count);
            for (int i = 0; i < count; i++) {
                IntPtr value = Marshal.ReadIntPtr(argv, i * IntPtr.Size);
                arguments.Add(Marshal.PtrToStringUni(value) ?? string.Empty);
            }
            return arguments.ToArray();
        } finally {
            LocalFree(argv);
        }
    }
}
'@
    }

    return @([DevboxMcpCommandLineNativeMethods]::Split($CommandLine))
}

function Resolve-McpComparablePath {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $null
    }
    try {
        $resolved = (Get-Item -LiteralPath $Path -Force -ErrorAction Stop).FullName
    } catch {
        try {
            $resolved = [IO.Path]::GetFullPath($Path)
        } catch {
            return $null
        }
    }
    return ([string]$resolved).TrimEnd('\').Replace('/', '\').ToLowerInvariant()
}

function Test-IsOwnedServerCommandLine {
    param(
        [string]$CommandLine,
        [string]$ProjectRoot
    )

    if ([string]::IsNullOrWhiteSpace($CommandLine)) {
        return $false
    }

    $text = ([string]$CommandLine).Replace('/', '\').ToLowerInvariant()
    if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
        $isLegacyJs = $text.Contains('src\server.js') -and $text.Contains('.env.runtime')
        $isRust = $text.Contains('rust-mcp\target\release\devbox-mcp.exe')
        return ($isLegacyJs -or $isRust)
    }

    $normalizedRoot = Resolve-McpComparablePath -Path $ProjectRoot
    if ([string]::IsNullOrWhiteSpace($normalizedRoot)) {
        return $false
    }
    $expectedServer = "$normalizedRoot\src\server.js"
    $expectedRuntimeEnv = "$normalizedRoot\.env.runtime"
    $expectedRust = "$normalizedRoot\rust-mcp\target\release\devbox-mcp.exe"
    $arguments = @(Split-WindowsCommandLine -CommandLine ([string]$CommandLine))
    if ($arguments.Count -eq 0) {
        return $false
    }

    $executable = Resolve-McpComparablePath -Path ([string]$arguments[0])
    if ($executable -eq $expectedRust) {
        return $true
    }

    $serverMatches = $false
    $runtimeEnvMatches = $false
    for ($i = 1; $i -lt $arguments.Count; $i++) {
        $argument = [string]$arguments[$i]
        if ($argument.StartsWith('--env-file=', [StringComparison]::OrdinalIgnoreCase)) {
            $runtimePath = $argument.Substring('--env-file='.Length)
            $runtimeEnvMatches = (Resolve-McpComparablePath -Path $runtimePath) -eq $expectedRuntimeEnv
            continue
        }
        if ($argument.Equals('--env-file', [StringComparison]::OrdinalIgnoreCase) -and ($i + 1) -lt $arguments.Count) {
            $i++
            $runtimeEnvMatches = (Resolve-McpComparablePath -Path ([string]$arguments[$i])) -eq $expectedRuntimeEnv
            continue
        }
        if ((Resolve-McpComparablePath -Path $argument) -eq $expectedServer) {
            $serverMatches = $true
        }
    }
    return ($serverMatches -and $runtimeEnvMatches)
}

function Find-OwnedServerProcess {
    param(
        [string]$PidFile,
        [string]$ProjectRoot
    )

    if (Test-Path $PidFile) {
        $pidText = (Get-Content $PidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
        if ($pidText -match '^\d+$') {
            $candidate = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f [int]$pidText) -ErrorAction SilentlyContinue
            # A checkout-local PID file is the ownership authority for legacy
            # relative JS launches. The command still has to be an MCP shape.
            if ($candidate -and (Test-IsOwnedServerCommandLine -CommandLine ([string]$candidate.CommandLine))) {
                return $candidate
            }
        }
    }

    # Missing/stale PID metadata must never let one worktree claim another
    # checkout's relative `src/server.js` process. Recovery discovery is only
    # allowed for launch command lines that contain this checkout's absolute root.
    return Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object { Test-IsOwnedServerCommandLine -CommandLine ([string]$_.CommandLine) -ProjectRoot $ProjectRoot } |
        Sort-Object CreationDate -Descending |
        Select-Object -First 1
}

function Stop-ExistingServerIfOwned {
    param(
        [string]$PidFile,
        [string]$ProjectRoot
    )

    $ownedProcess = Find-OwnedServerProcess -PidFile $PidFile -ProjectRoot $ProjectRoot
    if (-not $ownedProcess) {
        Remove-Item $PidFile -Force -ErrorAction SilentlyContinue
        return
    }

    $ownedPid = [int]$ownedProcess.ProcessId
    Assert-StartupDeadline -Phase 'stopping-existing-mcp'
    Stop-Process -Id $ownedPid -Force -ErrorAction SilentlyContinue

    # Poll the cheap process table first. Repeated Win32_Process/CIM queries can
    # themselves become very slow when the Windows/Hyper-V host is under load.
    for ($i = 0; $i -lt 16; $i++) {
        Assert-StartupDeadline -Phase 'stopping-existing-mcp'
        Start-Sleep -Milliseconds 250
        if (-not (Get-Process -Id $ownedPid -ErrorAction SilentlyContinue)) {
            Remove-Item $PidFile -Force -ErrorAction SilentlyContinue
            return
        }
    }

    # PID still exists after four seconds. Re-check ownership before issuing one
    # final force-stop so PID reuse can never cause an unrelated process kill.
    $stillOwned = Get-CimInstance Win32_Process -Filter "ProcessId=$ownedPid" -ErrorAction SilentlyContinue
    if ($stillOwned -and (Test-IsOwnedServerCommandLine -CommandLine ([string]$stillOwned.CommandLine))) {
        Assert-StartupDeadline -Phase 'stopping-existing-mcp'
        Stop-Process -Id $ownedPid -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500
    }

    $finalProcess = Get-CimInstance Win32_Process -Filter "ProcessId=$ownedPid" -ErrorAction SilentlyContinue
    if ($finalProcess -and (Test-IsOwnedServerCommandLine -CommandLine ([string]$finalProcess.CommandLine))) {
        throw "Failed to stop owned MCP server PID $ownedPid within the bounded stop window. CommandLine: $($finalProcess.CommandLine)"
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

function Resolve-McpImplementation {
    param([string]$ConfiguredValue)

    $value = if ([string]::IsNullOrWhiteSpace($ConfiguredValue)) { 'rust' } else { $ConfiguredValue.Trim().ToLowerInvariant() }
    if ($value -notin @('rust', 'js')) {
        throw "Invalid DEVBOX_MCP_IMPLEMENTATION=$ConfiguredValue. Expected rust or js."
    }
    return $value
}

function Resolve-CargoExecutable {
    param([string]$ConfiguredPath)

    if (-not [string]::IsNullOrWhiteSpace($ConfiguredPath)) {
        if (Test-Path $ConfiguredPath) { return (Resolve-Path $ConfiguredPath).Path }
        $configuredCommand = Get-Command $ConfiguredPath -ErrorAction SilentlyContinue
        if ($configuredCommand -and $configuredCommand.Source) { return [string]$configuredCommand.Source }
        throw "Cargo executable not found at or on PATH as '$ConfiguredPath'."
    }

    $candidates = @(
        $(if ($env:USERPROFILE) { Join-Path $env:USERPROFILE '.cargo\bin\cargo.exe' } else { $null }),
        'cargo.exe',
        'cargo'
    )
    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        if (Test-Path $candidate) { return (Resolve-Path $candidate).Path }
        $command = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($command -and $command.Source) { return [string]$command.Source }
    }
    throw 'Rust/Cargo was not found. Install Rust/Cargo or set CARGO_EXE. Temporary rollback: DEVBOX_MCP_IMPLEMENTATION=js.'
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

function Assert-McpReplacementReady {
    param(
        [Parameter(Mandatory = $true)][string]$Implementation,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$RuntimeEnvFile
    )

    if ($Implementation -eq 'rust') {
        $configuredCargo = if (-not [string]::IsNullOrWhiteSpace($env:CARGO_EXE)) {
            $env:CARGO_EXE
        } else {
            Get-EnvValue -FilePath (Join-Path $ProjectRoot '.env') -Name 'CARGO_EXE'
        }
        $cargoExe = Resolve-CargoExecutable -ConfiguredPath $configuredCargo
        $manifestPath = Join-Path $ProjectRoot 'rust-mcp\Cargo.toml'
        $binaryPath = Join-Path $ProjectRoot 'rust-mcp\target\release\devbox-mcp.exe'
        Push-Location $ProjectRoot
        try {
            $buildOutput = & $cargoExe 'build' '--manifest-path' $manifestPath '--release' '--locked' 2>&1 | Out-String
            if ($LASTEXITCODE -ne 0) {
                throw "Rust MCP release preflight failed before the existing MCP was stopped. Output:`n$buildOutput`nTemporary rollback: DEVBOX_MCP_IMPLEMENTATION=js."
            }
            if (-not (Test-Path $binaryPath)) {
                throw "Rust MCP release build succeeded but the binary was not found at $binaryPath. The existing MCP was not stopped."
            }
            $parityOutput = & $binaryPath '--parity-report' 2>&1 | Out-String
            if ($LASTEXITCODE -ne 0) {
                throw "Rust MCP binary preflight failed before the existing MCP was stopped. Output:`n$parityOutput"
            }
        } finally {
            Pop-Location
        }
        return [pscustomobject]@{
            Implementation = 'rust'
            FilePath = $binaryPath
            ArgumentList = @()
        }
    }

    $nodeExe = Resolve-NodeExecutable -ConfiguredPath (Get-EnvValue -FilePath (Join-Path $ProjectRoot '.env') -Name 'NODE_EXE')
    $serverPath = Join-Path $ProjectRoot 'src/server.js'
    $syntaxOutput = & $nodeExe '--check' $serverPath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "MCP rollback preflight failed JavaScript syntax validation. The existing MCP was not stopped. Output:`n$syntaxOutput"
    }

    $dependencyProbe = @'
const dependencies = ['express', '@modelcontextprotocol/sdk/server/mcp.js', 'zod/v4', 'jose'];
for (const dependency of dependencies) await import(dependency);
'@
    Push-Location $ProjectRoot
    try {
        $dependencyOutput = & $nodeExe ("--env-file={0}" -f $RuntimeEnvFile) '--input-type=module' '-e' $dependencyProbe 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) {
            throw "MCP rollback dependency preflight failed. The existing MCP was not stopped. Restore the locked dependencies with npm ci. Output:`n$dependencyOutput"
        }
    } finally {
        Pop-Location
    }
    return [pscustomobject]@{
        Implementation = 'js'
        FilePath = $nodeExe
        ArgumentList = @(("--env-file={0}" -f $RuntimeEnvFile), $serverPath)
    }
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
        Assert-StartupDeadline -Phase 'waiting-public-health'
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

function Resolve-DefaultRouteIPv4BindAddress {
    try {
        $routes = @(Get-NetRoute -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0' -ErrorAction Stop |
            Sort-Object -Property @{ Expression = {
                $interfaceMetric = 0
                try {
                    $ipInterface = Get-NetIPInterface -InterfaceIndex $_.InterfaceIndex -AddressFamily IPv4 -ErrorAction Stop
                    $interfaceMetric = [int]$ipInterface.InterfaceMetric
                } catch {
                }
                [int]$_.RouteMetric + $interfaceMetric
            } })
        foreach ($route in $routes) {
            $adapter = Get-NetAdapter -InterfaceIndex $route.InterfaceIndex -ErrorAction SilentlyContinue
            if (-not $adapter -or $adapter.Status -ne 'Up' -or $adapter.HardwareInterface -ne $true) {
                continue
            }
            $address = Get-NetIPAddress -InterfaceIndex $route.InterfaceIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
                Where-Object {
                    $_.IPAddress -and
                    $_.IPAddress -notmatch '^127\.' -and
                    $_.IPAddress -notmatch '^169\.254\.' -and
                    ($_.AddressState -eq 'Preferred' -or $null -eq $_.AddressState)
                } |
                Select-Object -First 1
            if ($address) {
                return [pscustomobject]@{
                    Address = [string]$address.IPAddress
                    InterfaceAlias = [string]$adapter.Name
                    InterfaceIndex = [int]$route.InterfaceIndex
                    PrefixOrigin = [string]$address.PrefixOrigin
                    RouteMetric = [int]$route.RouteMetric
                }
            }
        }
    } catch {
    }
    return $null
}

function Start-CloudflaredNamedTunnel {
    param(
        [string]$ContainerName,
        [string]$TunnelToken,
        [string]$PublicHostname,
        [int]$Port,
        [string]$RunDir,
        [string]$MetricsUrl = '',
        [string]$EdgeIpVersion = '',
        [string]$TransportProtocol = '',
        [string]$EdgeBindAddress = '',
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
    $metricsUrl = if (-not [string]::IsNullOrWhiteSpace($MetricsUrl)) {
        [string]$MetricsUrl
    } elseif (-not [string]::IsNullOrWhiteSpace($env:CLOUDFLARED_METRICS_URL)) {
        [string]$env:CLOUDFLARED_METRICS_URL
    } else {
        'http://127.0.0.1:20241/metrics'
    }
    $metricsUri = [Uri]$metricsUrl
    if ($metricsUri.Scheme -ne 'http' -or -not $metricsUri.IsLoopback -or $metricsUri.Port -lt 1) {
        throw "CLOUDFLARED_METRICS_URL must be a loopback http URL such as http://127.0.0.1:20241/metrics."
    }
    $metricsAddress = "{0}:{1}" -f $metricsUri.Host, $metricsUri.Port
    [System.IO.File]::WriteAllText($metricsUrlFile, $metricsUrl, [System.Text.UTF8Encoding]::new($false))
    $edgeIpVersion = if (-not [string]::IsNullOrWhiteSpace($EdgeIpVersion)) {
        [string]$EdgeIpVersion
    } elseif (-not [string]::IsNullOrWhiteSpace($env:CLOUDFLARED_EDGE_IP_VERSION)) {
        [string]$env:CLOUDFLARED_EDGE_IP_VERSION
    } else {
        'auto'
    }
    $edgeIpVersion = $edgeIpVersion.Trim().ToLowerInvariant()
    if ($edgeIpVersion -notin @('auto', '4', '6')) {
        throw "CLOUDFLARED_EDGE_IP_VERSION must be one of: auto, 4, 6."
    }

    $transportProtocol = if (-not [string]::IsNullOrWhiteSpace($TransportProtocol)) {
        [string]$TransportProtocol
    } elseif (-not [string]::IsNullOrWhiteSpace($env:TUNNEL_TRANSPORT_PROTOCOL)) {
        [string]$env:TUNNEL_TRANSPORT_PROTOCOL
    } else {
        'auto'
    }
    $transportProtocol = $transportProtocol.Trim().ToLowerInvariant()
    if ($transportProtocol -notin @('auto', 'quic', 'http2')) {
        throw "CLOUDFLARED_TRANSPORT_PROTOCOL must be one of: auto, quic, http2."
    }

    $requestedEdgeBindAddress = if (-not [string]::IsNullOrWhiteSpace($EdgeBindAddress)) {
        [string]$EdgeBindAddress
    } elseif (-not [string]::IsNullOrWhiteSpace($env:TUNNEL_EDGE_BIND_ADDRESS)) {
        [string]$env:TUNNEL_EDGE_BIND_ADDRESS
    } else {
        'auto'
    }
    $requestedEdgeBindAddress = $requestedEdgeBindAddress.Trim()
    $effectiveEdgeBindAddress = ''
    $bindResolution = 'unbound'
    $bindInterfaceAlias = $null
    $bindInterfaceIndex = $null
    $bindPrefixOrigin = $null
    $shouldResolveDefaultRoute = [string]::IsNullOrWhiteSpace($requestedEdgeBindAddress) -or $requestedEdgeBindAddress -eq 'auto'

    if (-not $shouldResolveDefaultRoute) {
        $parsedBindAddress = $null
        if (-not [System.Net.IPAddress]::TryParse($requestedEdgeBindAddress, [ref]$parsedBindAddress)) {
            throw "CLOUDFLARED_EDGE_BIND_ADDRESS must be 'auto' or a valid local IP address."
        }
        try {
            $localBind = Get-NetIPAddress -IPAddress $requestedEdgeBindAddress -ErrorAction Stop | Select-Object -First 1
            if ($localBind) {
                $effectiveEdgeBindAddress = $requestedEdgeBindAddress
                $bindResolution = 'configured-local-address'
                $bindInterfaceAlias = [string]$localBind.InterfaceAlias
                $bindInterfaceIndex = [int]$localBind.InterfaceIndex
                $bindPrefixOrigin = [string]$localBind.PrefixOrigin
            } else {
                Write-Warning "Configured Cloudflare edge bind address $requestedEdgeBindAddress is not currently assigned locally; resolving the active physical default-route IPv4 address instead."
                $shouldResolveDefaultRoute = $true
                $bindResolution = 'configured-address-stale'
            }
        } catch {
            # If adapter inspection itself fails, preserve the explicit value and
            # let cloudflared validate it rather than silently changing routing.
            $effectiveEdgeBindAddress = $requestedEdgeBindAddress
            $bindResolution = 'configured-unverified'
        }
    }

    if ($shouldResolveDefaultRoute -and $edgeIpVersion -ne '6') {
        $resolvedBind = Resolve-DefaultRouteIPv4BindAddress
        if ($resolvedBind) {
            $effectiveEdgeBindAddress = [string]$resolvedBind.Address
            $bindResolution = if ($bindResolution -eq 'configured-address-stale') { 'configured-stale-default-route' } else { 'auto-default-route' }
            $bindInterfaceAlias = [string]$resolvedBind.InterfaceAlias
            $bindInterfaceIndex = [int]$resolvedBind.InterfaceIndex
            $bindPrefixOrigin = [string]$resolvedBind.PrefixOrigin
        } else {
            $effectiveEdgeBindAddress = ''
            $bindResolution = 'auto-unbound-fallback'
            Write-Warning 'Could not resolve an active physical default-route IPv4 address; cloudflared will use normal Windows route selection.'
        }
    } elseif ($shouldResolveDefaultRoute -and $edgeIpVersion -eq '6') {
        $bindResolution = 'ipv6-unbound'
    }

    $transportStateFile = Join-Path $RunDir 'host-cloudflared.transport.json'
    $transportState = [ordered]@{
        EdgeIpVersion = $edgeIpVersion
        ProtocolSelection = $transportProtocol
        RequestedEdgeBindAddress = $requestedEdgeBindAddress
        EffectiveEdgeBindAddress = $effectiveEdgeBindAddress
        BindResolution = $bindResolution
        BindInterfaceAlias = $bindInterfaceAlias
        BindInterfaceIndex = $bindInterfaceIndex
        BindPrefixOrigin = $bindPrefixOrigin
        MetricsUrl = $metricsUrl
        UpdatedAtUtc = [DateTime]::UtcNow.ToString('o')
    } | ConvertTo-Json -Depth 4
    [System.IO.File]::WriteAllText($transportStateFile, $transportState, [System.Text.UTF8Encoding]::new($false))

    Stop-ExistingHostCloudflared -PidFile $pidFile
    [System.IO.File]::WriteAllText($tokenFile, $TunnelToken, [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText($configFile, "url: http://127.0.0.1:$Port`nloglevel: info`n", [System.Text.UTF8Encoding]::new($false))
    if ($DockerReady -and (Test-DockerObjectExists -Type container -Name $ContainerName)) {
        Remove-DockerContainerIfPresent -ContainerName $ContainerName
    }

    if (Test-Path $stdoutLog) { Remove-Item $stdoutLog -Force -ErrorAction SilentlyContinue }
    if (Test-Path $stderrLog) { Remove-Item $stderrLog -Force -ErrorAction SilentlyContinue }

    $cloudflaredArgs = @('tunnel', '--config', $configFile, '--no-autoupdate', '--loglevel', 'info', '--metrics', $metricsAddress, '--edge-ip-version', $edgeIpVersion)
    if ($transportProtocol -ne 'auto') {
        $cloudflaredArgs += @('--protocol', $transportProtocol)
    }
    if ($effectiveEdgeBindAddress) {
        $cloudflaredArgs += @('--edge-bind-address', $effectiveEdgeBindAddress)
    }
    $cloudflaredArgs += @('run', '--token-file', $tokenFile)

    $process = Start-Process -FilePath $cloudflaredExe `
        -ArgumentList $cloudflaredArgs `
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
        [string]$MetricsUrl = '',
        [string]$EdgeIpVersion = '',
        [string]$TransportProtocol = '',
        [string]$EdgeBindAddress = '',
        [bool]$DockerReady = $false
    )

    $hasToken = -not [string]::IsNullOrWhiteSpace($TunnelToken)
    $hasHostname = -not [string]::IsNullOrWhiteSpace($PublicHostname)

    if ($hasToken -or $hasHostname) {
        if (-not ($hasToken -and $hasHostname)) {
            throw "CLOUDFLARED_TUNNEL_TOKEN and CLOUDFLARED_PUBLIC_HOSTNAME must both be set to use the named Cloudflare tunnel."
        }

        return Start-CloudflaredNamedTunnel -ContainerName $ContainerName -TunnelToken $TunnelToken -PublicHostname $PublicHostname -Port $Port -RunDir $RunDir -MetricsUrl $MetricsUrl -EdgeIpVersion $EdgeIpVersion -TransportProtocol $TransportProtocol -EdgeBindAddress $EdgeBindAddress -DockerReady:$DockerReady
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
$implementationFile = Join-Path $runDir "mcp.implementation"
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

$startupTimeoutRaw = Get-EnvValue -FilePath $envFile -Name 'DEVBOX_STARTUP_TIMEOUT_SECONDS'
$startupTimeoutSeconds = if ($startupTimeoutRaw -match '^\d+$') {
    [Math]::Min(1800, [Math]::Max(30, [int]$startupTimeoutRaw))
} elseif ($selectedRuntimeEarly -eq 'host') {
    180
} else {
    900
}

Enter-ChatGptDevboxLifecycleMutex -RunDir $runDir -SelectedRuntime $selectedRuntimeEarly -TimeoutSeconds $startupTimeoutSeconds
try {
Ensure-Directory -Path $runDir
Write-StartupPhase -Phase 'preparing-runtime'
Write-GuardianDesiredState -RunDir $runDir -ShouldRun $true -Source "Start-ChatGptDevboxMcp.ps1"

$configuredMcpImplementation = if (-not [string]::IsNullOrWhiteSpace($env:DEVBOX_MCP_IMPLEMENTATION)) {
    $env:DEVBOX_MCP_IMPLEMENTATION
} else {
    Get-EnvValue -FilePath $envFile -Name 'DEVBOX_MCP_IMPLEMENTATION'
}
$mcpImplementation = Resolve-McpImplementation -ConfiguredValue $configuredMcpImplementation
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
$cloudflaredMetricsUrl = Get-EnvValue -FilePath $envFile -Name "CLOUDFLARED_METRICS_URL"
$cloudflaredEdgeIpVersion = Get-EnvValue -FilePath $envFile -Name "CLOUDFLARED_EDGE_IP_VERSION"
$cloudflaredTransportProtocol = Get-EnvValue -FilePath $envFile -Name "CLOUDFLARED_TRANSPORT_PROTOCOL"
$cloudflaredEdgeBindAddress = Get-EnvValue -FilePath $envFile -Name "CLOUDFLARED_EDGE_BIND_ADDRESS"
$usingNamedTunnel = (-not [string]::IsNullOrWhiteSpace($cloudflaredTunnelToken)) -and (-not [string]::IsNullOrWhiteSpace($cloudflaredPublicHostname))
$configuredPublicBaseUrl = Normalize-PublicBaseUrl -Value (Get-EnvValue -FilePath $envFile -Name "PUBLIC_BASE_URL")
if (-not $configuredPublicBaseUrl) {
    $configuredPublicBaseUrl = Normalize-PublicBaseUrl -Value $cloudflaredPublicHostname
}

$dockerReady = $false
Ensure-Directory -Path $hostWorkspace
if ($selectedRuntime -eq 'docker') {
    Write-StartupPhase -Phase 'preparing-docker-runtime'
    Assert-StartupDeadline -Phase 'preparing-docker-runtime'
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
    # Tunnel-only is itself an explicit request to repair the configured public path.
    # Do not require callers/Guardian to redundantly repeat -Public.
    $Public = $true
}
if ($Public) {
    Write-StartupPhase -Phase 'starting-cloudflare-tunnel'
    Assert-StartupDeadline -Phase 'starting-cloudflare-tunnel'
    $publicBaseUrl = Start-CloudflaredPublicTunnel -ContainerName $cloudflaredContainerName -TunnelToken $cloudflaredTunnelToken -PublicHostname $cloudflaredPublicHostname -Port $port -SelectedRuntime $selectedRuntime -RunDir $runDir -MetricsUrl $cloudflaredMetricsUrl -EdgeIpVersion $cloudflaredEdgeIpVersion -TransportProtocol $cloudflaredTransportProtocol -EdgeBindAddress $cloudflaredEdgeBindAddress -DockerReady:$dockerReady
    Write-StartupPhase -Phase 'cloudflare-tunnel-started' -Extra @{ PublicBaseUrl = $publicBaseUrl }
}
if ($TunnelOnly) {
    $hostTunnelPidFile = if ($usingNamedTunnel) { Join-Path $runDir 'host-cloudflared.pid' } else { '' }
    Write-StartupPhase -Phase 'waiting-public-health'
    Wait-ForHealthyPublicEndpoint -ContainerName $cloudflaredContainerName -PublicBaseUrl $publicBaseUrl -HostCloudflaredPidFile $hostTunnelPidFile
    $script:startupSucceeded = $true
    Write-StartupPhase -Phase 'ready' -Status 'ready' -Extra @{ PublicBaseUrl = $publicBaseUrl; TunnelOnly = $true }
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
    "DEVBOX_MCP_IMPLEMENTATION" = $mcpImplementation
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

Write-StartupPhase -Phase 'writing-runtime-config'
Write-RuntimeEnvFile -SourceEnvFile $envFile -RuntimeEnvFile $runtimeEnvFile -Overrides $overrides
Assert-StartupDeadline -Phase 'preflighting-mcp-replacement'
Write-StartupPhase -Phase 'preflighting-mcp-replacement'
$launchSpec = Assert-McpReplacementReady -Implementation $mcpImplementation -ProjectRoot $root -RuntimeEnvFile $runtimeEnvFile
Assert-StartupDeadline -Phase 'stopping-existing-mcp'
Write-StartupPhase -Phase 'stopping-existing-mcp'
Stop-ExistingServerIfOwned -PidFile $pidFile -ProjectRoot $root
Remove-Item $implementationFile -Force -ErrorAction SilentlyContinue

if (Test-Path $stdoutLog) { Remove-Item $stdoutLog -Force -ErrorAction SilentlyContinue }
if (Test-Path $stderrLog) { Remove-Item $stderrLog -Force -ErrorAction SilentlyContinue }

Assert-StartupDeadline -Phase 'starting-mcp'
Write-StartupPhase -Phase 'starting-mcp'
$startProcessParameters = @{
    FilePath = [string]$launchSpec.FilePath
    WorkingDirectory = $root
    RedirectStandardOutput = $stdoutLog
    RedirectStandardError = $stderrLog
    PassThru = $true
    WindowStyle = 'Hidden'
}
$launchArguments = @($launchSpec.ArgumentList)
if ($launchArguments.Count -gt 0) {
    $startProcessParameters['ArgumentList'] = $launchArguments
}
$childEnvironment = Read-RuntimeEnvValues -FilePath $runtimeEnvFile
if ($mcpImplementation -eq 'rust') {
    $childEnvironment['DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE'] = '1'
}
$previousChildEnvironment = Set-TemporaryProcessEnvironment -Values $childEnvironment
try {
    $process = Start-Process @startProcessParameters
} finally {
    Restore-TemporaryProcessEnvironment -Previous $previousChildEnvironment
}

$script:startupMcpPid = [int]$process.Id
# Establish ownership immediately. If health/elevation validation later fails,
# the outer catch block removes this exact owned process and PID file.
Set-Content -Path $pidFile -Value $process.Id -Encoding ASCII
Set-Content -Path $implementationFile -Value $mcpImplementation -Encoding ASCII
Write-StartupPhase -Phase 'waiting-local-health' -Extra @{ McpProcessId = [int]$process.Id; LocalUrl = "http://127.0.0.1:$port" }

$localUrl = "http://127.0.0.1:$port"
for ($i = 0; $i -lt 30; $i++) {
    Assert-StartupDeadline -Phase 'waiting-local-health'
    Start-Sleep -Seconds 2
    try {
        $response = Invoke-WebRequest -Uri "$localUrl/healthz" -UseBasicParsing -TimeoutSec 5
        if ($response.Content -match "ok") {
            break
        }
    } catch {
    }
}

Assert-StartupDeadline -Phase 'waiting-local-health'
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
    throw "MCP server PID $($process.Id) started without elevation. Host mode refuses medium-integrity MCP so host_exec cannot spam UAC. Start via elevated Guardian or ChatGptDevboxMcp-ElevatedStart."
}

Write-StartupPhase -Phase 'local-health-ready' -Extra @{ McpProcessId = [int]$process.Id; LocalUrl = $localUrl }

if ($Public) {
    $hostTunnelPidFile = if ($usingNamedTunnel) { Join-Path $runDir 'host-cloudflared.pid' } else { '' }
    Write-StartupPhase -Phase 'waiting-public-health' -Extra @{ McpProcessId = [int]$process.Id; PublicBaseUrl = $publicBaseUrl }
    Wait-ForHealthyPublicEndpoint -ContainerName $cloudflaredContainerName -PublicBaseUrl $publicBaseUrl -HostCloudflaredPidFile $hostTunnelPidFile
}

$script:startupSucceeded = $true
Write-StartupPhase -Phase 'ready' -Status 'ready' -Extra @{
    McpProcessId = [int]$process.Id
    LocalUrl = $localUrl
    PublicBaseUrl = $publicBaseUrl
    SelectedRuntime = $selectedRuntime
}

Write-Host "Local MCP URL: $localUrl"
if ($Public) {
    Write-Host "Public MCP URL: $publicBaseUrl"
    Write-Host "Legacy MCP URL: $publicBaseUrl/mcp"
}
Write-Host "Authentication mode: $(if ($authMode -eq 'none') { 'No Authentication' } else { 'OAuth' })"
Write-Host "Selected runtime: $selectedRuntime (requested: $runtimeMode)"
Write-Host "MCP implementation: $mcpImplementation"
} catch {
    $failureMessage = $_.Exception.Message
    try {
        Write-StartupPhase -Phase 'failed' -Status 'failed' -Details $failureMessage -Extra @{
            McpProcessId = $script:startupMcpPid
            SelectedRuntime = $selectedRuntime
        }
    } catch {
    }

    if ($script:startupMcpPid) {
        try {
            $candidate = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f [int]$script:startupMcpPid) -ErrorAction SilentlyContinue
            if ($candidate -and (Test-IsOwnedServerCommandLine -CommandLine ([string]$candidate.CommandLine))) {
                Stop-Process -Id ([int]$script:startupMcpPid) -Force -ErrorAction SilentlyContinue
            }
        } catch {
        }
        try {
            if (Test-Path $pidFile) {
                $pidText = Get-Content $pidFile -ErrorAction SilentlyContinue | Select-Object -First 1
                if ($pidText -eq [string]$script:startupMcpPid) {
                    Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
                    Remove-Item $implementationFile -Force -ErrorAction SilentlyContinue
                }
            }
        } catch {
        }
    }

    # A full Windows host startup owns the named tunnel it launched, but a
    # replacement can fail before the old MCP is stopped (for example a dependency
    # preflight failure). Preserve the new tunnel when the existing origin still
    # answers; only tear it down when no healthy MCP remains behind it.
    if (-not $TunnelOnly -and $selectedRuntime -eq 'host' -and $Public) {
        $originStillHealthy = $false
        try {
            $originCheck = Invoke-WebRequest -Uri "http://127.0.0.1:$port/healthz" -UseBasicParsing -TimeoutSec 3
            $originStillHealthy = $originCheck.Content -match 'ok'
        } catch {
        }
        if (-not $originStillHealthy) {
            try { Stop-ExistingHostCloudflared -PidFile (Join-Path $runDir 'host-cloudflared.pid') } catch {}
        }
    }
    throw
} finally {
    Exit-ChatGptDevboxLifecycleMutex
}
