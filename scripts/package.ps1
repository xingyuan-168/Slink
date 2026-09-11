param([string]$Destination, [switch]$SkipBuild, [string]$CMakePath)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -CMakePath $CMakePath
}
$taskBuild = Join-Path $taskRoot 'out/build/x86/Release'
if (-not $Destination) {
    $Destination = Join-Path $taskRoot ('out/packages/GameWindowMapper-x86-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$taskDestination = [IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $taskDestination) { throw 'Package destination already exists; choose a new directory. Existing files are never overwritten.' }
$taskBinaries = @('GameWindowMapper.dll','GameWindowMapper.lib','mapper_demo.exe','native_tests.exe','dynamic_source.exe')
foreach ($taskName in $taskBinaries) {
    if (-not (Test-Path -LiteralPath (Join-Path $taskBuild $taskName))) { throw "Missing build output: $taskName" }
}
New-Item -ItemType Directory -Path $taskDestination | Out-Null
foreach ($taskName in $taskBinaries) { Copy-Item -LiteralPath (Join-Path $taskBuild $taskName) -Destination $taskDestination }
Copy-Item -LiteralPath (Join-Path $taskRoot 'resources/GameWindowMapper.ico') -Destination $taskDestination
foreach ($taskDirectory in @('include','examples','docs')) {
    Copy-Item -LiteralPath (Join-Path $taskRoot $taskDirectory) -Destination $taskDestination -Recurse
}
Copy-Item -LiteralPath (Join-Path $taskRoot 'README.md') -Destination $taskDestination
$taskX64 = Join-Path $taskRoot 'out/build/x64-source/Release/dynamic_source.exe'
if (Test-Path -LiteralPath $taskX64) { Copy-Item -LiteralPath $taskX64 -Destination (Join-Path $taskDestination 'dynamic_source_x64.exe') }
$taskFiles = Get-ChildItem -LiteralPath $taskDestination -File -Recurse | Sort-Object FullName
$taskHashes = foreach ($taskFile in $taskFiles) {
    $taskRelative = $taskFile.FullName.Substring($taskDestination.Length + 1).Replace('\','/')
    (Get-FileHash -LiteralPath $taskFile.FullName -Algorithm SHA256).Hash + '  ' + $taskRelative
}
$taskHashes | Set-Content -LiteralPath (Join-Path $taskDestination 'SHA256SUMS.txt') -Encoding utf8
Write-Output "Package: $taskDestination"
