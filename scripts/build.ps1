param([switch]$Test)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskCmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
$taskCmake = if ($taskCmakeCommand) { $taskCmakeCommand.Source } else { Join-Path $env:LOCALAPPDATA 'CQ_SLink\tools\cmake-3.31.10\cmake\data\bin\cmake.exe' }
if (-not (Test-Path -LiteralPath $taskCmake)) { throw 'CMake >= 3.25 is required. Install CMake or add it to PATH.' }
Push-Location $taskRoot
try {
  & $taskCmake --preset x86
  if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
  & $taskCmake --build --preset x86-release --parallel
  if ($LASTEXITCODE -ne 0) { throw 'CMake build failed' }
  if ($Test) {
    $taskCtest = Join-Path (Split-Path -Parent $taskCmake) 'ctest.exe'
    & $taskCtest --preset x86-release
    if ($LASTEXITCODE -ne 0) { throw 'CTest failed' }
  }
} finally { Pop-Location }

