param(
    [switch]$Public,
    [switch]$OAuth,
    [switch]$RebuildRuntime
)

$ErrorActionPreference = "Stop"
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
    $result = Invoke-Docker -Arguments @('version', '--format', '{{.Server.Version}}') -IgnoreExitCode
    return ($result.ExitCode -eq 0)
}

function Start-DockerDesktopIfNeeded {
    if (Test-DockerEngine) {
        return
    }

    $dockerDesktop = "C:\Program Files\Docker\Docker\Docker Desktop.exe"
    if (-not (Test-Path $dockerDesktop)) {
        throw "Docker Desktop is not installed at $dockerDesktop"
    }

    Write-Host "Docker engine is not ready. Starting Docker Desktop..."
    Start-Process -FilePath $dockerDesktop | Out-Null
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Seconds 2
        if (Test-DockerEngine) {
            return
        }
    }

    throw "Docker engine did not become ready within 120 seconds."
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
    Stop-Process -Id $ownedPid -Force
    Start-Sleep -Seconds 1
    if (Get-Process -Id $ownedPid -ErrorAction SilentlyContinue) {
        throw "Failed to stop PID $ownedPid."
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
    return ($result.ExitCode -eq 0)
}

function Remove-DockerContainerIfPresent {
    param([string]$ContainerName)

    [void](Invoke-Docker -Arguments @('rm', '-f', $ContainerName) -IgnoreExitCode)
}

function Get-DockerContainerRunningState {
    param([string]$ContainerName)

    $result = Invoke-Docker -Arguments @('inspect', '--type', 'container', $ContainerName, '--format', '{{.State.Running}}') -IgnoreExitCode
    if ($result.ExitCode -ne 0) {
        return $null
    }

    return $result.Text.Trim()
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
        [void](Invoke-Docker -Arguments @('run', '-d', '--name', $ContainerName, '--init', '-w', $DevboxWorkspace, '-v', "${HostWorkspace}:${DevboxWorkspace}", $ImageName, 'sleep', 'infinity'))
        return
    }

    $running = Get-DockerContainerRunningState -ContainerName $ContainerName
    if ($running.Trim() -ne "true") {
        [void](Invoke-Docker -Arguments @('start', $ContainerName))
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

    for ($i = 0; $i -lt 30; $i++) {
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
        [string]$PublicBaseUrl
    )

    $healthUrl = "$($PublicBaseUrl.TrimEnd('/'))/healthz"
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Seconds 2
        $running = Get-DockerContainerRunningState -ContainerName $ContainerName
        if ($null -eq $running -or $running -ne "true") {
            break
        }

        try {
            $response = Invoke-WebRequest -Uri $healthUrl -UseBasicParsing -TimeoutSec 5
            if ($response.Content -match "ok") {
                return
            }
        } catch {
        }
    }

    $logs = Get-DockerLogsText -ContainerName $ContainerName
    throw "The Cloudflare tunnel did not expose a healthy public endpoint at $healthUrl. Logs:`n$logs"
}

function Start-CloudflaredNamedTunnel {
    param(
        [string]$ContainerName,
        [string]$TunnelToken,
        [string]$PublicHostname,
        [int]$Port
    )

    if (-not $TunnelToken) {
        throw "CLOUDFLARED_TUNNEL_TOKEN is required for the named Cloudflare tunnel."
    }

    $publicBaseUrl = Normalize-PublicBaseUrl -Value $PublicHostname
    if (-not $publicBaseUrl) {
        throw "CLOUDFLARED_PUBLIC_HOSTNAME is required for the named Cloudflare tunnel."
    }

    if (Test-DockerObjectExists -Type container -Name $ContainerName) {
        Remove-DockerContainerIfPresent -ContainerName $ContainerName
    }

    [void](Invoke-Docker -Arguments @('run', '-d', '--name', $ContainerName, 'cloudflare/cloudflared:latest', 'tunnel', '--no-autoupdate', 'run', '--token', $TunnelToken, '--url', "http://host.docker.internal:$Port"))

    for ($i = 0; $i -lt 15; $i++) {
        Start-Sleep -Seconds 2
        $running = Get-DockerContainerRunningState -ContainerName $ContainerName
        if ($null -eq $running) {
            break
        }

        if ($running -eq "true") {
            return $publicBaseUrl
        }
    }

    $logs = Get-DockerLogsText -ContainerName $ContainerName
    throw "The named Cloudflare tunnel did not stay healthy. Logs:`n$logs"
}

function Start-CloudflaredPublicTunnel {
    param(
        [string]$ContainerName,
        [string]$TunnelToken,
        [string]$PublicHostname,
        [int]$Port
    )

    $hasToken = -not [string]::IsNullOrWhiteSpace($TunnelToken)
    $hasHostname = -not [string]::IsNullOrWhiteSpace($PublicHostname)

    if ($hasToken -or $hasHostname) {
        if (-not ($hasToken -and $hasHostname)) {
            throw "CLOUDFLARED_TUNNEL_TOKEN and CLOUDFLARED_PUBLIC_HOSTNAME must both be set to use the named Cloudflare tunnel."
        }

        return Start-CloudflaredNamedTunnel -ContainerName $ContainerName -TunnelToken $TunnelToken -PublicHostname $PublicHostname -Port $Port
    }

    return Start-CloudflaredQuickTunnel -ContainerName $ContainerName -Port $Port
}

Enter-ChatGptDevboxLifecycleMutex
try {
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

Start-DockerDesktopIfNeeded

Ensure-Directory -Path $runDir
Write-GuardianDesiredState -RunDir $runDir -ShouldRun $true -Source "Start-ChatGptDevboxMcp.ps1"

$nodeExe = Resolve-NodeExecutable -ConfiguredPath (Get-EnvValue -FilePath $envFile -Name "NODE_EXE")
$script:dockerConfiguredPath = Get-EnvValue -FilePath $envFile -Name "DOCKER_EXE"
[void](Get-DockerExecutable)

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

Ensure-Directory -Path $hostWorkspace
Ensure-RuntimeImage -Root $root -ImageName $imageName -ForceRebuild:$RebuildRuntime
Ensure-DevboxContainer -ContainerName $containerName -ImageName $imageName -HostWorkspace $hostWorkspace -DevboxWorkspace $devboxWorkspace -Recreate:$RebuildRuntime

$publicBaseUrl = $configuredPublicBaseUrl
if ($Public) {
    $publicBaseUrl = Start-CloudflaredPublicTunnel -ContainerName $cloudflaredContainerName -TunnelToken $cloudflaredTunnelToken -PublicHostname $cloudflaredPublicHostname -Port $port
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
}

$overrides = @{
    "MCP_AUTH_MODE" = $authMode
    "PUBLIC_BASE_URL" = $publicBaseUrl
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

Set-Content -Path $pidFile -Value $process.Id

if ($Public) {
    Wait-ForHealthyPublicEndpoint -ContainerName $cloudflaredContainerName -PublicBaseUrl $publicBaseUrl
}

Write-Host "Local MCP URL: $localUrl"
if ($Public) {
    Write-Host "Public MCP URL: $publicBaseUrl"
    Write-Host "Legacy MCP URL: $publicBaseUrl/mcp"
}
Write-Host "Authentication mode: $(if ($authMode -eq 'none') { 'No Authentication' } else { 'OAuth' })"
} finally {
    Exit-ChatGptDevboxLifecycleMutex
}
