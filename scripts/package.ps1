param([string]$Destination, [switch]$SkipBuild, [string]$CMakePath)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskOutput = [IO.Path]::GetFullPath((Join-Path $taskRoot 'output'))
if ($Destination -and [IO.Path]::GetFullPath($Destination) -ne $taskOutput) {
    throw 'Delivery must be the project output/ directory.'
}
if (-not $SkipBuild) { & (Join-Path $PSScriptRoot 'build.ps1') -CMakePath $CMakePath }
$taskDll = Join-Path $taskRoot 'out/build/x86/Release/GameWindowMapper.dll'
$taskGuide = Join-Path $taskRoot 'docs/E_LANGUAGE.md'
foreach ($taskSource in @($taskDll,$taskGuide)) {
    if (-not (Test-Path -LiteralPath $taskSource -PathType Leaf)) { throw "Missing source: $taskSource" }
}
$taskAllowed = @('GameWindowMapper.dll','使用说明.md')
if (Test-Path -LiteralPath $taskOutput) {
    $taskUnexpected = @(Get-ChildItem -LiteralPath $taskOutput -Force | Where-Object {
        $_.PSIsContainer -or $_.Name -notin $taskAllowed -or ($_.Attributes -band [IO.FileAttributes]::ReparsePoint)
    })
    if ($taskUnexpected.Count) { throw ('Unexpected output files; preserve and organize them before packaging: ' + ($taskUnexpected.Name -join ', ')) }
} else { New-Item -ItemType Directory -Path $taskOutput | Out-Null }
Copy-Item -LiteralPath $taskDll -Destination (Join-Path $taskOutput 'GameWindowMapper.dll')
Copy-Item -LiteralPath $taskGuide -Destination (Join-Path $taskOutput '使用说明.md')
$taskFiles = @(Get-ChildItem -LiteralPath $taskOutput -File -Force)
if ($taskFiles.Count -ne 2) { throw 'Delivery must contain exactly two files.' }
if ((Get-FileHash -LiteralPath $taskDll).Hash -ne (Get-FileHash -LiteralPath (Join-Path $taskOutput 'GameWindowMapper.dll')).Hash) { throw 'DLL copy verification failed.' }
Write-Output "Ready: $taskOutput (GameWindowMapper.dll + 使用说明.md)"
