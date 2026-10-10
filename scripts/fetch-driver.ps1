param([string]$ArchivePath)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskCache = Join-Path $taskRoot 'out/vendor/virtual-display-driver/25.7.23'
$taskUrl = 'https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/25.7.23/VirtualDisplayDriver-x86.Driver.Only.zip'
$taskZipHash = 'e24210692b442b39af763536330ce78b423f19342b7a7792c26de3944e418b3a'
$taskFiles = [ordered]@{
    'MttVDD.inf' = '550d211fe481e74dfe3f9d724ed78be48b3a9113405965d683d9373e8d672f5d'
    'MttVDD.dll' = 'c9ca837f57a98fbd43bc416a7f535a95843626e7759eaf85cf0cd7ce334dbb05'
    'mttvdd.cat' = '08a0093fc9b2e32b287a6f8a77ca4de0a31830d29fc33d2b13a918dc859468f6'
}

function Assert-TaskCachePath([string]$Path) {
    $taskFull = [IO.Path]::GetFullPath($Path)
    $taskAllowed = [IO.Path]::GetFullPath((Join-Path $taskRoot 'out'))
    if (-not $taskFull.StartsWith($taskAllowed + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Driver cache must remain under this checkout's out directory: $taskFull"
    }
    $taskItem = $taskFull
    while ($taskItem -and $taskItem.Length -ge $taskRoot.Length) {
        if (Test-Path -LiteralPath $taskItem) {
            if ((Get-Item -LiteralPath $taskItem -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Refusing reparse point in driver cache path: $taskItem"
            }
        }
        $taskItem = Split-Path -Parent $taskItem
    }
}

Assert-TaskCachePath $taskCache
New-Item -ItemType Directory -Path $taskCache -Force | Out-Null
$taskComplete = $true
foreach ($taskName in $taskFiles.Keys) {
    $taskPath = Join-Path $taskCache $taskName
    Assert-TaskCachePath $taskPath
    if (-not (Test-Path -LiteralPath $taskPath)) { $taskComplete = $false; continue }
    if ((Get-FileHash -LiteralPath $taskPath -Algorithm SHA256).Hash -ne $taskFiles[$taskName]) {
        throw "Existing cached $taskName has the wrong SHA256; it was not overwritten. Inspect the cache before retrying."
    }
}
if ($taskComplete) { Write-Output $taskCache; return }

$taskOwnedDownload = $false
if (-not $ArchivePath) {
    $ArchivePath = Join-Path $taskCache 'driver.zip'
    Assert-TaskCachePath $ArchivePath
    if (-not (Test-Path -LiteralPath $ArchivePath)) {
        $taskDownload = Join-Path $taskCache ('driver-' + [Guid]::NewGuid().ToString('N') + '.download')
        Assert-TaskCachePath $taskDownload
        try {
            $taskOwnedDownload = $true
            Invoke-WebRequest -Uri $taskUrl -OutFile $taskDownload -UseBasicParsing
            if ((Get-FileHash -LiteralPath $taskDownload -Algorithm SHA256).Hash -ne $taskZipHash) {
                throw 'The downloaded VirtualDrivers release archive failed its pinned SHA256.'
            }
            Move-Item -LiteralPath $taskDownload -Destination $ArchivePath
            $taskOwnedDownload = $false
        } finally {
            if ($taskOwnedDownload -and (Test-Path -LiteralPath $taskDownload)) {
                Assert-TaskCachePath $taskDownload
                Remove-Item -LiteralPath $taskDownload
            }
        }
    }
}
if ((Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash -ne $taskZipHash) {
    throw 'The supplied VirtualDrivers release archive failed its pinned SHA256.'
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$taskZip = [IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($ArchivePath))
try {
    foreach ($taskName in $taskFiles.Keys) {
        $taskPath = Join-Path $taskCache $taskName
        if (Test-Path -LiteralPath $taskPath) { continue }
        # Read exact known entries only; never expand arbitrary ZIP paths.
        $taskEntry = $taskZip.GetEntry('VirtualDisplayDriver/' + $taskName)
        if (-not $taskEntry) { throw "Pinned archive has no expected entry: $taskName" }
        $taskInput = $taskEntry.Open()
        $taskMemory = [IO.MemoryStream]::new()
        try { $taskInput.CopyTo($taskMemory); $taskBytes = $taskMemory.ToArray() }
        finally { $taskMemory.Dispose(); $taskInput.Dispose() }
        $taskSha = [Security.Cryptography.SHA256]::Create()
        try { $taskDigest = [BitConverter]::ToString($taskSha.ComputeHash($taskBytes)).Replace('-', '').ToLowerInvariant() }
        finally { $taskSha.Dispose() }
        if ($taskDigest -ne $taskFiles[$taskName]) { throw "Pinned driver entry hash mismatch: $taskName" }
        Assert-TaskCachePath $taskPath
        $taskOutput = [IO.File]::Open($taskPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $taskOutput.Write($taskBytes, 0, $taskBytes.Length); $taskOutput.Flush($true) }
        finally { $taskOutput.Dispose() }
    }
} finally { $taskZip.Dispose() }
foreach ($taskSignedFile in @('MttVDD.dll', 'mttvdd.cat')) {
    $taskSignature = Get-AuthenticodeSignature -LiteralPath (Join-Path $taskCache $taskSignedFile)
    if ($taskSignature.Status -ne 'Valid') { throw "Authenticode validation failed for $taskSignedFile : $($taskSignature.Status)" }
}
Write-Output $taskCache
