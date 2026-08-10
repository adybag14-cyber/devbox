$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$startPath = Join-Path $root 'scripts\Start-ChatGptDevboxMcp.ps1'
$ownershipPath = Join-Path $root 'scripts\DevboxMcpOwnership.ps1'
. $ownershipPath

$tokens = $null
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($startPath, [ref]$tokens, [ref]$errors)
if ($errors.Count -gt 0) {
    throw "PowerShell parser errors in $startPath`n$($errors | Out-String)"
}

foreach ($name in @('Find-OwnedServerProcess', 'Stop-ExistingServerIfOwned')) {
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

function Wait-ForCimProcess {
    param(
        [Parameter(Mandatory = $true)][int]$ProcessId,
        [int]$TimeoutMilliseconds = 5000
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
    do {
        $candidate = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f $ProcessId) -ErrorAction SilentlyContinue
        if ($candidate -and -not [string]::IsNullOrWhiteSpace([string]$candidate.CommandLine)) {
            return $candidate
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "Process $ProcessId did not become visible through Win32_Process within ${TimeoutMilliseconds}ms."
}

function Wait-ForOwnedServerProcess {
    param(
        [Parameter(Mandatory = $true)][string]$PidFile,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][int]$ExpectedProcessId,
        [int]$TimeoutMilliseconds = 5000
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
    $lastFound = $null
    do {
        $lastFound = Find-OwnedServerProcess -PidFile $PidFile -ProjectRoot $ProjectRoot
        if ($lastFound -and [int]$lastFound.ProcessId -eq $ExpectedProcessId) {
            return $lastFound
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)

    $expected = Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f $ExpectedProcessId) -ErrorAction SilentlyContinue
    $lastDescription = if ($lastFound) {
        "PID $($lastFound.ProcessId): $($lastFound.CommandLine)"
    } else {
        '<none>'
    }
    $expectedDescription = if ($expected) { [string]$expected.CommandLine } else { '<missing>' }
    throw "Expected checkout-local MCP PID $ExpectedProcessId was not recovered within ${TimeoutMilliseconds}ms. Last match: $lastDescription. Expected command line: $expectedDescription"
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
    [void](Wait-ForCimProcess -ProcessId $process.Id)
    return $process
}

try {
    Initialize-DummyCheckout -Path $checkoutA
    Initialize-DummyCheckout -Path $checkoutB

    $programFilesShort = 'C:\PROGRA~1'
    if ((Test-Path -LiteralPath $programFilesShort) -and -not [string]::IsNullOrWhiteSpace($env:ProgramFiles)) {
        $shortComparable = Resolve-McpComparablePath -Path $programFilesShort
        $longComparable = Resolve-McpComparablePath -Path $env:ProgramFiles
        if ($shortComparable -ne $longComparable) {
            throw "8.3 path canonicalization mismatch: $programFilesShort -> $shortComparable; $env:ProgramFiles -> $longComparable"
        }
    }

    # Reproduce the production incident: checkout A has a legacy relative JS MCP,
    # while checkout B has no PID file. B must never discover or stop A.
    $processA = Start-DummyMcp -Path $checkoutA
    $pidFileB = Join-Path $checkoutB 'run\mcp.pid'
    Push-Location $checkoutB
    try {
        $foundForB = Find-OwnedServerProcess -PidFile $pidFileB -ProjectRoot $checkoutB
        if ($foundForB) {
            throw "Cross-checkout fallback incorrectly claimed PID $($foundForB.ProcessId): $($foundForB.CommandLine)"
        }
        Stop-ExistingServerIfOwned -PidFile $pidFileB -ProjectRoot $checkoutB
    } finally {
        Pop-Location
    }
    if (-not (Get-Process -Id $processA.Id -ErrorAction SilentlyContinue)) {
        throw 'Cross-checkout stop killed checkout A MCP.'
    }

    # New managed launches use absolute arguments, so checkout-local recovery is
    # still possible even when its PID sidecar is missing.
    $processBAbsolute = Start-DummyMcp -Path $checkoutB -AbsoluteArguments
    $absoluteCim = Wait-ForCimProcess -ProcessId $processBAbsolute.Id
    $absoluteArgs = @(Split-WindowsCommandLine -CommandLine ([string]$absoluteCim.CommandLine))
    $absoluteServer = Resolve-McpComparablePath -Path ([string]$absoluteArgs[$absoluteArgs.Count - 1])
    $expectedServer = (Resolve-McpComparablePath -Path $checkoutB) + '\src\server.js'
    if ($absoluteServer -ne $expectedServer) {
        throw "Windows command-line path canonicalization did not resolve to checkout B. Expected $expectedServer got $absoluteServer"
    }
    $foundAbsolute = Wait-ForOwnedServerProcess -PidFile $pidFileB -ProjectRoot $checkoutB -ExpectedProcessId $processBAbsolute.Id
    if ([int]$foundAbsolute.ProcessId -ne $processBAbsolute.Id) {
        throw 'Absolute checkout-local MCP recovery returned an unexpected PID.'
    }
    Stop-Process -Id $processBAbsolute.Id -Force
    if (-not $processBAbsolute.WaitForExit(5000)) {
        throw 'Absolute-argument fixture process did not exit after forced termination.'
    }

    # A PID file is not sufficient authority for a relative command line because
    # stale/reused PIDs cannot prove which checkout owns that process. Fail closed.
    $processBLegacy = Start-DummyMcp -Path $checkoutB
    Set-Content -LiteralPath $pidFileB -Value $processBLegacy.Id -Encoding ASCII
    $foundLegacy = Find-OwnedServerProcess -PidFile $pidFileB -ProjectRoot $checkoutB
    if ($foundLegacy) {
        throw "Relative command line was incorrectly authorized by PID metadata: $($foundLegacy.CommandLine)"
    }
    Stop-ExistingServerIfOwned -PidFile $pidFileB -ProjectRoot $checkoutB
    if (-not (Get-Process -Id $processBLegacy.Id -ErrorAction SilentlyContinue)) {
        throw 'Fail-closed PID recovery killed a relative command that could not prove checkout ownership.'
    }
    if (Test-Path -LiteralPath $pidFileB) {
        throw 'Rejected stale/unprovable PID metadata was not removed.'
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
