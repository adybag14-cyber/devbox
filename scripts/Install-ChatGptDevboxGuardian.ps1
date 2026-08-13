[CmdletBinding()]
param(
    [switch]$Public,
    [switch]$OAuth,
    [ValidateSet('auto', 'host', 'docker')]
    [string]$Runtime = 'auto',
    [string]$TaskPrefix = 'ChatGptDevboxGuardian'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$runDir = Join-Path $projectRoot 'run'
$guardianDir = Join-Path $runDir 'guardian'
$envFile = Join-Path $projectRoot '.env'
$runtimeEnvFile = Join-Path $projectRoot '.env.runtime'
$ensureScript = Join-Path $PSScriptRoot 'Ensure-ChatGptDevboxGuardian.ps1'
$elevatedLauncher = Join-Path $PSScriptRoot 'Run-Start-ChatGptDevboxMcp.vbs'
$powerShellResolver = Join-Path $PSScriptRoot 'Resolve-DevboxPowerShell.ps1'
. $powerShellResolver
$settingsPath = Join-Path $runDir 'guardian.settings.json'
$desiredStatePath = Join-Path $runDir 'guardian.desired-state.json'
$wscriptExe = Join-Path $env:WINDIR 'System32\wscript.exe'
$userId = '{0}\{1}' -f $env:USERDOMAIN, $env:USERNAME
$startupTaskName = "$TaskPrefix-Startup"
$logonTaskName = "$TaskPrefix-Logon"
$keepAliveTaskName = "$TaskPrefix-KeepAlive"
$elevatedStartTaskName = if ($TaskPrefix -eq 'ChatGptDevboxGuardian') { 'ChatGptDevboxMcp-ElevatedStart' } else { "$TaskPrefix-McpElevatedStart" }
$startScript = Join-Path $PSScriptRoot 'Start-ChatGptDevboxMcp.ps1'

foreach ($path in @($runDir, $guardianDir)) {
    if (-not (Test-Path $path)) {
        New-Item -ItemType Directory -Path $path | Out-Null
    }
}

if (-not (Test-Path $elevatedLauncher)) {
    throw "Elevated MCP launcher not found: $elevatedLauncher"
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

function Write-JsonFile {
    param(
        [string]$Path,
        [object]$Value
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

function Remove-TaskIfPresent {
    param([string]$TaskName)

    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    }
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

$existingSettings = Read-JsonFile -Path $settingsPath
$primaryEnvFile = Get-PrimaryEnvFile
$inferredPort = 8100
$inferredPublic = $false
$inferredOAuth = $false
$devboxContainerName = 'chatgpt-devbox-runtime'
$cloudflaredContainerName = 'chatgpt-devbox-cloudflared'
$publicBaseUrl = ''
$authMode = 'none'
$inferredRuntimeMode = 'auto'

if ($primaryEnvFile) {
    $portValue = Get-EnvValue -FilePath $primaryEnvFile -Name 'PORT'
    if ($portValue -match '^\d+$') {
        $inferredPort = [int]$portValue
    }

    $publicBaseUrl = Get-EnvValue -FilePath $primaryEnvFile -Name 'PUBLIC_BASE_URL'
    if (-not $publicBaseUrl) {
        $publicBaseUrl = Get-EnvValue -FilePath $envFile -Name 'CLOUDFLARED_PUBLIC_HOSTNAME'
    }

    $authMode = Get-EnvValue -FilePath $primaryEnvFile -Name 'MCP_AUTH_MODE'
    if (-not $authMode) {
        $authMode = Get-EnvValue -FilePath $envFile -Name 'MCP_AUTH_MODE'
    }

    $runtimeValue = (Get-EnvValue -FilePath $primaryEnvFile -Name 'DEVBOX_RUNTIME_MODE').ToLowerInvariant()
    if ($runtimeValue -in @('auto', 'host', 'docker')) {
        $inferredRuntimeMode = $runtimeValue
    }

    $devboxValue = Get-EnvValue -FilePath $envFile -Name 'DEVBOX_CONTAINER_NAME'
    if ($devboxValue) {
        $devboxContainerName = $devboxValue
    }

    $cloudflaredValue = Get-EnvValue -FilePath $envFile -Name 'CLOUDFLARED_CONTAINER_NAME'
    if ($cloudflaredValue) {
        $cloudflaredContainerName = $cloudflaredValue
    }

    $inferredPublic = -not [string]::IsNullOrWhiteSpace($publicBaseUrl)
    $inferredOAuth = $authMode -and $authMode -ne 'none'
}

$publicEnabled = if ($PSBoundParameters.ContainsKey('Public')) {
    [bool]$Public
} elseif ($existingSettings -and $null -ne $existingSettings.Public) {
    [bool]$existingSettings.Public
} else {
    $inferredPublic
}

$oauthEnabled = if ($PSBoundParameters.ContainsKey('OAuth')) {
    [bool]$OAuth
} elseif ($existingSettings -and $null -ne $existingSettings.OAuth) {
    [bool]$existingSettings.OAuth
} else {
    $inferredOAuth
}

$existingRuntimeMode = if ($existingSettings -and $existingSettings.PSObject.Properties['RuntimeMode']) {
    ([string]$existingSettings.RuntimeMode).ToLowerInvariant()
} else { '' }
$existingSelectedRuntime = if ($existingSettings -and $existingSettings.PSObject.Properties['SelectedRuntime']) {
    ([string]$existingSettings.SelectedRuntime).ToLowerInvariant()
} else { '' }

$runtimeMode = if ($PSBoundParameters.ContainsKey('Runtime')) {
    $Runtime.ToLowerInvariant()
} elseif ($existingRuntimeMode -in @('auto', 'host', 'docker')) {
    $existingRuntimeMode
} else {
    $inferredRuntimeMode
}

$selectedRuntime = if ($runtimeMode -in @('host', 'docker')) {
    $runtimeMode
} elseif ($existingSelectedRuntime -in @('host', 'docker')) {
    $existingSelectedRuntime
} else {
    $legacyPid = Join-Path $runDir 'mcp.pid'
    $legacyHealthy = $false
    if (Test-Path $legacyPid) {
        $legacyPidText = Get-Content $legacyPid -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($legacyPidText -match '^\d+$' -and (Get-Process -Id ([int]$legacyPidText) -ErrorAction SilentlyContinue)) {
            try {
                $legacyHealth = Invoke-WebRequest -Uri ("http://127.0.0.1:{0}/healthz" -f $inferredPort) -UseBasicParsing -TimeoutSec 5
                $legacyHealthy = ($legacyHealth.Content -match 'ok')
            } catch {
                $legacyHealthy = $false
            }
        }
    }
    if ($legacyHealthy) { 'host' } else { 'docker' }
}

$settings = @{
    Public = $publicEnabled
    OAuth = $oauthEnabled
    Port = if ($existingSettings -and $existingSettings.Port) { [int]$existingSettings.Port } else { $inferredPort }
    DevboxContainerName = if ($existingSettings -and $existingSettings.DevboxContainerName) { [string]$existingSettings.DevboxContainerName } else { $devboxContainerName }
    CloudflaredContainerName = if ($existingSettings -and $existingSettings.CloudflaredContainerName) { [string]$existingSettings.CloudflaredContainerName } else { $cloudflaredContainerName }
    PublicBaseUrl = if ($existingSettings -and $existingSettings.PublicBaseUrl) { [string]$existingSettings.PublicBaseUrl } else { [string]$publicBaseUrl }
    AuthMode = if ($existingSettings -and $existingSettings.AuthMode) { [string]$existingSettings.AuthMode } else { [string]$authMode }
    RuntimeMode = $runtimeMode
    SelectedRuntime = $selectedRuntime
    UpdatedAtUtc = [DateTime]::UtcNow.ToString('o')
    Source = 'Install-ChatGptDevboxGuardian.ps1'
}

Write-JsonFile -Path $settingsPath -Value $settings
Write-JsonFile -Path $desiredStatePath -Value @{
    ShouldRun = $true
    UpdatedAtUtc = [DateTime]::UtcNow.ToString('o')
    Source = 'Install-ChatGptDevboxGuardian.ps1'
}

foreach ($taskName in @($startupTaskName, $logonTaskName, $keepAliveTaskName, $elevatedStartTaskName)) {
    Remove-TaskIfPresent -TaskName $taskName
}

$powerShellExe = Resolve-DevboxPowerShellExecutable
$ensureActionArgs = @(
    '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
    '-WindowStyle', 'Hidden', '-File', ('"{0}"' -f $ensureScript),
    '-ProjectRoot', ('"{0}"' -f $projectRoot)
) -join ' '
$action = New-ScheduledTaskAction -Execute $powerShellExe -Argument $ensureActionArgs
$startupTrigger = New-ScheduledTaskTrigger -AtStartup
$logonTrigger = New-ScheduledTaskTrigger -AtLogOn -User $userId
$keepAliveTrigger = New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(1) -RepetitionInterval (New-TimeSpan -Minutes 1)
$settingsSet = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 2) `
    -Priority 4 `
    -RestartCount 3 `
    -RestartInterval (New-TimeSpan -Minutes 1)
$interactivePrincipal = New-ScheduledTaskPrincipal -UserId $userId -LogonType Interactive -RunLevel Highest
$startupPrincipal = New-ScheduledTaskPrincipal -UserId $userId -LogonType S4U -RunLevel Highest

# Startup and keepalive are non-interactive so Guardian recovery does not depend on a desktop login.
# The ordinary logon trigger remains Interactive for session-sensitive host tooling.
Register-ScheduledTask -TaskName $startupTaskName -Action $action -Trigger $startupTrigger -Settings $settingsSet -Principal $startupPrincipal -Force | Out-Null
Register-ScheduledTask -TaskName $logonTaskName -Action $action -Trigger $logonTrigger -Settings $settingsSet -Principal $interactivePrincipal -Force | Out-Null
Register-ScheduledTask -TaskName $keepAliveTaskName -Action $action -Trigger $keepAliveTrigger -Settings $settingsSet -Principal $startupPrincipal -Force | Out-Null

# On-demand elevated MCP start (no UAC after registration). Used when a medium
# shell tries to start host-mode MCP and when agents need silent elevation.
$elevatedStartArgs = @('-Runtime', 'host')
if ($publicEnabled) { $elevatedStartArgs += '-Public' }
if ($oauthEnabled) { $elevatedStartArgs += '-OAuth' }
$elevatedVbsArgs = @('//B', '//NoLogo', ('"{0}"' -f $elevatedLauncher)) + $elevatedStartArgs
$elevatedAction = New-ScheduledTaskAction -Execute $wscriptExe -Argument ($elevatedVbsArgs -join ' ')
$elevatedSettings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 10)
Register-ScheduledTask -TaskName $elevatedStartTaskName -Action $elevatedAction -Settings $elevatedSettings -Principal $interactivePrincipal -Force | Out-Null

& $powerShellExe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $ensureScript | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "Guardian ensure process failed with exit code $LASTEXITCODE."
}

$taskInfo = @(
    Get-ScheduledTaskInfo -TaskName $startupTaskName | Select-Object @{Name = 'TaskName'; Expression = { $startupTaskName } }, LastRunTime, NextRunTime, LastTaskResult
Get-ScheduledTaskInfo -TaskName $logonTaskName | Select-Object @{Name = 'TaskName'; Expression = { $logonTaskName } }, LastRunTime, NextRunTime, LastTaskResult
    Get-ScheduledTaskInfo -TaskName $keepAliveTaskName | Select-Object @{Name = 'TaskName'; Expression = { $keepAliveTaskName } }, LastRunTime, NextRunTime, LastTaskResult
    Get-ScheduledTaskInfo -TaskName $elevatedStartTaskName | Select-Object @{Name = 'TaskName'; Expression = { $elevatedStartTaskName } }, LastRunTime, NextRunTime, LastTaskResult
)

Get-ScheduledTask -TaskName $startupTaskName, $logonTaskName, $keepAliveTaskName, $elevatedStartTaskName |
    Select-Object TaskName, State, @{Name = 'RunLevel'; Expression = { $_.Principal.RunLevel } } |
    Format-Table -AutoSize

$taskInfo |
    Select-Object TaskName, LastRunTime, NextRunTime, LastTaskResult |
    Format-Table -AutoSize
