$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$startPath = Join-Path $root 'scripts\Start-ChatGptDevboxMcp.ps1'
$installPath = Join-Path $root 'scripts\Install-ChatGptDevboxGuardian.ps1'
$vbsPath = Join-Path $root 'scripts\Run-Start-ChatGptDevboxMcp.vbs'

function Assert-Contains {
    param([string]$Text, [string]$Needle, [string]$Message)
    if (-not $Text.Contains($Needle)) { throw $Message }
}

function Assert-Before {
    param([string]$Text, [string]$First, [string]$Second, [string]$Message)
    $a = $Text.IndexOf($First, [StringComparison]::Ordinal)
    $b = $Text.IndexOf($Second, [StringComparison]::Ordinal)
    if ($a -lt 0 -or $b -lt 0 -or $a -ge $b) { throw $Message }
}

$start = Get-Content -LiteralPath $startPath -Raw
$install = Get-Content -LiteralPath $installPath -Raw
$vbs = Get-Content -LiteralPath $vbsPath -Raw

Assert-Contains $start '$script:lifecycleMutex.WaitOne(0, $false)' 'Lifecycle mutex must reject concurrent starts instead of queueing them.'
Assert-Contains $start "Join-Path `$RunDir 'startup-state.json'" 'Startup phase journal is missing.'
Assert-Contains $start "Write-StartupPhase -Phase 'waiting-local-health'" 'Local-health phase journaling is missing.'
Assert-Contains $start "Write-StartupPhase -Phase 'waiting-public-health'" 'Public-health phase journaling is missing.'
Assert-Contains $start 'Assert-StartupDeadline' 'Startup deadline enforcement is missing.'
Assert-Contains $start "Assert-StartupDeadline -Phase 'stopping-existing-mcp'" 'Owned MCP stop must honor the startup deadline.'
Assert-Contains $start "Assert-StartupDeadline -Phase 'preflighting-mcp-replacement'" 'MCP replacement preflight is missing.'
Assert-Contains $start '$value = if ([string]::IsNullOrWhiteSpace($ConfiguredValue)) { ''rust'' }' 'Rust must be the managed MCP default when no implementation override is configured.'
Assert-Contains $start '$value -notin @(''rust'', ''js'')' 'Managed MCP implementation selection must reject unknown values.'
Assert-Contains $start 'if ([IO.Path]::IsPathRooted($candidate) -and (Test-Path -LiteralPath $candidate))' 'Cargo resolution must not execute a bare cargo.exe from the current directory.'
Assert-Contains $start '$runtimeEnvMatches = [IO.Path]::IsPathRooted($runtimePath)' 'Checkout-scoped ownership must reject relative runtime-env paths.'
Assert-Contains $start 'if ([IO.Path]::IsPathRooted($argument)' 'Checkout-scoped ownership must reject relative server paths.'
Assert-Contains $start '& $cargoExe ''build'' ''--manifest-path'' $manifestPath ''--target-dir'' $targetDir ''--release'' ''--locked''' 'Rust replacement preflight must build the locked release binary into its canonical target directory.'
Assert-Contains $start '& $binaryPath ''--parity-report''' 'Rust replacement preflight must probe the built binary before cutover.'
Assert-Contains $start "const dependencies = ['express', '@modelcontextprotocol/sdk/server/mcp.js', 'zod/v4', 'jose'];" 'JS rollback dependency preflight must remain available.'
Assert-Before $start '$launchSpec = Assert-McpReplacementReady -Implementation $mcpImplementation -ProjectRoot $root -RuntimeEnvFile $runtimeEnvFile' "Write-StartupPhase -Phase 'stopping-existing-mcp'" 'The selected Rust/JS implementation must be fully preflighted before the existing MCP is stopped.'
Assert-Contains $start '$expectedRust = "$normalizedRoot\rust-mcp\target\release\devbox-mcp.exe"' 'Managed ownership checks must recognize only the current checkout Rust MCP binary.'
Assert-Contains $start 'Where-Object { Test-IsOwnedServerCommandLine -CommandLine ([string]$_.CommandLine) -ProjectRoot $ProjectRoot }' 'Fallback MCP ownership discovery must be scoped to the current checkout root.'
Assert-Contains $start 'Test-IsOwnedServerCommandLine -CommandLine ([string]$candidate.CommandLine) -ProjectRoot $ProjectRoot' 'PID-file recovery must re-prove checkout-specific ownership.'
Assert-Contains $start 'ArgumentList = @(("--env-file={0}" -f $RuntimeEnvFile), $serverPath)' 'JS rollback launches must use absolute checkout-local runtime and server paths.'
Assert-Contains $start 'Set-Content -Path $implementationFile -Value $mcpImplementation -Encoding ASCII' 'Managed startup must persist which MCP implementation owns the PID.'
Assert-Contains $start '$process = Start-Process @startProcessParameters' 'Managed startup must launch the selected implementation spec rather than a hard-coded Node command.'
Assert-Contains $start '$childEnvironment = Read-RuntimeEnvValues -FilePath $runtimeEnvFile' 'Managed startup must load the generated runtime environment before spawning either implementation.'
Assert-Contains $start "`$childEnvironment['DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE'] = '1'" 'Managed Rust startup must mark .env.runtime authoritative over inherited production environment values.'
Assert-Before $start '$previousChildEnvironment = Set-TemporaryProcessEnvironment -Values $childEnvironment' '$process = Start-Process @startProcessParameters' 'Managed child runtime values must override inherited process environment before spawn.'
Assert-Contains $start 'Restore-TemporaryProcessEnvironment -Previous $previousChildEnvironment' 'Managed startup must restore the parent process environment after spawning the child.'
Assert-Contains $start '$Public = $true' 'Tunnel-only repair must imply the configured public tunnel.'
Assert-Contains $start '@(''--protocol'', $transportProtocol)' 'Explicit Cloudflare transport protocol support is missing.'
Assert-Contains $start '@(''--edge-bind-address'', $effectiveEdgeBindAddress)' 'Explicit Cloudflare edge bind support is missing.'
Assert-Contains $start 'function Resolve-DefaultRouteIPv4BindAddress' 'DHCP-aware physical default-route bind resolution is missing.'
Assert-Contains $start '$adapter.HardwareInterface -ne $true' 'Automatic tunnel binding must avoid virtual adapters.'
Assert-Contains $start "$requestedEdgeBindAddress -eq 'auto'" 'Cloudflare edge bind auto mode is missing.'
Assert-Contains $start "'configured-stale-default-route'" 'Stale configured DHCP addresses must fall back to the active physical default route.'
Assert-Contains $start 'BindInterfaceAlias = $bindInterfaceAlias' 'Tunnel transport state must record the resolved interface.'
Assert-Contains $start '$originStillHealthy = $originCheck.Content -match ''ok''' 'Failed full startup must preserve the named tunnel when the existing MCP origin is still healthy.'
Assert-Contains $start 'for ($i = 0; $i -lt 16; $i++)' 'Owned MCP stop must use a bounded short polling window.'
Assert-Contains $start '$script:startupMcpPid = [int]$process.Id' 'Spawned MCP PID ownership tracking is missing.'
Assert-Contains $start 'Stop-Process -Id ([int]$script:startupMcpPid) -Force' 'Failed startup must clean up the spawned MCP process.'
Assert-Before $start 'Set-Content -Path $pidFile -Value $process.Id -Encoding ASCII' '$localUrl = "http://127.0.0.1:$port"' 'MCP PID must be persisted before health validation begins.'
Assert-Contains $vbs 'shell.Run(command, 0, True)' 'Scheduled-task VBS must wait for the real startup child and propagate its exit code.'
Assert-Contains $install '-ExecutionTimeLimit (New-TimeSpan -Minutes 10)' 'Elevated startup task must have a bounded execution time.'

foreach ($file in @($startPath, $installPath)) {
    $tokens = $null
    $errors = $null
    [System.Management.Automation.Language.Parser]::ParseFile($file, [ref]$tokens, [ref]$errors) | Out-Null
    if ($errors.Count -gt 0) {
        throw "PowerShell parser errors in $file`n$($errors | Out-String)"
    }
}

$legacy = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
if (Test-Path $legacy) {
    $escapedStart = $startPath.Replace("'", "''")
    $escapedInstall = $installPath.Replace("'", "''")
    $command = @"
`$ErrorActionPreference='Stop'; foreach(`$f in @('$escapedStart','$escapedInstall')) { `$t=`$null; `$e=`$null; [System.Management.Automation.Language.Parser]::ParseFile(`$f,[ref]`$t,[ref]`$e) | Out-Null; if(`$e.Count -gt 0) { throw (`$e | Out-String) } }
"@
    & $legacy -NoLogo -NoProfile -NonInteractive -Command $command
    if ($LASTEXITCODE -ne 0) { throw "Windows PowerShell 5.1 parse validation failed with exit code $LASTEXITCODE." }
}

Write-Host 'Windows Devbox startup lifecycle contract checks passed.'
