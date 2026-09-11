param([Parameter(Mandatory)][int]$TargetProcessId,[Parameter(Mandatory)][string]$LogPath)
$ErrorActionPreference = 'Stop'
$taskProcess = Get-Process -Id $TargetProcessId
$taskStarted = $taskProcess.StartTime
$taskPattern = "pid_${TargetProcessId}_*"
while ($true) {
    $taskCurrent = Get-Process -Id $TargetProcessId -ErrorAction SilentlyContinue
    if (-not $taskCurrent -or $taskCurrent.StartTime -ne $taskStarted) { break }
    try {
        $taskEngines = @(Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUEngine -OperationTimeoutSec 10 |
            Where-Object Name -Like $taskPattern | Select-Object Name,UtilizationPercentage)
        $taskMemory = @(Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory -OperationTimeoutSec 10 |
            Where-Object Name -Like $taskPattern | Select-Object Name,DedicatedUsage,SharedUsage)
        @{utc=(Get-Date).ToUniversalTime().ToString('o');pid=$TargetProcessId;engines=$taskEngines;memory=$taskMemory} |
            ConvertTo-Json -Depth 5 -Compress | Add-Content -LiteralPath $LogPath -Encoding utf8
    } catch {
        @{utc=(Get-Date).ToUniversalTime().ToString('o');error=$_.Exception.Message} |
            ConvertTo-Json -Compress | Add-Content -LiteralPath $LogPath -Encoding utf8
    }
    Start-Sleep -Seconds 5
}
