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

function Invoke-Docker {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$IgnoreExitCode
    )

    $dockerExe = Get-DockerExecutable
    $stdoutPath = Join-Path $env:TEMP ("chatgpt-devbox-docker-{0}.stdout.log" -f [System.Guid]::NewGuid().ToString('N'))
    $stderrPath = Join-Path $env:TEMP ("chatgpt-devbox-docker-{0}.stderr.log" -f [System.Guid]::NewGuid().ToString('N'))

    try {
        $process = Start-Process -FilePath $dockerExe `
            -ArgumentList $Arguments `
            -Wait `
            -PassThru `
            -WindowStyle Hidden `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath

        $stdoutText = if (Test-Path $stdoutPath) { [string](Get-Content -Path $stdoutPath -Raw) } else { '' }
        $stderrText = if (Test-Path $stderrPath) { [string](Get-Content -Path $stderrPath -Raw) } else { '' }
        $exitCode = [int]$process.ExitCode
        $text = (($stdoutText, $stderrText) -join '').Trim()
    } finally {
        Remove-Item -LiteralPath $stdoutPath -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $stderrPath -Force -ErrorAction SilentlyContinue
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
        $pidText = Get-Content $PidFile -ErrorAction SilentlyContinue | Select-Object -First 1
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
$script:dockerConfiguredPath = Get-EnvValue -FilePath $envFile -Name "DOCKER_EXE"
Write-GuardianDesiredState -RunDir $runDir -ShouldRun $false -Source "Stop-ChatGptDevboxMcp.ps1"

if (Test-Path $pidFile) {
    $ownedProcess = Find-OwnedServerProcess -PidFile $pidFile
    if ($ownedProcess) {
        $ownedPid = [int]$ownedProcess.ProcessId
        Stop-Process -Id $ownedPid -Force
        Start-Sleep -Seconds 1
    }

    Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
}

if ((Test-Path $envFile) -and ($Tunnel -or $All)) {
    $cloudflaredContainerName = Get-EnvValue -FilePath $envFile -Name "CLOUDFLARED_CONTAINER_NAME"
    $inspectResult = Invoke-Docker -Arguments @('inspect', '--type', 'container', $cloudflaredContainerName) -IgnoreExitCode
    if ($inspectResult.ExitCode -eq 0) {
        [void](Invoke-Docker -Arguments @('rm', '-f', $cloudflaredContainerName) -IgnoreExitCode)
    }
}

if ((Test-Path $envFile) -and $All) {
    $containerName = Get-EnvValue -FilePath $envFile -Name "DEVBOX_CONTAINER_NAME"
    $inspectResult = Invoke-Docker -Arguments @('inspect', '--type', 'container', $containerName) -IgnoreExitCode
    if ($inspectResult.ExitCode -eq 0) {
        [void](Invoke-Docker -Arguments @('stop', $containerName) -IgnoreExitCode)
    }
}
} finally {
    Exit-ChatGptDevboxLifecycleMutex
}
