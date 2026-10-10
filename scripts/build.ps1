param(
    [switch]$Test,
    [switch]$VirtualDisplayTests,
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
if ($VirtualDisplayTests -and -not $Test) { throw '-VirtualDisplayTests requires -Test and an installed own driver.' }
$taskPreset = if ($X64Source) { 'x64-source' } elseif ($X64Helper) { 'x64-helper' } else { 'x86' }
Push-Location $taskRoot
try {
    if (-not $X64Source) {
        & (Join-Path $PSScriptRoot 'fetch-driver.ps1') | Out-Host
        & $CMakePath --preset x64-helper
        if ($LASTEXITCODE -ne 0) { throw 'x64 helper configure failed' }
        & $CMakePath --build --preset x64-helper-release --target gwm_recovery driver_probe --parallel
        if ($LASTEXITCODE -ne 0) { throw 'x64 helper build failed' }
        if ($Test) {
            $taskProbe = Join-Path $taskRoot 'out/build/x64-helper/Release/driver_probe.exe'
            # Windows PowerShell 5 otherwise decodes native UTF-8 JSON using the
            # console code page, which can even consume the closing quote.
            function Read-TaskProbe([string]$Argument) {
                $taskInfo = New-Object System.Diagnostics.ProcessStartInfo
                $taskInfo.FileName = $taskProbe
                $taskInfo.Arguments = $Argument
                $taskInfo.UseShellExecute = $false
                $taskInfo.CreateNoWindow = $true
                $taskInfo.RedirectStandardOutput = $true
                $taskInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
                $taskProcess = New-Object System.Diagnostics.Process
                $taskProcess.StartInfo = $taskInfo
                try {
                    [void]$taskProcess.Start()
                    $taskJson = $taskProcess.StandardOutput.ReadToEnd()
                    $taskProcess.WaitForExit()
                    return @{ Code = $taskProcess.ExitCode; Data = ($taskJson | ConvertFrom-Json) }
                } finally { $taskProcess.Dispose() }
            }
            $taskInstalled = Read-TaskProbe '--inspect'
            if ($taskInstalled.Code -ne 0 -or -not $taskInstalled.Data.installed) { throw 'Native canvas tests require the already installed project driver; tests never initiate first-time installation.' }
            $taskDesktop = Read-TaskProbe '--desktop'
            if ($taskDesktop.Code -ne 0 -or $taskDesktop.Data.inputDesktop -ne 'Default' -or $taskDesktop.Data.remoteSession) { throw 'Native window tests require an unlocked local Default desktop. No window/display checks were run.' }
        }
        if ($VirtualDisplayTests) {
            & $CMakePath --build --preset x64-helper-release --target recovery_prompt_tests request_cache_tests --parallel
            if ($LASTEXITCODE -ne 0) { throw 'recovery prompt build failed' }
            & (Join-Path (Split-Path -Parent $CMakePath) 'ctest.exe') --test-dir out/build/x64-helper -C Release -R '^(recovery_prompt|request_cache)$' --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw 'recovery prompt test failed' }
        }
        if ($X64Helper) { return }
    }
    if ($taskPreset -eq 'x86') {
        $taskDisplayTests = if ($VirtualDisplayTests) { 'ON' } else { 'OFF' }
        & $CMakePath --preset $taskPreset "-DGWM_ENABLE_DISPLAY_TESTS=$taskDisplayTests"
    } else { & $CMakePath --preset $taskPreset }
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    if ($X64Source) { & $CMakePath --build --preset "$taskPreset-release" --target dynamic_source --parallel }
    elseif ($VirtualDisplayTests) {
        & $CMakePath --build --preset "$taskPreset-release" --target native_tests gpu_geometry_tests virtual_capture_tests recovery_integration_tests canvas_virtual_tests --parallel
    }
    elseif ($Test) { & $CMakePath --build --preset "$taskPreset-release" --target native_tests --parallel }
    else { & $CMakePath --build --preset "$taskPreset-release" --target GameWindowMapper --parallel }
    if ($LASTEXITCODE -ne 0) { throw 'CMake build failed' }
    if ($Test) {
        $taskCtest = Join-Path (Split-Path -Parent $CMakePath) 'ctest.exe'
        $taskFilter = if ($VirtualDisplayTests) { '^(single_entry_lifecycle|gpu_geometry|virtual_capture|recovery_integration|canvas_virtual)$' } else { '^single_entry_lifecycle$' }
        & $taskCtest --preset x86-release -R $taskFilter --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw 'CTest failed' }
    }
} finally { Pop-Location }
