param()

$ErrorActionPreference = "Stop"
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

    throw "Docker CLI was not found. Install Docker Desktop or set DOCKER_EXE in the environment."
}

$root = Split-Path -Parent $PSScriptRoot
$envFile = Join-Path $root ".env"
$script:dockerConfiguredPath = if (Test-Path $envFile) {
    $match = Select-String -Path $envFile -Pattern '^DOCKER_EXE=(.*)$' | Select-Object -First 1
    if ($match) { $match.Matches[0].Groups[1].Value.Trim() } else { $null }
} else {
    $null
}
$publicBaseUrlMatch = Select-String -Path $envFile -Pattern '^CLOUDFLARED_PUBLIC_HOSTNAME=(.*)$' | Select-Object -First 1
$configuredPublicValue = if ($publicBaseUrlMatch) { $publicBaseUrlMatch.Matches[0].Groups[1].Value.Trim() } else { "" }

if ($configuredPublicValue) {
    if ($configuredPublicValue -match '^https?://') {
        Write-Host "$($configuredPublicValue.TrimEnd('/'))/mcp"
    } else {
        Write-Host "https://$($configuredPublicValue.TrimEnd('/'))/mcp"
    }
    exit 0
}

$match = Select-String -Path $envFile -Pattern '^CLOUDFLARED_CONTAINER_NAME=(.*)$' | Select-Object -First 1
$containerName = if ($match) { $match.Matches[0].Groups[1].Value.Trim() } else { "chatgpt-devbox-cloudflared" }

$dockerExe = Resolve-DockerExecutable
$stdoutPath = Join-Path $env:TEMP ("chatgpt-devbox-docker-{0}.stdout.log" -f [System.Guid]::NewGuid().ToString('N'))
$stderrPath = Join-Path $env:TEMP ("chatgpt-devbox-docker-{0}.stderr.log" -f [System.Guid]::NewGuid().ToString('N'))

try {
    $process = Start-Process -FilePath $dockerExe `
        -ArgumentList @('logs', $containerName) `
        -Wait `
        -PassThru `
        -WindowStyle Hidden `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath

    $logs = ''
    if (Test-Path $stdoutPath) {
        $logs += [string](Get-Content -Path $stdoutPath -Raw)
    }
    if (Test-Path $stderrPath) {
        $logs += [string](Get-Content -Path $stderrPath -Raw)
    }

    if ($process.ExitCode -ne 0) {
        throw "docker logs $containerName failed with exit code $($process.ExitCode). Output:`n$($logs.Trim())"
    }
} finally {
    Remove-Item -LiteralPath $stdoutPath -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $stderrPath -Force -ErrorAction SilentlyContinue
}

$urlMatch = [regex]::Match(($logs | Out-String), 'https://[a-z0-9-]+\.trycloudflare\.com')
if (-not $urlMatch.Success) {
    throw "Could not find a trycloudflare URL in $containerName logs."
}

Write-Host "$($urlMatch.Value.TrimEnd('/'))/mcp"
