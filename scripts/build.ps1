param(
    [switch]$Test,
    [switch]$X64Source,
    [switch]$X64Helper,
    [string]$CMakePath
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
if (-not $CMakePath) {
    $taskCommand = Get-Command cmake -ErrorAction SilentlyContinue
    if ($taskCommand) { $CMakePath = $taskCommand.Source }
    else {
        $taskCandidates = @(
            (Join-Path $env:ProgramFiles 'CMake/bin/cmake.exe'),
            (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'),
            (Join-Path $env:LOCALAPPDATA 'CQ_SLink/tools/cmake-3.31.10/cmake/data/bin/cmake.exe')
        )
        $CMakePath = $taskCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    }
}
if (-not $CMakePath -or -not (Test-Path -LiteralPath $CMakePath)) {
    throw 'CMake >= 3.25 is required. Install it or pass -CMakePath. Also install MSVC 2022 C++ and Windows SDK 10.0.18362+.'
}
if ($X64Source -and $X64Helper) { throw 'Choose only one of -X64Source and -X64Helper.' }
if ($Test -and ($X64Source -or $X64Helper)) { throw '-Test applies to the x86 DLL build.' }
$taskPreset = if ($X64Source) { 'x64-source' } elseif ($X64Helper) { 'x64-helper' } else { 'x86' }
Push-Location $taskRoot
try {
    if (-not $X64Source) {
        & (Join-Path $PSScriptRoot 'fetch-driver.ps1') | Out-Host
        & $CMakePath --preset x64-helper
        if ($LASTEXITCODE -ne 0) { throw 'x64 helper configure failed' }
        & $CMakePath --build --preset x64-helper-release --target gwm_recovery driver_probe --parallel
        if ($LASTEXITCODE -ne 0) { throw 'x64 helper build failed' }
        if ($X64Helper) { return }
    }
    & $CMakePath --preset $taskPreset
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    if ($X64Source) { & $CMakePath --build --preset "$taskPreset-release" --target dynamic_source --parallel }
    elseif ($Test) { & $CMakePath --build --preset "$taskPreset-release" --parallel }
    else { & $CMakePath --build --preset "$taskPreset-release" --target GameWindowMapper --parallel }
    if ($LASTEXITCODE -ne 0) { throw 'CMake build failed' }
    if ($Test) {
        $taskCtest = Join-Path (Split-Path -Parent $CMakePath) 'ctest.exe'
        & $taskCtest --preset x86-release -R "^single_entry_lifecycle$"
        if ($LASTEXITCODE -ne 0) { throw 'CTest failed' }
    }
} finally { Pop-Location }
