[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [string]$BuildDirectory = '',
    [string]$Destination = '',
    [switch]$PreflightOnly,
    [string]$ImportPrebuilt = '',
    [string]$SourceCommit = ''
)
$ErrorActionPreference = 'Stop'
$Configuration = if ($Configuration -ieq 'Release') { 'Release' } else { 'Debug' }
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$source = Join-Path $repoRoot 'WaveLink\src\platform\windows\blehelper'
$cache = Join-Path $PSScriptRoot ('.ble-cache\' + $Configuration)
$cachedHost = Join-Path $cache 'WaveLinkBleHost.exe'
$cachedManifest = Join-Path $cache 'source-manifest.json'
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repoRoot ('build\ble-helper-msvc-' + $Configuration.ToLowerInvariant()) }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)

function Get-Hash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $sha.Dispose() }
}
function Get-HelperInputs {
    $pending = New-Object Collections.Queue
    Get-ChildItem -LiteralPath $source -File -Recurse | ForEach-Object { $pending.Enqueue($_.FullName) }
    $seen = @{}
    while ($pending.Count) {
        $path = [IO.Path]::GetFullPath($pending.Dequeue())
        if ($seen.ContainsKey($path)) { continue }
        if (-not $path.StartsWith($repoRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'BLE helper input is outside the project.' }
        $file = Get-Item -LiteralPath $path
        $seen[$path] = $file
        $content = [IO.File]::ReadAllText($path)
        foreach ($include in [regex]::Matches($content, '(?m)^\s*#\s*include\s*"([^"]+)"')) {
            $pending.Enqueue([IO.Path]::GetFullPath((Join-Path $file.DirectoryName $include.Groups[1].Value)))
        }
    }
    $seen.Values | Sort-Object FullName
}
function Get-SourceHash {
    # Follow local headers too (e.g. blesendpolicy.h outside the native folder).
    # Normalize Git text across CRLF/LF checkouts; include every helper input.
    $inputs = foreach ($file in $helperInputs) {
        $relative = $file.FullName.Substring($repoRoot.Length + 1).Replace('\', '/')
        $content = [IO.File]::ReadAllText($file.FullName).Replace("`r`n", "`n")
        $relative + "`n" + $content.Length + "`n" + $content
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($inputs -join "`n")))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Confirm-Helper([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5a4d) { throw 'BLE helper is not a Windows executable.' }
    $pe = [BitConverter]::ToInt32($bytes, 0x3c)
    if ($pe -lt 0 -or $pe + 6 -gt $bytes.Length -or [BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550 -or [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664) { throw 'BLE helper must be a Windows x64 executable.' }
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $Path
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = 'Hidden'
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    $started = $false
    try {
        $started = $process.Start()
        if (-not $started) { throw 'Could not start BLE helper.' }
        $stream = $process.StandardOutput.BaseStream
        $header = New-Object byte[] 4
        $offset = 0
        while ($offset -lt 4) {
            $pending = $stream.ReadAsync($header, $offset, 4 - $offset)
            if (-not $pending.Wait(5000) -or $pending.Result -le 0) { throw 'BLE helper handshake timed out.' }
            $offset += $pending.Result
        }
        $length = [BitConverter]::ToUInt32($header, 0)
        if ($length -lt 1 -or $length -gt 4096) { throw 'Invalid BLE helper handshake length.' }
        $payload = New-Object byte[] $length
        $offset = 0
        while ($offset -lt $length) {
            $pending = $stream.ReadAsync($payload, $offset, $length - $offset)
            if (-not $pending.Wait(5000) -or $pending.Result -le 0) { throw 'Incomplete BLE helper handshake.' }
            $offset += $pending.Result
        }
        $hello = [Text.Encoding]::UTF8.GetString($payload) | ConvertFrom-Json
        if ($hello.event -ne 'hello' -or $hello.role -ne 'ble' -or $hello.version -ne 1 -or $hello.fileReaderVersion -ne 1 -or $hello.sendControlVersion -ne 1) { throw 'BLE helper uses an incompatible IPC version.' }
        # Inspect only startup. No radio, connection or GATT command is sent.
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(2000)) { $process.Kill(); $process.WaitForExit() }
    } finally {
        if ($started -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
    }
}
function Save-Cache([string]$HostPath, [string]$Origin) {
    Confirm-Helper $HostPath
    [void][IO.Directory]::CreateDirectory($cache)
    $hash = Get-Hash $HostPath
    if ([IO.Path]::GetFullPath($HostPath) -ine [IO.Path]::GetFullPath($cachedHost)) { Copy-Item -LiteralPath $HostPath -Destination $cachedHost -Force }
    $manifest = [ordered]@{ schema=1; configuration=$Configuration; sourceHash=$sourceHash; sha256=$hash; origin=$Origin }
    [IO.File]::WriteAllText($cachedManifest, ($manifest | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))
}
function Confirm-Cache {
    if (-not (Test-Path -LiteralPath $cachedHost -PathType Leaf) -or -not (Test-Path -LiteralPath $cachedManifest -PathType Leaf)) { throw 'No source-matched BLE helper cache is available.' }
    $manifest = [IO.File]::ReadAllText($cachedManifest) | ConvertFrom-Json
    if ($manifest.schema -ne 1 -or $manifest.configuration -cne $Configuration -or $manifest.sourceHash -cne $sourceHash) { throw 'BLE helper source changed. Rebuild it with Visual Studio 2022 C++ tools and Windows 10/11 SDK.' }
    if ($manifest.sha256 -cne (Get-Hash $cachedHost)) { throw 'BLE helper cache checksum mismatch. Rebuild or import a trusted matching helper.' }
}
try {
    $helperInputs = @(Get-HelperInputs)
    $sourceHash = Get-SourceHash
    if ($ImportPrebuilt) {
        if ($SourceCommit -notmatch '^[a-zA-Z0-9._/-]+$' -or $SourceCommit.StartsWith('-')) { throw 'Import requires a trusted SourceCommit tag or commit.' }
        # Import is explicit; require unchanged sources relative to the stated ref.
        $sourcePaths = @('WaveLink/src/platform/windows/blehelper') + @($helperInputs | ForEach-Object { $_.FullName.Substring($repoRoot.Length + 1).Replace('\', '/') })
        & git -C $repoRoot diff --quiet $SourceCommit -- @sourcePaths
        if ($LASTEXITCODE -ne 0) { throw 'Current BLE helper sources do not match SourceCommit.' }
        $untracked = @(& git -C $repoRoot ls-files --others --exclude-standard -- @sourcePaths)
        if ($LASTEXITCODE -ne 0 -or $untracked.Count) { throw 'Untracked BLE helper inputs prevent importing a prebuilt helper.' }
        Save-Cache ([IO.Path]::GetFullPath($ImportPrebuilt)) ('Imported from ' + $SourceCommit)
        Write-Output ('[BLE] Imported and verified matching ' + $Configuration + ' helper: ' + $cachedHost)
        exit 0
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $visualStudio = ''
    if (Test-Path -LiteralPath $vswhere) {
        $visualStudio = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    }
    if (-not $visualStudio) {
        try { Confirm-Cache } catch { throw ('Visual Studio 2022 C++ tools were not found. ' + $_.Exception.Message + ' Install Desktop development with C++ and Windows 10/11 SDK, or import a trusted matching helper using deploy/Build-BleHost.ps1 -Configuration ' + $Configuration + ' -ImportPrebuilt <exe> -SourceCommit <tag>.') }
        Write-Output '[BLE] MSVC is unavailable; the verified helper cache matches the current BLE sources.'
        if ($PreflightOnly) { exit 0 }
        Confirm-Helper $cachedHost
        $output = Join-Path $BuildDirectory $Configuration
        [void][IO.Directory]::CreateDirectory($output)
        Copy-Item -LiteralPath $cachedHost -Destination (Join-Path $output 'WaveLinkBleHost.exe') -Force
    } else {
        Write-Output ('[BLE] Visual Studio 2022 C++ tools: ' + $visualStudio)
        if ($PreflightOnly) { exit 0 }
        $cmake = if ($env:CMAKE) { $env:CMAKE.Trim('"') } else { 'cmake.exe' }
        & $cmake -S $source -B $BuildDirectory -G 'Visual Studio 17 2022' -A x64
        if ($LASTEXITCODE -ne 0) { throw 'BLE host configuration failed. Check the Visual Studio C++ tools and Windows SDK installation.' }
        & $cmake --build $BuildDirectory --config $Configuration --parallel 2
        if ($LASTEXITCODE -ne 0) { throw 'BLE host build failed.' }
        Save-Cache (Join-Path $BuildDirectory ($Configuration + '\WaveLinkBleHost.exe')) 'Built with Visual Studio 2022'
    }
    $hostPath = Join-Path $BuildDirectory ($Configuration + '\WaveLinkBleHost.exe')
    if ($Destination) {
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetFullPath($Destination))
        Copy-Item -LiteralPath $hostPath -Destination (Join-Path $Destination 'WaveLinkBleHost.exe') -Force
    }
    Write-Output ('[BLE] Windows BLE host is ready: ' + $hostPath)
} catch {
    Write-Output ('[ERROR] ' + $_.Exception.Message)
    exit 1
}
