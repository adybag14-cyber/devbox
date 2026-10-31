param([string]$StatusPath,[string]$ReadyPath,[string]$ReleasePath)
$ErrorActionPreference='Stop'
$handle=[IO.File]::Open($StatusPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
try {
  [IO.File]::WriteAllText($ReadyPath,'ready')
  $watch=[Diagnostics.Stopwatch]::StartNew()
  while(-not [IO.File]::Exists($ReleasePath) -and $watch.Elapsed.TotalSeconds -lt 15){Start-Sleep -Milliseconds 10}
  if(-not [IO.File]::Exists($ReleasePath)){throw 'Status-lock fixture release timed out'}
} finally {$handle.Dispose()}
