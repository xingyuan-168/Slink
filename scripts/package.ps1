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
if (Test-Path -LiteralPath $taskOutput) {
    $taskDirectory = Get-Item -LiteralPath $taskOutput -Force
    if (-not $taskDirectory.PSIsContainer -or ($taskDirectory.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'The delivery directory must be an ordinary project directory.'
    }
} else { New-Item -ItemType Directory -Path $taskOutput | Out-Null }
# Produce only our two delivery files. Existing user examples and images are
# user assets, not packaging garbage, and are never moved or deleted here.
foreach ($taskName in @('GameWindowMapper.dll','使用说明.md')) {
    $taskTarget = Join-Path $taskOutput $taskName
    if (Test-Path -LiteralPath $taskTarget) {
        $taskExisting = Get-Item -LiteralPath $taskTarget -Force
        if ($taskExisting.PSIsContainer -or ($taskExisting.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing to overwrite a directory or link: $taskTarget"
        }
    }
}
Copy-Item -LiteralPath $taskDll -Destination (Join-Path $taskOutput 'GameWindowMapper.dll')
Copy-Item -LiteralPath $taskGuide -Destination (Join-Path $taskOutput '使用说明.md')
if ((Get-FileHash -LiteralPath $taskDll).Hash -ne (Get-FileHash -LiteralPath (Join-Path $taskOutput 'GameWindowMapper.dll')).Hash) { throw 'DLL copy verification failed.' }
if ((Get-FileHash -LiteralPath $taskGuide).Hash -ne (Get-FileHash -LiteralPath (Join-Path $taskOutput '使用说明.md')).Hash) { throw 'Guide copy verification failed.' }
Write-Output "Ready: $taskOutput (GameWindowMapper.dll + 使用说明.md)"
