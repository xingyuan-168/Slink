param([switch]$GeometryOnly)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskExe = Join-Path $taskRoot 'out\build\x86\Release\dwm_poc.exe'
$taskReports = Join-Path $taskRoot 'out\qualification'
New-Item -ItemType Directory -Path $taskReports -Force | Out-Null
$taskCases = if ($GeometryOnly) {
  foreach ($taskBackend in @('gdi','d3d')) { foreach ($taskState in @('normal','maximized')) {
    @{Name="geometry-$taskBackend-$taskState"; Arguments="--backend $taskBackend --geometry-only --cycles 100 --seconds 2 $(if ($taskState -eq 'maximized') {'--maximized'})"}
  }}
} else {
  foreach ($taskMapping in @('dwm','wgc')) { foreach ($taskEdge in @('right','left','top','bottom')) {
    @{Name="$taskMapping-d3d-$taskEdge"; Arguments="--backend d3d --cycles 1 --seconds 2 --edge $taskEdge $(if ($taskMapping -eq 'wgc') {'--wgc'})"}
  }}
}
$taskResults = @()
foreach ($taskCase in $taskCases) {
  $taskStart = [Diagnostics.ProcessStartInfo]::new()
  $taskStart.FileName = $taskExe
  $taskStart.Arguments = $taskCase.Arguments
  $taskStart.UseShellExecute = $false
  $taskStart.CreateNoWindow = $true
  $taskStart.RedirectStandardOutput = $true
  $taskStart.RedirectStandardError = $true
  $taskProcess = [Diagnostics.Process]::Start($taskStart)
  $taskOutput = $taskProcess.StandardOutput.ReadToEndAsync()
  $taskErrors = $taskProcess.StandardError.ReadToEndAsync()
  $taskProcess.WaitForExit()
  $taskCode = $taskProcess.ExitCode
  $taskLog = $taskOutput.Result + $taskErrors.Result
  [IO.File]::WriteAllText((Join-Path $taskReports ($taskCase.Name+'.txt')), $taskLog, [Text.UTF8Encoding]::new($false))
  Write-Output "$($taskCase.Name): exit=$taskCode"
  Write-Output $taskLog
  $taskResults += @{case=$taskCase.Name;arguments=$taskCase.Arguments;exitCode=$taskCode}
  $taskProcess.Dispose()
}
$taskName = if ($GeometryOnly) {'geometry-results.json'} else {'mapping-results.json'}
$taskResults | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskReports $taskName) -Encoding utf8
if ($taskResults | Where-Object exitCode -ne 0) { exit 1 }
