[CmdletBinding()]
param(
    [string]$Repository = 'yffdd/wavelink',
    [string]$KeyDirectory = '',
    [string]$UpdateDirectory = ''
)
$ErrorActionPreference = 'Stop'
if (-not $KeyDirectory) { $KeyDirectory = Join-Path ([Environment]::GetFolderPath('UserProfile')) '.wavelink-release' }
if (-not $UpdateDirectory) { $UpdateDirectory = $PSScriptRoot }
Add-Type -AssemblyName System.Security
if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw 'Use a GitHub repository name in owner/repository form.' }
$directory = [IO.Path]::GetFullPath($UpdateDirectory)
$configFile = Join-Path $directory 'publish.local.json'
$channelFile = Join-Path $directory 'channel.cmake'
$existing = $null
if (Test-Path -LiteralPath $configFile) { $existing = [IO.File]::ReadAllText($configFile) | ConvertFrom-Json }
$keyFile = Join-Path $KeyDirectory 'update-signing.key'
if ($existing -and $existing.signingKey) { $keyFile = [string]$existing.signingKey }
$keyFile = [IO.Path]::GetFullPath($keyFile)
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..')).TrimEnd('\') + '\'
if ($keyFile.StartsWith($sourceRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'The PRIVATE signing key must remain outside the source/release tree.' }
if (-not (Test-Path -LiteralPath $keyFile -PathType Leaf)) { throw "Signing key missing: $keyFile. Generate the key once; this tool will never replace it." }
# Derive/validate the public key without exposing private bytes in output or
# copying the encrypted private file into the project.
$rsa = New-Object Security.Cryptography.RSACryptoServiceProvider
$rsa.PersistKeyInCsp = $false
try {
    $secret = [Security.Cryptography.ProtectedData]::Unprotect([IO.File]::ReadAllBytes($keyFile), $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
    $rsa.FromXmlString([Text.Encoding]::UTF8.GetString($secret))
    if ($rsa.KeySize -lt 2048) { throw 'RSA signing key must be at least 2048 bits.' }
    $parameters = $rsa.ExportParameters($false)
    $memory = New-Object IO.MemoryStream; $writer = New-Object IO.BinaryWriter($memory)
    $writer.Write([uint32]0x31415352); $writer.Write([uint32]$rsa.KeySize)
    $writer.Write([uint32]$parameters.Exponent.Length); $writer.Write([uint32]$parameters.Modulus.Length)
    $writer.Write([uint32]0); $writer.Write([uint32]0); $writer.Write($parameters.Exponent); $writer.Write($parameters.Modulus); $writer.Flush()
    $public = [Convert]::ToBase64String($memory.ToArray()); $writer.Dispose(); $memory.Dispose()
} finally { $rsa.Dispose(); if ($secret) { [Array]::Clear($secret, 0, $secret.Length) } }
$publicFile = Join-Path ([IO.Path]::GetDirectoryName($keyFile)) 'update-public.txt'
if (Test-Path -LiteralPath $publicFile) {
    if ([IO.File]::ReadAllText($publicFile).Trim() -cne $public) { throw 'The existing public key file does not match the private key. No configuration was changed.' }
}
if (Test-Path -LiteralPath $channelFile) {
    $channel = [IO.File]::ReadAllText($channelFile)
    $keyMatch = [regex]::Match($channel, 'set\(WAVELINK_UPDATE_PUBLIC_KEY\s+"([A-Za-z0-9+/=]+)"\)')
    $urlMatch = [regex]::Match($channel, 'set\(WAVELINK_UPDATE_FEED_URL\s+"(https://[^"\r\n]+)"\)')
    if (-not $keyMatch.Success -or -not $urlMatch.Success -or $keyMatch.Groups[1].Value -cne $public) { throw 'Existing channel.cmake is invalid or uses a different key. It was preserved; correct it explicitly before publishing.' }
}
if ($existing) {
    if (-not $existing.installerBaseUrl) { throw 'Existing publish.local.json has no installerBaseUrl; it was not overwritten.' }
    if ($existing.notesSource -ne 'currentChangelog' -and -not $existing.notesFile) { throw 'Configure notesSource=currentChangelog or a custom notesFile.' }
}
function Write-NewUtf8File([string]$Path, [string]$Text) {
    $stream = New-Object IO.FileStream($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $bytes = (New-Object Text.UTF8Encoding $false).GetBytes($Text); $stream.Write($bytes, 0, $bytes.Length) }
    finally { $stream.Dispose() }
}
[IO.Directory]::CreateDirectory($directory) | Out-Null
if (-not (Test-Path -LiteralPath $channelFile)) {
    $feed = "https://github.com/$Repository/releases/latest/download/latest.json"
    Write-NewUtf8File $channelFile "# Public update channel; private signing key is never embedded.`r`nset(WAVELINK_UPDATE_FEED_URL `"$feed`")`r`nset(WAVELINK_UPDATE_PUBLIC_KEY `"$public`")`r`n"
    Write-Output "Created public channel: $channelFile"
}
if (-not $existing) {
    $settings = [ordered]@{
        installerBaseUrl="https://github.com/$Repository/releases/download/v{version}"
        signingKey=$keyFile.Replace('\', '/')
        notesSource='currentChangelog'
    } | ConvertTo-Json
    Write-NewUtf8File $configFile ($settings + "`r`n")
    Write-Output "Created publishing configuration: $configFile"
}
Write-Output 'Update publishing is ready. Existing settings and signing keys were preserved. No files were uploaded.'
