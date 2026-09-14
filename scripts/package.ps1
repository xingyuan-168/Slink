param([string]$Destination, [switch]$SkipBuild, [string]$CMakePath)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -CMakePath $CMakePath
}
$taskBuild = Join-Path $taskRoot 'out/build/x86/Release'
if (-not $Destination) {
    $Destination = Join-Path $taskRoot 'output'
}
$taskDestination = [IO.Path]::GetFullPath($Destination)
$taskOutputRoot = [IO.Path]::GetFullPath((Join-Path $taskRoot 'output'))
if ($taskDestination -ne $taskOutputRoot -and -not $taskDestination.StartsWith($taskOutputRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'All delivery files must be placed in the repository output/ directory.'
}
$taskBinaries = @('GameWindowMapper.dll','GameWindowMapper.lib','mapper_demo.exe','native_tests.exe','dynamic_source.exe','gpu_geometry_tests.exe','capture_geometry_tests.exe')
foreach ($taskName in $taskBinaries) {
    if (-not (Test-Path -LiteralPath (Join-Path $taskBuild $taskName))) { throw "Missing build output: $taskName" }
}
New-Item -ItemType Directory -Path $taskDestination -Force | Out-Null
foreach ($taskName in $taskBinaries) { Copy-Item -LiteralPath (Join-Path $taskBuild $taskName) -Destination $taskDestination }
Copy-Item -LiteralPath (Join-Path $taskRoot 'resources/GameWindowMapper.ico') -Destination $taskDestination
foreach ($taskDirectory in @('include','examples','docs','src','tests','scripts','resources')) {
    Copy-Item -LiteralPath (Join-Path $taskRoot $taskDirectory) -Destination $taskDestination -Recurse
}
Copy-Item -LiteralPath (Join-Path $taskRoot 'README.md') -Destination $taskDestination
foreach($taskName in @('CMakeLists.txt','CMakePresets.json')) { Copy-Item -LiteralPath (Join-Path $taskRoot $taskName) -Destination $taskDestination }
New-Item -ItemType Directory -Path (Join-Path $taskDestination 'input') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $taskRoot 'input/LOGO.png') -Destination (Join-Path $taskDestination 'input/LOGO.png')
$taskX64 = Join-Path $taskRoot 'out/build/x64-source/Release/dynamic_source.exe'
if (Test-Path -LiteralPath $taskX64) { Copy-Item -LiteralPath $taskX64 -Destination (Join-Path $taskDestination 'dynamic_source_x64.exe') }
$taskFiles = Get-ChildItem -LiteralPath $taskDestination -File -Recurse | Where-Object { $_.Name -notin @('.gitkeep','SHA256SUMS.txt') } | Sort-Object FullName
$taskHashes = foreach ($taskFile in $taskFiles) {
    $taskRelative = $taskFile.FullName.Substring($taskDestination.Length + 1).Replace('\','/')
    (Get-FileHash -LiteralPath $taskFile.FullName -Algorithm SHA256).Hash + '  ' + $taskRelative
}
$taskHashes | Set-Content -LiteralPath (Join-Path $taskDestination 'SHA256SUMS.txt') -Encoding utf8
Write-Output "Package: $taskDestination"
