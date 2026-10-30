param([Parameter(Mandatory)][string]$OutputRoot)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$manifest=Get-Content -LiteralPath (Join-Path $root 'extension/manifest.json') -Raw|ConvertFrom-Json
$binary=Join-Path $root 'target/release/devbox-chrome-bridge.exe'
$version=(& $binary --version|Out-String).Trim()
if($LASTEXITCODE -ne 0 -or $version -ne "devbox-chrome-bridge $($manifest.version)"){throw 'Binary/extension version mismatch'}
$destination=Join-Path ([IO.Path]::GetFullPath($OutputRoot)) "devbox-chrome-bridge-$($manifest.version)-windows-x86_64"
if(Test-Path -LiteralPath $destination){throw 'Preserve existing package'}
New-Item -ItemType Directory -Path $destination -Force|Out-Null
Copy-Item -LiteralPath $binary,(Join-Path $root 'README.md'),(Join-Path $root 'Cargo.lock') -Destination $destination
Copy-Item -LiteralPath (Join-Path $root 'extension') -Destination $destination -Recurse
$metadataText=& cargo +1.91.1 metadata --manifest-path (Join-Path $root 'Cargo.toml') --locked --offline --format-version 1 --filter-platform x86_64-pc-windows-msvc
if($LASTEXITCODE -ne 0){throw 'Dependency license metadata unavailable'}
$metadata=$metadataText|ConvertFrom-Json
$resolved=@($metadata.resolve.nodes|ForEach-Object id)
$licenses=Join-Path $destination 'licenses'
New-Item -ItemType Directory -Path $licenses|Out-Null
$notices=@()
foreach($package in $metadata.packages|Where-Object{$_.source -and $_.id -in $resolved}){
  $packageDir=Split-Path -Parent $package.manifest_path
  $documents=@(Get-ChildItem -LiteralPath $packageDir -File|Where-Object Name -match '^(LICENSE|LICENCE|COPYING|NOTICE)([._-]|$)')
  if(!$documents.Count){throw "Missing license text for $($package.name) $($package.version)"}
  $licenseDir=Join-Path $licenses "$($package.name)-$($package.version)"
  New-Item -ItemType Directory -Path $licenseDir|Out-Null
  $documents|ForEach-Object{Copy-Item -LiteralPath $_.FullName -Destination $licenseDir}
  $notices+=@{name=$package.name;version=$package.version;license=$package.license;source=$package.source;files=@($documents.Name)}
}
$notices|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $licenses 'dependencies.json')
$source=(git -C $root rev-parse HEAD|Out-String).Trim()
if($LASTEXITCODE -ne 0){throw 'Source identity unavailable'}
$sourceTree=(git -C $root rev-parse 'HEAD^{tree}'|Out-String).Trim()
if($LASTEXITCODE -ne 0){throw 'Source tree identity unavailable'}
$dirty=!!(git -C $root status --porcelain)
$files=@(Get-ChildItem -LiteralPath $destination -Recurse -File|ForEach-Object{[ordered]@{path=[IO.Path]::GetRelativePath($destination,$_.FullName);bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}})
[ordered]@{version=$manifest.version;source=$source;sourceTree=$sourceTree;sourceDirty=$dirty;extensionId=(Get-Content -LiteralPath (Join-Path $root 'extension/id.txt') -Raw).Trim();files=$files;qualification='See exact source CI and separate live Chrome receipts; package assembly alone does not qualify UI behavior.'}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $destination 'manifest-sha256.json')
$archive=$destination+'.zip'
Compress-Archive -LiteralPath $destination -DestinationPath $archive
$archiveHash=(Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant()
$output=[IO.Path]::GetFullPath($OutputRoot)
Copy-Item -LiteralPath (Join-Path $destination 'manifest-sha256.json') -Destination (Join-Path $output 'chrome-bridge-manifest.json')
[IO.File]::WriteAllText((Join-Path $output 'chrome-bridge-SHA256SUMS'),$archiveHash+'  '+[IO.Path]::GetFileName($archive)+"`n",[Text.UTF8Encoding]::new($false))
[ordered]@{directory=$destination;archive=$archive;sha256=$archiveHash;version=$manifest.version;source=$source;sourceTree=$sourceTree;sourceDirty=$dirty}|ConvertTo-Json
