param(
    [switch]$Public,
    [switch]$OAuth,
    [switch]$RebuildRuntime,
    [ValidateSet('auto', 'host', 'docker')]
    [string]$Runtime = 'auto'
)

$ErrorActionPreference = "Stop"

function Get-DockerLogsText {
    param([string]$ContainerName)

    $output = cmd /d /c "docker logs $ContainerName 2>&1"
    return ($output | Out-String)
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
    $dockerCommand = Get-Command docker -ErrorAction SilentlyContinue
    if (-not $dockerCommand -or -not (Test-Path $dockerCommand.Source)) {
        return $false
    }
    $stdoutPath = Join-Path $env:TEMP ("devbox-docker-probe-{0}.stdout" -f [Guid]::NewGuid().ToString('N'))
    $stderrPath = Join-Path $env:TEMP ("devbox-docker-probe-{0}.stderr" -f [Guid]::NewGuid().ToString('N'))
    $process = $null
    try {
        $process = Start-Process -FilePath $dockerCommand.Source `
            -ArgumentList @('version', '--format', '{{.Server.Version}}') `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath `
            -WindowStyle Hidden `
            -PassThru
        if (-not $process.WaitForExit(5000)) {
            $candidate = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f $process.Id) -ErrorAction SilentlyContinue
            if ($candidate -and ([string]$candidate.CommandLine) -match 'docker(?:\.exe)?.*version' -and ([string]$candidate.CommandLine) -notmatch 'codex\.js|@openai/codex') {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            }
            return $false
        }
        return ($process.ExitCode -eq 0)
    } finally {
        if ($process) { $process.Dispose() }
        Remove-Item $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue
    }
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
    Start-Process -FilePath $dockerDesktop -WindowStyle Hidden | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(120)
    while ([DateTime]::UtcNow -lt $deadline) {
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

function Stop-ExistingServerIfOwned {
    param(
        [string]$PidFile,
        [string]$ProjectRoot
    )

    if (-not (Test-Path $PidFile)) {
        return
    }

    $pidText = (Get-Content $PidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
    if (-not $pidText) {
        Remove-Item $PidFile -Force -ErrorAction SilentlyContinue
        return
    }

    $ownedPid = [int]$pidText
    $commandLine = Get-CommandLineForPid -ProcessId $ownedPid
    if (-not $commandLine) {
        Remove-Item $PidFile -Force -ErrorAction SilentlyContinue
        return
    }

    if ($commandLine -notlike "*src/server.js*" -and $commandLine -notlike "*src\\server.js*") {
        throw "Refusing to stop PID $ownedPid because it does not look like the docker-chatgpt-devbox server."
    }

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

    $output = cmd /d /c "docker inspect --type $Type $Name 2>&1"
    $exitCode = $LASTEXITCODE
    if ($exitCode -eq 0) {
        return $true
    }
    $text = ($output | Out-String)
    if ($text -match 'No such (object|container|image)') {
        return $false
    }
    throw "docker inspect failed; refusing to create a conflicting object named $Name. Output:`n$text"
}

function Remove-DockerContainerIfPresent {
    param([string]$ContainerName)

    cmd /d /c "docker rm -f $ContainerName >NUL 2>NUL"
}

function Get-DockerContainerRunningState {
    param([string]$ContainerName)

    $result = cmd /d /c "docker inspect --type container $ContainerName --format ""{{.State.Running}}"" 2>&1"
    $exitCode = $LASTEXITCODE
    $text = ($result | Out-String).Trim()
    if ($exitCode -eq 0) {
        return $text
    }
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

    $output = docker run -d --name $ContainerName --init -w $DevboxWorkspace -v "${HostWorkspace}:${DevboxWorkspace}" $ImageName sleep infinity 2>&1
    if ($LASTEXITCODE -eq 0) {
        return
    }

    $text = ($output | Out-String)
    if ($text -match 'Conflict.*container name') {
        $running = Get-DockerContainerRunningState -ContainerName $ContainerName
        if ($null -ne $running) {
            docker start $ContainerName *> $null
            if ($LASTEXITCODE -eq 0) {
                return
            }
        }
    }
    throw "Failed to create devbox container $ContainerName. Output:`n$text"
}

function Ensure-RuntimeImage {
    param(
        [string]$Root,
        [string]$ImageName,
        [switch]$ForceRebuild
    )

    docker image inspect $ImageName *> $null
    $exists = ($LASTEXITCODE -eq 0)
    if ($exists -and -not $ForceRebuild) {
        return
    }

    docker build -f (Join-Path $Root "runtime.Dockerfile") -t $ImageName $Root
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to build runtime image $ImageName."
    }
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
        docker start $ContainerName *> $null
        if ($LASTEXITCODE -ne 0) {
            Remove-DockerContainerIfPresent -ContainerName $ContainerName
            New-DevboxContainer -ContainerName $ContainerName -ImageName $ImageName -HostWorkspace $HostWorkspace -DevboxWorkspace $DevboxWorkspace
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

    docker run -d --name $ContainerName cloudflare/cloudflared:latest tunnel --no-autoupdate --url "http://host.docker.internal:$Port" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to start Cloudflare quick tunnel."
    }

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

function Resolve-CloudflaredExecutable {
    $candidates = @(
        $env:CLOUDFLARED_EXE,
        $(if (${env:ProgramFiles(x86)}) { Join-Path ${env:ProgramFiles(x86)} 'cloudflared\cloudflared.exe' } else { $null }),
        'C:\Program Files (x86)\cloudflared\cloudflared.exe'
    )
    $command = Get-Command cloudflared -ErrorAction SilentlyContinue
    if ($command) { $candidates += [string]$command.Source }
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) { return $candidate }
    }
    throw 'cloudflared was not found. Install it or set CLOUDFLARED_EXE.'
}

function Stop-OwnedHostCloudflared {
    param([string]$PidFile)

    if (-not (Test-Path $PidFile)) { return }
    $pidText = Get-Content $PidFile -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($pidText -match '^\d+$') {
        $candidate = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f [int]$pidText) -ErrorAction SilentlyContinue
        if ($candidate -and ([string]$candidate.CommandLine) -match 'cloudflared(?:\.exe)?.*host-cloudflared\.tunnel-token\.txt') {
            Stop-Process -Id ([int]$candidate.ProcessId) -Force
            for ($i = 0; $i -lt 20; $i++) {
                Start-Sleep -Milliseconds 250
                if (-not (Get-Process -Id ([int]$candidate.ProcessId) -ErrorAction SilentlyContinue)) { break }
            }
        }
    }
    Remove-Item -LiteralPath $PidFile -Force -ErrorAction SilentlyContinue
}

function Start-HostCloudflaredNamedTunnel {
    param(
        [string]$TunnelToken,
        [string]$PublicHostname,
        [int]$Port,
        [string]$RunDir
    )

    if (-not $TunnelToken) { throw 'CLOUDFLARED_TUNNEL_TOKEN is required for the named Cloudflare tunnel.' }
    $publicBaseUrl = Normalize-PublicBaseUrl -Value $PublicHostname
    if (-not $publicBaseUrl) { throw 'CLOUDFLARED_PUBLIC_HOSTNAME is required for the named Cloudflare tunnel.' }
    $cloudflaredExe = Resolve-CloudflaredExecutable
    $pidFile = Join-Path $RunDir 'host-cloudflared.pid'
    $tokenFile = Join-Path $RunDir 'host-cloudflared.tunnel-token.txt'
    $configFile = Join-Path $RunDir 'host-cloudflared-config.yml'
    $stdoutLog = Join-Path $RunDir 'host-cloudflared.stdout.log'
    $stderrLog = Join-Path $RunDir 'host-cloudflared.stderr.log'
    Stop-OwnedHostCloudflared -PidFile $pidFile
    [System.IO.File]::WriteAllText($tokenFile, $TunnelToken, [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText($configFile, "url: http://127.0.0.1:$Port`nloglevel: info`n", [System.Text.UTF8Encoding]::new($false))
    Remove-Item $stdoutLog, $stderrLog -Force -ErrorAction SilentlyContinue
    $process = Start-Process -FilePath $cloudflaredExe `
        -ArgumentList @('tunnel', '--config', $configFile, '--no-autoupdate', '--loglevel', 'info', 'run', '--token-file', $tokenFile) `
        -WorkingDirectory $RunDir `
        -RedirectStandardOutput $stdoutLog `
        -RedirectStandardError $stderrLog `
        -PassThru `
        -WindowStyle Hidden
    Set-Content -Path $pidFile -Value $process.Id -Encoding ASCII
    Start-Sleep -Seconds 3
    $process.Refresh()
    if ($process.HasExited) {
        $stderr = if (Test-Path $stderrLog) { Get-Content $stderrLog -Raw } else { '' }
        throw "The named Cloudflare tunnel exited early with code $($process.ExitCode). Logs:`n$stderr"
    }
    return $publicBaseUrl
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

    docker run -d --name $ContainerName cloudflare/cloudflared:latest tunnel --no-autoupdate run --token $TunnelToken --url "http://host.docker.internal:$Port" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to start the named Cloudflare tunnel."
    }

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
        [int]$Port,
        [ValidateSet('host', 'docker')]
        [string]$SelectedRuntime,
        [string]$RunDir
    )

    $hasToken = -not [string]::IsNullOrWhiteSpace($TunnelToken)
    $hasHostname = -not [string]::IsNullOrWhiteSpace($PublicHostname)

    if ($hasToken -or $hasHostname) {
        if (-not ($hasToken -and $hasHostname)) {
            throw "CLOUDFLARED_TUNNEL_TOKEN and CLOUDFLARED_PUBLIC_HOSTNAME must both be set to use the named Cloudflare tunnel."
        }

        if ($SelectedRuntime -eq 'host') {
            return Start-HostCloudflaredNamedTunnel -TunnelToken $TunnelToken -PublicHostname $PublicHostname -Port $Port -RunDir $RunDir
        }
        return Start-CloudflaredNamedTunnel -ContainerName $ContainerName -TunnelToken $TunnelToken -PublicHostname $PublicHostname -Port $Port
    }

    if ($SelectedRuntime -eq 'host') {
        throw 'Docker is required for Cloudflare quick tunnels. Configure a named tunnel for host mode.'
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
if ($selectedRuntime -eq 'docker') {
    Start-DockerDesktopIfNeeded
    Ensure-RuntimeImage -Root $root -ImageName $imageName -ForceRebuild:$RebuildRuntime
    Ensure-DevboxContainer -ContainerName $containerName -ImageName $imageName -HostWorkspace $hostWorkspace -DevboxWorkspace $devboxWorkspace -Recreate:$RebuildRuntime
}

$publicBaseUrl = $configuredPublicBaseUrl
if ($Public) {
    $publicBaseUrl = Start-CloudflaredPublicTunnel -ContainerName $cloudflaredContainerName -TunnelToken $cloudflaredTunnelToken -PublicHostname $cloudflaredPublicHostname -Port $port -SelectedRuntime $selectedRuntime -RunDir $runDir
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

Set-Content -Path $pidFile -Value $process.Id

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

if ($Public) {
    $hostTunnelPidFile = if ($selectedRuntime -eq 'host') { Join-Path $runDir 'host-cloudflared.pid' } else { '' }
    Wait-ForHealthyPublicEndpoint -ContainerName $cloudflaredContainerName -PublicBaseUrl $publicBaseUrl -HostCloudflaredPidFile $hostTunnelPidFile
}

Write-Host "Local MCP URL: $localUrl/mcp"
if ($Public) {
    Write-Host "Public MCP URL: $publicBaseUrl/mcp"
}
Write-Host "Authentication mode: $(if ($authMode -eq 'none') { 'No Authentication' } else { 'OAuth' })"
Write-Host "Selected runtime: $selectedRuntime (requested: $runtimeMode)"
} finally {
    Exit-ChatGptDevboxLifecycleMutex
}
