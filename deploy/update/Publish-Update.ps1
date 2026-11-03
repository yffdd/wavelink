[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Config,
    [Parameter(Mandatory=$true)][string]$Installer,
    [Parameter(Mandatory=$true)][string]$Version,
    [string]$ChangelogFile = ''
)
$ErrorActionPreference = 'Stop'
if (-not $ChangelogFile) { $ChangelogFile = Join-Path $PSScriptRoot '..\..\WaveLink\src\ui\helpdialog.cpp' }
if ($Version -notmatch '^\d{1,6}(\.\d{1,6}){1,3}$') { throw 'Invalid release version.' }
# Local config contains paths, not private key material. It is never embedded.
$settings = Get-Content -LiteralPath $Config -Raw -Encoding UTF8 | ConvertFrom-Json
$file = Get-Item -LiteralPath $Installer
$channelFile = Join-Path ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Config))) 'channel.cmake'
if (-not (Test-Path -LiteralPath $channelFile)) { throw 'Configure channel.cmake before publishing online updates.' }
$channel = [IO.File]::ReadAllText($channelFile)
$publicMatch = [regex]::Match($channel, 'set\(WAVELINK_UPDATE_PUBLIC_KEY\s+"([A-Za-z0-9+/=]+)"\)')
if (-not $publicMatch.Success) { throw 'No valid public key found in channel.cmake.' }
$baseUrl = ([string]$settings.installerBaseUrl).Replace('{version}', $Version).TrimEnd('/')
if ($baseUrl -match '[{}]') { throw 'Unknown placeholder in installerBaseUrl. Only {version} is supported.' }
$url = $baseUrl + '/' + [Uri]::EscapeDataString($file.Name)
$notesFile = [string]$settings.notesFile
if ($settings.notesSource -eq 'currentChangelog') {
    $source = [IO.File]::ReadAllText([IO.Path]::GetFullPath($ChangelogFile), [Text.Encoding]::UTF8)
    $release = [regex]::Match($source, '(?s)txtCurrentRelease->setHtml\s*\(\s*QString::fromUtf8\s*\(\s*R"\((.*?)\)"\s*\)\s*\)\s*;')
    if (-not $release.Success) { throw 'Current release notes were not found. No signed manifest was generated; update the extractor or configure a custom notesFile.' }
    $items = @([regex]::Matches($release.Groups[1].Value, '(?s)<li\b[^>]*>(.*?)</li>') | ForEach-Object {
        $plain = [regex]::Replace($_.Groups[1].Value, '<[^>]+>', ' ')
        $plain = [Net.WebUtility]::HtmlDecode($plain)
        '- ' + [regex]::Replace($plain, '\s+', ' ').Trim()
    })
    if (-not $items.Count) { throw 'Current changelog contains no release items. Refusing to publish empty notes.' }
    $notesFile = Join-Path $file.DirectoryName ("WaveLink_v$($Version)_ReleaseNotes.txt")
    [IO.File]::WriteAllText($notesFile, (("WaveLink v$Version" + "`r`n`r`n" + ($items -join "`r`n") + "`r`n")), (New-Object Text.UTF8Encoding $false))
    Write-Output "Release notes extracted from the current in-app changelog: $notesFile"
}
if (-not $notesFile) { throw 'Set notesSource=currentChangelog or provide notesFile.' }
& (Join-Path $PSScriptRoot 'New-UpdateManifest.ps1') -Installer $file.FullName -Version $Version -DownloadUrl $url -SigningKey $settings.signingKey -NotesFile $notesFile -OutputPath (Join-Path $file.DirectoryName 'latest.json') -ExpectedPublicKey $publicMatch.Groups[1].Value -Force
Write-Output "Installer URL: $url"
Write-Output 'Upload the installer FIRST, verify its public HTTPS download, then publish latest.json LAST. This tool does not upload or publish external files.'
