param(
    [switch]$Public,
    [switch]$OAuth,
    [switch]$RebuildRuntime
)

$ErrorActionPreference = "Stop"

function Test-DockerEngine {
    docker version --format '{{.Server.Version}}' *> $null
    return ($LASTEXITCODE -eq 0)
}

function Start-DockerDesktopIfNeeded {
    if (Test-DockerEngine) {
        return
    }

    $dockerDesktop = "C:\Program Files\Docker\Docker\Docker Desktop.exe"
    if (-not (Test-Path $dockerDesktop)) {
        throw "Docker Desktop is not installed at $dockerDesktop"
    }

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

    docker inspect --type $Type $Name *> $null
    return ($LASTEXITCODE -eq 0)
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
        docker rm -f $ContainerName *> $null
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to remove existing devbox container $ContainerName."
        }
        $exists = $false
    }

    if (-not $exists) {
        docker run -d --name $ContainerName --init -w $DevboxWorkspace -v "${HostWorkspace}:${DevboxWorkspace}" $ImageName sleep infinity
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to create devbox container $ContainerName."
        }
        return
    }

    $running = docker inspect --type container $ContainerName --format '{{.State.Running}}'
    if ($running.Trim() -ne "true") {
        docker start $ContainerName *> $null
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to start devbox container $ContainerName."
        }
    }
}

function Start-CloudflaredQuickTunnel {
    param(
        [string]$ContainerName,
        [int]$Port
    )

    if (Test-DockerObjectExists -Type container -Name $ContainerName) {
        docker rm -f $ContainerName *> $null
    }

    docker run -d --name $ContainerName cloudflare/cloudflared:latest tunnel --no-autoupdate --url "http://host.docker.internal:$Port" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to start Cloudflare quick tunnel."
    }

    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Seconds 2
        $logs = docker logs $ContainerName 2>&1
        $match = [regex]::Match(($logs | Out-String), 'https://[a-z0-9-]+\.trycloudflare\.com')
        if ($match.Success) {
            return $match.Value.TrimEnd("/")
        }
    }

    throw "Cloudflare quick tunnel did not publish a URL."
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
        docker rm -f $ContainerName *> $null
    }

    docker run -d --name $ContainerName cloudflare/cloudflared:latest tunnel --no-autoupdate run --token $TunnelToken --url "http://host.docker.internal:$Port" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to start the named Cloudflare tunnel."
    }

    for ($i = 0; $i -lt 15; $i++) {
        Start-Sleep -Seconds 2
        $running = docker inspect --type container $ContainerName --format '{{.State.Running}}' 2>$null
        if ($LASTEXITCODE -ne 0) {
            break
        }

        if ($running.Trim() -eq "true") {
            return $publicBaseUrl
        }
    }

    $logs = docker logs $ContainerName 2>&1 | Out-String
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

$nodeExe = Resolve-NodeExecutable -ConfiguredPath (Get-EnvValue -FilePath $envFile -Name "NODE_EXE")

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

Write-Host "Local MCP URL: $localUrl/mcp"
if ($Public) {
    Write-Host "Public MCP URL: $publicBaseUrl/mcp"
}
Write-Host "Authentication mode: $(if ($authMode -eq 'none') { 'No Authentication' } else { 'OAuth' })"
