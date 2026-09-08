param([Parameter(Mandatory = $true)][string]$VcpkgRoot)
$ErrorActionPreference = 'Stop'
$scriptPath = Join-Path ([IO.Path]::GetFullPath($VcpkgRoot)) 'bootstrap-vcpkg.bat'
if (-not (Test-Path -LiteralPath $scriptPath -PathType Leaf)) { throw "Missing vcpkg bootstrap: $scriptPath" }
& $scriptPath '-disableMetrics'
if ($LASTEXITCODE -ne 0) { throw "vcpkg bootstrap failed with exit code $LASTEXITCODE" }
