[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Installer,
    [Parameter(Mandatory=$true)][string]$Version,
    [Parameter(Mandatory=$true)][string]$DownloadUrl,
    [Parameter(Mandatory=$true)][string]$SigningKey,
    [Parameter(Mandatory=$true)][string]$NotesFile,
    [Parameter(Mandatory=$true)][string]$OutputPath,
    [string]$ExpectedPublicKey = '',
    [ValidateRange(1,90)][int]$ValidDays = 30,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
if ($Version -notmatch '^\d{1,6}(\.\d{1,6}){1,3}$') { throw 'Invalid version. Use e.g. 0.13 or 0.13.1.' }
$url = [Uri]$DownloadUrl
if (-not $url.IsAbsoluteUri -or $url.Scheme -ne 'https' -or $url.UserInfo -or $url.Fragment) { throw 'Download URL must be HTTPS, without credentials or fragments.' }
$file = Get-Item -LiteralPath $Installer
if ($file.PSIsContainer -or $file.Extension -ine '.exe' -or $file.Length -le 0 -or $file.Length -gt 1GB) { throw 'Installer must be a nonempty EXE, no larger than 1 GiB.' }
$notes = [IO.File]::ReadAllText([IO.Path]::GetFullPath($NotesFile), [Text.Encoding]::UTF8)
if ($notes.Length -gt 20000) { throw 'Release notes exceed 20,000 characters.' }
$target = [IO.Path]::GetFullPath($OutputPath)
if ((Test-Path -LiteralPath $target) -and -not $Force) { throw 'Manifest exists; use -Force to replace it explicitly.' }
$keyTarget = [IO.Path]::GetFullPath($SigningKey)
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..')).TrimEnd('\') + '\'
if ($keyTarget.StartsWith($repoRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Private signing key must be outside the source/release tree.' }
$rsa = New-Object Security.Cryptography.RSACryptoServiceProvider
$rsa.PersistKeyInCsp = $false
try {
    $secret = [Security.Cryptography.ProtectedData]::Unprotect([IO.File]::ReadAllBytes($keyTarget), $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
    $rsa.FromXmlString([Text.Encoding]::UTF8.GetString($secret))
    if ($rsa.KeySize -lt 2048) { throw 'RSA key must be at least 2048 bits.' }
    if ($ExpectedPublicKey) {
        $parameters = $rsa.ExportParameters($false)
        $memory = New-Object IO.MemoryStream; $writer = New-Object IO.BinaryWriter($memory)
        $writer.Write([uint32]0x31415352); $writer.Write([uint32]$rsa.KeySize)
        $writer.Write([uint32]$parameters.Exponent.Length); $writer.Write([uint32]$parameters.Modulus.Length)
        $writer.Write([uint32]0); $writer.Write([uint32]0); $writer.Write($parameters.Exponent); $writer.Write($parameters.Modulus); $writer.Flush()
        $actualPublic = [Convert]::ToBase64String($memory.ToArray()); $writer.Dispose(); $memory.Dispose()
        if ($actualPublic -cne $ExpectedPublicKey.Trim()) { throw 'Private signing key does not match the public key embedded in channel.cmake. Refusing to publish an unusable update.' }
    }
    $now = [DateTime]::UtcNow
    $stream = [IO.File]::OpenRead($file.FullName)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { $digest = [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $hasher.Dispose() }
    $payload = [ordered]@{
        schema=1; product='WaveLink'; platform='windows-x64'; channel='stable'; version=$Version
        published=$now.ToString('yyyy-MM-ddTHH:mm:ssZ'); expires=$now.AddDays($ValidDays).ToString('yyyy-MM-ddTHH:mm:ssZ')
        notes=$notes; url=$url.AbsoluteUri; size=$file.Length
        sha256=$digest
    } | ConvertTo-Json -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($payload)
    $signature = $rsa.SignData($bytes, [Security.Cryptography.CryptoConfig]::MapNameToOID('SHA256'))
    $envelope = [ordered]@{payload=[Convert]::ToBase64String($bytes); signature=[Convert]::ToBase64String($signature)} | ConvertTo-Json
    # Verify our own output before publishing anything.
    if (-not $rsa.VerifyData($bytes, [Security.Cryptography.CryptoConfig]::MapNameToOID('SHA256'), $signature)) { throw 'Self-verification failed.' }
    $temporary = $target + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    [IO.File]::WriteAllText($temporary, $envelope, (New-Object Text.UTF8Encoding $false))
    Move-Item -LiteralPath $temporary -Destination $target -Force
    Write-Output "Signed update manifest generated: $target"
    Write-Output "Version $Version | $($file.Length) bytes | expires $($now.AddDays($ValidDays).ToString('u'))"
} finally { $rsa.Dispose(); if ($secret) { [Array]::Clear($secret, 0, $secret.Length) } }
