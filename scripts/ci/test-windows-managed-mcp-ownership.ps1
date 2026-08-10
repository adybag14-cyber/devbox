$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$startPath = Join-Path $root 'scripts\Start-ChatGptDevboxMcp.ps1'

$tokens = $null
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($startPath, [ref]$tokens, [ref]$errors)
if ($errors.Count -gt 0) {
    throw "PowerShell parser errors in $startPath`n$($errors | Out-String)"
}

foreach ($name in @('Test-IsOwnedServerCommandLine', 'Find-OwnedServerProcess', 'Stop-ExistingServerIfOwned')) {
    $functionAst = $ast.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true)
    if (-not $functionAst) { throw "Missing function $name in $startPath" }
    Invoke-Expression $functionAst.Extent.Text
}

# Stop-ExistingServerIfOwned calls this lifecycle helper. The ownership test has
# no startup deadline, so keep it as a no-op while exercising the real stop code.
function Assert-StartupDeadline { param([string]$Phase) }

$tempRoot = Join-Path $env:TEMP ("devbox-mcp-ownership-{0}-{1}" -f $PID, [guid]::NewGuid().ToString('N'))
$checkoutA = Join-Path $tempRoot 'checkout-a'
$checkoutB = Join-Path $tempRoot 'checkout-b'
$processes = New-Object System.Collections.Generic.List[System.Diagnostics.Process]

function Initialize-DummyCheckout {
    param([string]$Path)
    New-Item -ItemType Directory -Path (Join-Path $Path 'src') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $Path 'run') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $Path '.env.runtime') -Value '' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $Path 'src\server.js') -Value 'setInterval(() => {}, 1000);' -Encoding UTF8
}

function Start-DummyMcp {
    param(
        [string]$Path,
        [switch]$AbsoluteArguments
    )
    $nodeExe = (Get-Command node -ErrorAction Stop).Source
    $arguments = if ($AbsoluteArguments) {
        @(
            ("--env-file={0}" -f (Join-Path $Path '.env.runtime')),
            (Join-Path $Path 'src\server.js')
        )
    } else {
        @('--env-file=.env.runtime', 'src/server.js')
    }
    $process = Start-Process -FilePath $nodeExe -ArgumentList $arguments -WorkingDirectory $Path -PassThru -WindowStyle Hidden
    $processes.Add($process)
    Start-Sleep -Milliseconds 300
    return $process
}

try {
    Initialize-DummyCheckout -Path $checkoutA
    Initialize-DummyCheckout -Path $checkoutB

    # Reproduce the production incident: checkout A has a legacy relative JS MCP,
    # while checkout B has no PID file. B must never discover or stop A.
    $processA = Start-DummyMcp -Path $checkoutA
    $pidFileB = Join-Path $checkoutB 'run\mcp.pid'
    $foundForB = Find-OwnedServerProcess -PidFile $pidFileB -ProjectRoot $checkoutB
    if ($foundForB) {
        throw "Cross-checkout fallback incorrectly claimed PID $($foundForB.ProcessId): $($foundForB.CommandLine)"
    }
    Stop-ExistingServerIfOwned -PidFile $pidFileB -ProjectRoot $checkoutB
    if (-not (Get-Process -Id $processA.Id -ErrorAction SilentlyContinue)) {
        throw 'Cross-checkout stop killed checkout A MCP.'
    }

    # New managed launches use absolute arguments, so checkout-local recovery is
    # still possible even when its PID sidecar is missing.
    $processBAbsolute = Start-DummyMcp -Path $checkoutB -AbsoluteArguments
    $foundAbsolute = Find-OwnedServerProcess -PidFile $pidFileB -ProjectRoot $checkoutB
    if (-not $foundAbsolute -or [int]$foundAbsolute.ProcessId -ne $processBAbsolute.Id) {
        throw 'Absolute checkout-local MCP was not recoverable without its PID file.'
    }
    Stop-Process -Id $processBAbsolute.Id -Force
    Start-Sleep -Milliseconds 200

    # Existing installations can still have a relative JS command line. A valid
    # checkout-local PID file remains authoritative for that legacy shape.
    $processBLegacy = Start-DummyMcp -Path $checkoutB
    Set-Content -LiteralPath $pidFileB -Value $processBLegacy.Id -Encoding ASCII
    $foundLegacy = Find-OwnedServerProcess -PidFile $pidFileB -ProjectRoot $checkoutB
    if (-not $foundLegacy -or [int]$foundLegacy.ProcessId -ne $processBLegacy.Id) {
        throw 'Checkout-local PID file no longer recognizes a legacy relative JS MCP.'
    }

    Write-Host 'Windows managed MCP cross-checkout ownership checks passed.'
} finally {
    foreach ($process in $processes) {
        if (Get-Process -Id $process.Id -ErrorAction SilentlyContinue) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
    }
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}
