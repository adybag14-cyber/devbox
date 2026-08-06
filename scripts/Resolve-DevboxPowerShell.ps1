function Resolve-DevboxPowerShellExecutable {
    [CmdletBinding()]
    param(
        [string]$ConfiguredPath = $env:POWERSHELL_EXE,
        [string]$FallbackPath = $env:POWERSHELL_FALLBACK_EXE
    )

    $systemRoot = if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) { 'C:\Windows' } else { $env:SystemRoot }
    $programFiles = if ([string]::IsNullOrWhiteSpace($env:ProgramFiles)) { 'C:\Program Files' } else { $env:ProgramFiles }
    $legacy = Join-Path $systemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $pwsh7 = Join-Path $programFiles 'PowerShell\7\pwsh.exe'
    $candidates = @($ConfiguredPath, $pwsh7, $FallbackPath, $legacy, 'pwsh.exe', 'powershell.exe')

    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace([string]$candidate)) { continue }
        if ([System.IO.Path]::IsPathRooted([string]$candidate)) {
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { return [string]$candidate }
            continue
        }
        $command = Get-Command ([string]$candidate) -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($command) { return [string]$command.Source }
    }

    throw 'Neither PowerShell 7 nor Windows PowerShell 5.1 could be resolved.'
}
