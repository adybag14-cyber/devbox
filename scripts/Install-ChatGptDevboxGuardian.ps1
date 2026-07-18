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
$settingsPath = Join-Path $runDir 'guardian.settings.json'
$desiredStatePath = Join-Path $runDir 'guardian.desired-state.json'
$powerShellExe = 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'
$userId = '{0}\{1}' -f $env:USERDOMAIN, $env:USERNAME
$logonTaskName = "$TaskPrefix-Logon"
$keepAliveTaskName = "$TaskPrefix-KeepAlive"

foreach ($path in @($runDir, $guardianDir)) {
    if (-not (Test-Path $path)) {
        New-Item -ItemType Directory -Path $path | Out-Null
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

    $Value | ConvertTo-Json -Depth 8 | Set-Content -Path $Path -Encoding UTF8
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

foreach ($taskName in @($logonTaskName, $keepAliveTaskName)) {
    Remove-TaskIfPresent -TaskName $taskName
}

$action = New-ScheduledTaskAction -Execute $powerShellExe -Argument "-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$ensureScript`" -ProjectRoot `"$projectRoot`""
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $userId
$settingsSet = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId $userId -LogonType Interactive -RunLevel Highest

Register-ScheduledTask -TaskName $logonTaskName -Action $action -Trigger $trigger -Settings $settingsSet -Principal $principal -Force | Out-Null

$taskCommand = ('"{0}" -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File "{1}" -ProjectRoot "{2}"' -f $powerShellExe, $ensureScript, $projectRoot)
$startTime = (Get-Date).AddMinutes(1).ToString('HH:mm')
$createMinuteTask = & schtasks.exe /Create /TN $keepAliveTaskName /SC MINUTE /MO 1 /ST $startTime /TR $taskCommand /RL HIGHEST /F
if ($LASTEXITCODE -ne 0) {
    throw ($createMinuteTask | Out-String)
}

& $powerShellExe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $ensureScript -ProjectRoot $projectRoot | Out-Null
Start-Sleep -Seconds 5

$taskInfo = @(
    Get-ScheduledTaskInfo -TaskName $logonTaskName | Select-Object @{Name = 'TaskName'; Expression = { $logonTaskName } }, LastRunTime, NextRunTime, LastTaskResult
    Get-ScheduledTaskInfo -TaskName $keepAliveTaskName | Select-Object @{Name = 'TaskName'; Expression = { $keepAliveTaskName } }, LastRunTime, NextRunTime, LastTaskResult
)

Get-ScheduledTask -TaskName $logonTaskName, $keepAliveTaskName |
    Select-Object TaskName, State, Author |
    Format-Table -AutoSize

$taskInfo |
    Select-Object TaskName, LastRunTime, NextRunTime, LastTaskResult |
    Format-Table -AutoSize
