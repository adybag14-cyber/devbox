$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$startPath = Join-Path $root 'scripts\Start-ChatGptDevboxMcp.ps1'
$stopPath = Join-Path $root 'scripts\Stop-ChatGptDevboxMcp.ps1'
$ownershipPath = Join-Path $root 'scripts\DevboxMcpOwnership.ps1'
$installPath = Join-Path $root 'scripts\Install-ChatGptDevboxGuardian.ps1'
$ensureGuardianPath = Join-Path $root 'scripts\Ensure-ChatGptDevboxGuardian.ps1'
$watchGuardianPath = Join-Path $root 'scripts\Watch-ChatGptDevboxGuardian.ps1'
$guardianSupervisorPath = Join-Path $root 'scripts\devbox-guardian.mjs'
$vbsPath = Join-Path $root 'scripts\Run-Start-ChatGptDevboxMcp.vbs'

function Assert-Contains {
    param([string]$Text, [string]$Needle, [string]$Message)
    if (-not $Text.Contains($Needle)) { throw $Message }
}

function Assert-NotContains {
    param([string]$Text, [string]$Needle, [string]$Message)
    if ($Text.Contains($Needle)) { throw $Message }
}

function Assert-Before {
    param([string]$Text, [string]$First, [string]$Second, [string]$Message)
    $a = $Text.IndexOf($First, [StringComparison]::Ordinal)
    $b = $Text.IndexOf($Second, [StringComparison]::Ordinal)
    if ($a -lt 0 -or $b -lt 0 -or $a -ge $b) { throw $Message }
}

$start = Get-Content -LiteralPath $startPath -Raw
$stop = Get-Content -LiteralPath $stopPath -Raw
$ownership = Get-Content -LiteralPath $ownershipPath -Raw
$install = Get-Content -LiteralPath $installPath -Raw
$ensureGuardian = Get-Content -LiteralPath $ensureGuardianPath -Raw
$watchGuardian = Get-Content -LiteralPath $watchGuardianPath -Raw
$guardianSupervisor = Get-Content -LiteralPath $guardianSupervisorPath -Raw
$vbs = Get-Content -LiteralPath $vbsPath -Raw

Assert-Contains $start '$script:lifecycleMutex.WaitOne(0, $false)' 'Lifecycle mutex must reject concurrent starts instead of queueing them.'
Assert-Contains $start "Join-Path `$RunDir 'startup-state.json'" 'Startup phase journal is missing.'
Assert-Contains $start "Write-StartupPhase -Phase 'waiting-local-health'" 'Local-health phase journaling is missing.'
Assert-Contains $start '/readyz' 'Managed startup must gate candidate acceptance on operational readiness, not only liveness.'
Assert-Contains $start 'FirstPromotedAtUtc' 'Rust candidate provenance must preserve first promotion time.'
Assert-Contains $start 'LastStartedAtUtc' 'Rust candidate provenance must distinguish process restart time from promotion time.'
Assert-Contains $start "Write-StartupPhase -Phase 'waiting-public-health'" 'Public-health phase journaling is missing.'
Assert-Contains $start 'Assert-StartupDeadline' 'Startup deadline enforcement is missing.'
Assert-Contains $start "Assert-StartupDeadline -Phase 'stopping-existing-mcp'" 'Owned MCP stop must honor the startup deadline.'
Assert-Contains $start "Assert-StartupDeadline -Phase 'preflighting-mcp-replacement'" 'MCP replacement preflight is missing.'
Assert-Contains $start 'Get-EnvValue -FilePath $envFile -Name ''DEVBOX_MCP_PREFLIGHT_TIMEOUT_SECONDS''' 'MCP replacement preflight needs an independent non-destructive timeout budget.'
Assert-Contains $start 'Reset-StartupDeadlineWindow -TimeoutSeconds $preflightTimeoutSeconds -Phase ''preflighting-mcp-replacement''' 'Cold candidate builds must receive their own preflight deadline window.'
Assert-Contains $start 'Reset-StartupDeadlineWindow -TimeoutSeconds $startupTimeoutSeconds -Phase ''cutover-ready''' 'The destructive handover must refresh its deadline after candidate preflight.'
Assert-Contains $start '$value = if ([string]::IsNullOrWhiteSpace($ConfiguredValue)) { ''rust'' }' 'Rust must be the managed MCP default when no implementation override is configured.'
Assert-Contains $start '$value -notin @(''cpp'', ''rust'', ''js'')' 'Managed MCP implementation selection must reject unknown values.'
Assert-Contains $start ". (Join-Path `$PSScriptRoot 'DevboxMcpOwnership.ps1')" 'Start lifecycle must load the shared ownership classifier.'
Assert-Contains $stop ". (Join-Path `$PSScriptRoot 'DevboxMcpOwnership.ps1')" 'Stop lifecycle must load the shared ownership classifier.'
Assert-Contains $start 'if ([IO.Path]::IsPathRooted($candidate) -and (Test-Path -LiteralPath $candidate))' 'Cargo resolution must not execute a bare cargo.exe from the current directory.'
Assert-Contains $ownership '$runtimeEnvMatches = (Test-IsPathRootedSafe -Path $runtimePath) -and' 'Checkout-scoped ownership must reject relative runtime-env paths without throwing on malformed tokens.'
Assert-Contains $ownership 'if ((Test-IsPathRootedSafe -Path $argument) -and' 'Checkout-scoped ownership must reject relative server paths without throwing on malformed tokens.'
Assert-Contains $start '& $cargoExe ''build'' ''--manifest-path'' $manifestPath ''--target-dir'' $targetDir ''--release'' ''--locked''' 'Rust replacement preflight must build the locked release binary into its canonical target directory.'
Assert-Contains $start '& $binaryPath ''--parity-report''' 'Rust replacement preflight must probe the built binary before cutover.'
Assert-Contains $start "const dependencies = ['express', '@modelcontextprotocol/sdk/server/mcp.js', 'zod/v4', 'jose'];" 'JS rollback dependency preflight must remain available.'
Assert-Before $start '$launchSpec = Assert-McpReplacementReady -Implementation $mcpImplementation -ProjectRoot $root -RuntimeEnvFile $runtimeEnvFile' "Write-StartupPhase -Phase 'stopping-existing-mcp'" 'The selected Rust/JS implementation must be fully preflighted before the existing MCP is stopped.'
Assert-Before $start 'Reset-StartupDeadlineWindow -TimeoutSeconds $preflightTimeoutSeconds -Phase ''preflighting-mcp-replacement''' '$launchSpec = Assert-McpReplacementReady -Implementation $mcpImplementation -ProjectRoot $root -RuntimeEnvFile $runtimeEnvFile' 'The long preflight window must begin before candidate build/parity work.'
Assert-Before $start '$launchSpec = Assert-McpReplacementReady -Implementation $mcpImplementation -ProjectRoot $root -RuntimeEnvFile $runtimeEnvFile' 'Reset-StartupDeadlineWindow -TimeoutSeconds $startupTimeoutSeconds -Phase ''cutover-ready''' 'The downtime deadline must not start until candidate preflight succeeds.'
Assert-Before $start 'Reset-StartupDeadlineWindow -TimeoutSeconds $startupTimeoutSeconds -Phase ''cutover-ready''' "Write-StartupPhase -Phase 'stopping-existing-mcp'" 'The cutover deadline must be refreshed immediately before stopping the old MCP.'
Assert-Contains $ownership '$expectedRust = "$normalizedRoot\rust-mcp\target\release\devbox-mcp.exe"' 'Managed ownership checks must recognize only the current checkout Rust MCP binary.'
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
Assert-Contains $start 'function Remove-StaleAtomicTempFiles' 'Startup must prune abandoned top-level atomic temp files.'
Assert-Contains $start 'Remove-StaleAtomicTempFiles -RunDir $runDir' 'Startup must invoke stale atomic temp cleanup.'
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
Assert-Contains $install 'if ($LASTEXITCODE -ne 0)' 'Guardian installation must fail when the Ensure process fails.'
Assert-Contains $install 'New-ScheduledTaskAction -Execute $nodeExe -Argument $startupActionArgs' 'Guardian AtStartup must launch Node directly.'
Assert-Contains $install "'--direct-owner'" 'Direct AtStartup Guardian must ignore inherited legacy wrapper ownership.'
Assert-Contains $install 'New-ScheduledTaskAction -Execute $powerShellExe -Argument $ensureActionArgs' 'Guardian recovery tasks must retain the Ensure path.'
Assert-Contains $install '-ExecutionTimeLimit ([TimeSpan]::Zero)' 'Persistent AtStartup Guardian task must not be killed by a short execution limit.'
Assert-Contains $install 'Register-ScheduledTask -TaskName $keepAliveTaskName -Action $action -Trigger $keepAliveTrigger -Settings $settingsSet -Principal $startupPrincipal' 'Guardian KeepAlive must run non-interactively under the S4U principal.'
Assert-NotContains $install 'schtasks.exe /Create /TN $keepAliveTaskName' 'Guardian KeepAlive must not fall back to the interactive schtasks path.'
Assert-Contains $ensureGuardian 'function Get-EnsureMutexName' 'Guardian Ensure invocations must share a project-root-scoped mutex.'
Assert-Contains $ensureGuardian 'Global\ChatGptDevboxGuardianEnsure-' 'Guardian Ensure mutex must use a root-derived global name.'
Assert-NotContains $ensureGuardian 'Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |' 'Guardian Ensure must not enumerate every process during boot recovery.'
Assert-Contains $ensureGuardian '$escapedGuardianPath = [regex]::Escape($guardianScript)' 'Guardian legacy watcher ownership must be scoped to the current checkout path.'
Assert-Contains $ensureGuardian "'--project-root', ('`"{0}`"' -f `$ProjectRoot)" 'Guardian direct Node launch must quote the project root as one argument.'
Assert-Contains $ensureGuardian "('`"{0}`"' -f `$supervisorScript)" 'Guardian direct Node launch must quote the supervisor path as one argument.'
Assert-Contains $ensureGuardian "'--direct-owner'" 'Direct Ensure Guardian must ignore inherited legacy wrapper ownership.'
Assert-Contains $ensureGuardian "CreationDateUtc" 'Guardian stale corroboration must bind to a process instance, not PID alone.'
Assert-Contains $ensureGuardian "TotalMinutes -le 30" 'Guardian stale corroboration must survive the 10-minute KeepAlive cadence.'
Assert-Contains $ensureGuardian 'Start-Process -FilePath $nodeExe -ArgumentList $arguments -WorkingDirectory $ProjectRoot -WindowStyle Hidden' 'Guardian Ensure must launch the Node supervisor directly.'
Assert-Contains $ensureGuardian 'function Get-LiveGuardianSupervisorProcess' 'Guardian Ensure must identify a verified orphan supervisor separately from the watcher.'
Assert-Contains $ensureGuardian 'guardian failed to start persistently' 'Guardian Ensure must require a persistent Node supervisor before reporting success.'
Assert-Before $guardianSupervisor 'await writeFile(paths.pid' 'findMcpProcess(paths.runDir)' 'Guardian must publish its PID before potentially slow Windows process discovery.'
Assert-Contains $guardianSupervisor 'const publishedPid = await readPid(paths.pid)' 'Guardian shutdown must verify PID ownership before deleting its discoverability file.'
Assert-Contains $watchGuardian 'function Get-GuardianMutexName' 'Guardian mutex ownership must be scoped to the project root.'
Assert-Contains $watchGuardian 'Global\ChatGptDevboxGuardian-' 'Guardian mutex must use a root-derived namespace.'

foreach ($file in @($startPath, $stopPath, $ownershipPath, $installPath, $ensureGuardianPath, $watchGuardianPath)) {
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
    $escapedStop = $stopPath.Replace("'", "''")
    $escapedOwnership = $ownershipPath.Replace("'", "''")
    $escapedInstall = $installPath.Replace("'", "''")
    $escapedEnsureGuardian = $ensureGuardianPath.Replace("'", "''")
    $escapedWatchGuardian = $watchGuardianPath.Replace("'", "''")
    $command = @"
`$ErrorActionPreference='Stop'; foreach(`$f in @('$escapedStart','$escapedStop','$escapedOwnership','$escapedInstall','$escapedEnsureGuardian','$escapedWatchGuardian')) { `$t=`$null; `$e=`$null; [System.Management.Automation.Language.Parser]::ParseFile(`$f,[ref]`$t,[ref]`$e) | Out-Null; if(`$e.Count -gt 0) { throw (`$e | Out-String) } }
"@
    & $legacy -NoLogo -NoProfile -NonInteractive -Command $command
    if ($LASTEXITCODE -ne 0) { throw "Windows PowerShell 5.1 parse validation failed with exit code $LASTEXITCODE." }
}

$sourceAst = [System.Management.Automation.Language.Parser]::ParseFile($startPath, [ref]$null, [ref]$null)
foreach ($name in @('Get-RustSourceIdentity', 'Test-RustSourceIdentity')) {
    $function = $sourceAst.Find({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name }, $true)
    if (-not $function) { throw "Missing source provenance helper $name" }
    . ([scriptblock]::Create($function.Extent.Text))
}
$sourceFixture = Join-Path ([IO.Path]::GetTempPath()) ('devbox-source-contract-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $sourceFixture 'rust-mcp\src') -Force | Out-Null
try {
    git -C $sourceFixture init --quiet
    git -C $sourceFixture config user.email 'fixture@example.test'
    git -C $sourceFixture config user.name 'Devbox fixture'
    $sourceFile = Join-Path $sourceFixture 'rust-mcp\src\main.rs'
    [IO.File]::WriteAllText($sourceFile, 'fn main() {}')
    git -C $sourceFixture add .
    git -C $sourceFixture -c commit.gpgsign=false commit --quiet -m fixture
    if ($LASTEXITCODE -ne 0) { throw 'Could not prepare provenance fixture repository' }
    $clean = Get-RustSourceIdentity -ProjectRoot $sourceFixture
    if (-not (Test-RustSourceIdentity -BuildInfo $clean -GitSha $clean.GitSha -SourceTree $clean.SourceTree)) { throw 'Clean provenance was not accepted' }
    [IO.File]::AppendAllText($sourceFile, '// dirty input')
    $refused = $false
    try { Get-RustSourceIdentity -ProjectRoot $sourceFixture | Out-Null } catch { $refused = $_.Exception.Message -match 'clean tracked source' }
    if (-not $refused) { throw 'Dirty source was not refused' }
    git -C $sourceFixture restore -- 'rust-mcp/src/main.rs'
    $restored = Get-RustSourceIdentity -ProjectRoot $sourceFixture
    if ($restored.GitSha -ne $clean.GitSha) { throw 'Fixture HEAD unexpectedly changed' }
    foreach ($candidate in @(
        [pscustomobject]@{ GitSha=$clean.GitSha; SourceTree=$clean.SourceTree; SourceDirty=$true },
        [pscustomobject]@{ GitSha=$clean.GitSha; SourceTree=$clean.SourceTree; SourceDirty='false' },
        [pscustomobject]@{ GitSha=$clean.GitSha },
        [pscustomobject]@{ GitSha=$clean.GitSha; SourceTree=('0'*40); SourceDirty=$false }
    )) { if (Test-RustSourceIdentity -BuildInfo $candidate -GitSha $restored.GitSha -SourceTree $restored.SourceTree) { throw 'Untrusted candidate provenance was accepted after restoring the same HEAD' } }
    [IO.File]::WriteAllText((Join-Path $sourceFixture 'rust-mcp\src\untracked.rs'), '// hidden build input')
    $refused = $false
    try { Get-RustSourceIdentity -ProjectRoot $sourceFixture | Out-Null } catch { $refused = $_.Exception.Message -match 'untracked Rust build inputs' }
    if (-not $refused) { throw 'Untracked Rust source was not refused' }
} finally {
    $resolvedFixture = [IO.Path]::GetFullPath($sourceFixture)
    $tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolvedFixture.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolvedFixture) -notlike 'devbox-source-contract-*') { throw 'Unsafe provenance fixture cleanup target' }
    Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
}

Write-Host 'Windows Devbox startup lifecycle and source provenance contract checks passed.'
