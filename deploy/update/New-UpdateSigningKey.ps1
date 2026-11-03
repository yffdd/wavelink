[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$KeyPath,
    [Parameter(Mandatory=$true)][string]$PublicKeyPath
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
$privateTarget = [IO.Path]::GetFullPath($KeyPath)
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..')).TrimEnd('\') + '\'
if ($privateTarget.StartsWith($repoRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'The PRIVATE key must be stored outside the source/release tree.' }
if ((Test-Path -LiteralPath $privateTarget) -or (Test-Path -LiteralPath $PublicKeyPath)) { throw 'Refusing to overwrite an existing key or public key file.' }
$rsa = New-Object Security.Cryptography.RSACryptoServiceProvider 3072
$rsa.PersistKeyInCsp = $false
try {
    $parameters = $rsa.ExportParameters($false)
    $memory = New-Object IO.MemoryStream
    $writer = New-Object IO.BinaryWriter($memory)
    # BCRYPT_RSAKEY_BLOB header (little endian), exponent/modulus (big endian).
    $writer.Write([uint32]0x31415352); $writer.Write([uint32]$rsa.KeySize)
    $writer.Write([uint32]$parameters.Exponent.Length); $writer.Write([uint32]$parameters.Modulus.Length)
    $writer.Write([uint32]0); $writer.Write([uint32]0)
    $writer.Write($parameters.Exponent); $writer.Write($parameters.Modulus); $writer.Flush()
    $public = [Convert]::ToBase64String($memory.ToArray())
    $private = [Text.Encoding]::UTF8.GetBytes($rsa.ToXmlString($true))
    $protected = [Security.Cryptography.ProtectedData]::Protect($private, $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($privateTarget)) | Out-Null
    [IO.File]::WriteAllBytes($privateTarget, $protected)
    # Restrict the key file to this Windows user. DPAPI also encrypts its contents.
    $acl = New-Object Security.AccessControl.FileSecurity
    $acl.SetAccessRuleProtection($true, $false)
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($identity, 'FullControl', 'Allow')))
    [IO.File]::SetAccessControl($privateTarget, $acl)
    [IO.File]::WriteAllText([IO.Path]::GetFullPath($PublicKeyPath), $public, (New-Object Text.UTF8Encoding $false))
    Write-Output "Created DPAPI-encrypted PRIVATE key: $privateTarget"
    Write-Output "Created public BCRYPT blob: $PublicKeyPath"
    Write-Output 'Keep the private key and its Windows user profile safely backed up. Never distribute it or place it in the source archive.'
} finally { $rsa.Dispose(); if ($private) { [Array]::Clear($private, 0, $private.Length) } }
