param(
    [switch]$Tunnel,
    [switch]$All
)

$ErrorActionPreference = "Stop"
$script:dockerExe = $null
$script:dockerConfiguredPath = $null

function Resolve-DockerExecutable {
    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($candidate in @(
            $script:dockerConfiguredPath,
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
        $script:dockerExe = Resolve-DockerExecutable
    }

    return $script:dockerExe
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

function Invoke-Docker {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$IgnoreExitCode,
        [int]$TimeoutSeconds = 60
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
    } finally {
        if ($process) {
            $process.Dispose()
        }
    }

    if ($timedOut) {
        if (-not $IgnoreExitCode) {
            throw "docker $($Arguments -join ' ') timed out after $TimeoutSeconds seconds. Output:`n$text"
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
            throw "docker $($Arguments -join ' ') failed with exit code $exitCode. Output:`n$trimmedText"
        }

        throw "docker $($Arguments -join ' ') failed with exit code $exitCode."
    }

    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = $text
        Text = $text
    }
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

    if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
        return $false
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

    $executableArgument = [string]$arguments[0]
    $executable = if ([IO.Path]::IsPathRooted($executableArgument)) {
        Resolve-McpComparablePath -Path $executableArgument
    } else {
        $null
    }
    if ($executable -eq $expectedRust) {
        return $true
    }

    $serverMatches = $false
    $runtimeEnvMatches = $false
    for ($i = 1; $i -lt $arguments.Count; $i++) {
        $argument = [string]$arguments[$i]
        if ($argument.StartsWith('--env-file=', [StringComparison]::OrdinalIgnoreCase)) {
            $runtimePath = $argument.Substring('--env-file='.Length)
            $runtimeEnvMatches = [IO.Path]::IsPathRooted($runtimePath) -and ((Resolve-McpComparablePath -Path $runtimePath) -eq $expectedRuntimeEnv)
            continue
        }
        if ($argument.Equals('--env-file', [StringComparison]::OrdinalIgnoreCase) -and ($i + 1) -lt $arguments.Count) {
            $i++
            $runtimePath = [string]$arguments[$i]
            $runtimeEnvMatches = [IO.Path]::IsPathRooted($runtimePath) -and ((Resolve-McpComparablePath -Path $runtimePath) -eq $expectedRuntimeEnv)
            continue
        }
        if ([IO.Path]::IsPathRooted($argument) -and ((Resolve-McpComparablePath -Path $argument) -eq $expectedServer)) {
            $serverMatches = $true
        }
    }
    return ($serverMatches -and $runtimeEnvMatches)
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

function Find-OwnedServerProcess {
    param(
        [string]$PidFile,
        [string]$ProjectRoot
    )

    if (Test-Path $PidFile) {
        $pidText = Get-Content $PidFile -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($pidText -match '^\d+$') {
            $candidate = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f [int]$pidText) -ErrorAction SilentlyContinue
            if ($candidate -and (Test-IsOwnedServerCommandLine -CommandLine ([string]$candidate.CommandLine) -ProjectRoot $ProjectRoot)) {
                return $candidate
            }
        }
    }

    return Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object { Test-IsOwnedServerCommandLine -CommandLine ([string]$_.CommandLine) -ProjectRoot $ProjectRoot } |
        Sort-Object CreationDate -Descending |
        Select-Object -First 1
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

    $Value | ConvertTo-Json -Depth 8 | Set-Content -Path $Path -Encoding UTF8
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

Enter-ChatGptDevboxLifecycleMutex
try {
$root = Split-Path -Parent $PSScriptRoot
$envFile = Join-Path $root ".env"
$runDir = Join-Path $root "run"
$pidFile = Join-Path $runDir "mcp.pid"
$implementationFile = Join-Path $runDir "mcp.implementation"
$settingsPath = Join-Path $runDir 'guardian.settings.json'
$settings = if (Test-Path $settingsPath) { Get-Content $settingsPath -Raw | ConvertFrom-Json } else { $null }
$selectedRuntime = if ($settings -and $settings.PSObject.Properties['SelectedRuntime'] -and ([string]$settings.SelectedRuntime).ToLowerInvariant() -in @('host', 'docker')) {
    ([string]$settings.SelectedRuntime).ToLowerInvariant()
} else {
    $configuredRuntime = if (Test-Path $envFile) { (Get-EnvValue -FilePath $envFile -Name 'DEVBOX_RUNTIME_MODE').ToLowerInvariant() } else { '' }
    if ($configuredRuntime -eq 'host') { 'host' } else { 'docker' }
}
$script:dockerConfiguredPath = Get-EnvValue -FilePath $envFile -Name "DOCKER_EXE"
Write-GuardianDesiredState -RunDir $runDir -ShouldRun $false -Source "Stop-ChatGptDevboxMcp.ps1"

if (Test-Path $pidFile) {
    $ownedProcess = Find-OwnedServerProcess -PidFile $pidFile -ProjectRoot $root
    if ($ownedProcess) {
        $ownedPid = [int]$ownedProcess.ProcessId
        Stop-Process -Id $ownedPid -Force
        Start-Sleep -Seconds 1
    }

    Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
}
Remove-Item $implementationFile -Force -ErrorAction SilentlyContinue

if ((Test-Path $envFile) -and ($Tunnel -or $All)) {
    Stop-ExistingHostCloudflared -PidFile (Join-Path $runDir 'host-cloudflared.pid')
    if ($selectedRuntime -eq 'docker') {
        $cloudflaredContainerName = Get-EnvValue -FilePath $envFile -Name "CLOUDFLARED_CONTAINER_NAME"
        if (-not $cloudflaredContainerName) { $cloudflaredContainerName = 'chatgpt-devbox-cloudflared' }
        $inspectResult = Invoke-Docker -Arguments @('inspect', '--type', 'container', $cloudflaredContainerName) -IgnoreExitCode
        if ($inspectResult.ExitCode -eq 0) {
            [void](Invoke-Docker -Arguments @('rm', '-f', $cloudflaredContainerName) -IgnoreExitCode)
        }
    }
}

if ((Test-Path $envFile) -and $All -and $selectedRuntime -eq 'docker') {
    $containerName = Get-EnvValue -FilePath $envFile -Name "DEVBOX_CONTAINER_NAME"
    if (-not $containerName) { $containerName = 'chatgpt-devbox-runtime' }
    $inspectResult = Invoke-Docker -Arguments @('inspect', '--type', 'container', $containerName) -IgnoreExitCode
    if ($inspectResult.ExitCode -eq 0) {
        [void](Invoke-Docker -Arguments @('stop', $containerName) -IgnoreExitCode)
    }
}
} finally {
    Exit-ChatGptDevboxLifecycleMutex
}
