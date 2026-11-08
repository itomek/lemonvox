#Requires -Version 5.1
<#
.SYNOPSIS
Prepares WinGet portable manifests for an exact, versioned LemonVox EXE.
.DESCRIPTION
Does not download, install or submit anything. InstallerUrl must be the final
public HTTPS EXE URL, and Executable must be those exact bytes. Use -Preview
for unpublished local candidates; never submit a preview directory.
#>
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][ValidatePattern('^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$')][string]$Version,
    [Parameter(Mandatory = $true)][ValidatePattern('^[A-Za-z0-9][A-Za-z0-9.-]*\.[A-Za-z0-9][A-Za-z0-9.-]*$')][string]$PackageIdentifier,
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$Publisher,
    [Parameter(Mandatory = $true)][string]$InstallerUrl,
    [Parameter(Mandatory = $true)][string]$Output,
    [switch]$Preview
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'distribution-common.ps1')
if ($Version -eq '0.0.0' -and -not $Preview) { throw 'Version 0.0.0 requires -Preview and must not be submitted.' }
if ([string]::IsNullOrWhiteSpace($Publisher) -or $Publisher -match '[\x00-\x1F]') { throw 'Publisher cannot be blank or contain control characters.' }
$uri = $null
if (-not [Uri]::TryCreate($InstallerUrl, [UriKind]::Absolute, [ref]$uri) -or
    $uri.Scheme -ne 'https' -or $uri.UserInfo -or $uri.Query -or $uri.Fragment -or
    $uri.IsLoopback -or -not $uri.AbsolutePath.EndsWith('.exe', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'InstallerUrl must be a versioned public HTTPS EXE URL without credentials, query or fragment.'
}
if ($uri.AbsolutePath -notmatch ('(^|[/_-])v?' + [regex]::Escape($Version) + '($|[/_.-])')) {
    throw 'InstallerUrl path must identify the exact package version, not a moving latest URL.'
}
$exe = (Resolve-Path -LiteralPath $Executable).Path
if ([IO.Path]::GetExtension($exe) -ne '.exe' -or -not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'Executable must be an EXE file.' }
Assert-X64Executable $exe
$outputPath = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $outputPath) { throw "Output must be a new directory: $outputPath" }
$hash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
function YamlString([string]$value) { ConvertTo-Json -InputObject $value -Compress }
$idYaml = YamlString $PackageIdentifier
$versionYaml = YamlString $Version
$publisherYaml = YamlString $Publisher
$urlYaml = YamlString $InstallerUrl
$versionManifest = @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.version.1.12.0.schema.json
PackageIdentifier: $idYaml
PackageVersion: $versionYaml
DefaultLocale: en-US
ManifestType: version
ManifestVersion: 1.12.0
"@
$localeManifest = @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.defaultLocale.1.12.0.schema.json
PackageIdentifier: $idYaml
PackageVersion: $versionYaml
PackageLocale: en-US
Publisher: $publisherYaml
PublisherUrl: https://github.com/itomek
PackageName: LemonVox
PackageUrl: https://github.com/itomek/lemonvox
License: MIT
LicenseUrl: https://github.com/itomek/lemonvox/blob/main/LICENSE
ShortDescription: Desktop dictation backed by a separately installed Lemonade server.
Description: Requires Windows 11 x64 and a separately installed, running Lemonade server with transcription and cleanup models. Press a configurable shortcut to dictate into the focused app.
Moniker: lemonvox
ManifestType: defaultLocale
ManifestVersion: 1.12.0
"@
$installerManifest = @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.installer.1.12.0.schema.json
PackageIdentifier: $idYaml
PackageVersion: $versionYaml
InstallerType: portable
MinimumOSVersion: 10.0.22000.0
Commands:
- lemonvox
Installers:
- Architecture: x64
  InstallerUrl: $urlYaml
  InstallerSha256: $hash
ManifestType: installer
ManifestVersion: 1.12.0
"@
New-Item -ItemType Directory $outputPath | Out-Null
$encoding = New-Object Text.UTF8Encoding($false)
$previewLabel = if ($Preview) { "# UNPUBLISHED PREVIEW: URL/install unverified. Do not submit to winget-pkgs.`n" } else { '' }
[IO.File]::WriteAllText((Join-Path $outputPath "$PackageIdentifier.yaml"), $previewLabel + $versionManifest, $encoding)
[IO.File]::WriteAllText((Join-Path $outputPath "$PackageIdentifier.locale.en-US.yaml"), $previewLabel + $localeManifest, $encoding)
[IO.File]::WriteAllText((Join-Path $outputPath "$PackageIdentifier.installer.yaml"), $previewLabel + $installerManifest, $encoding)
Write-Output "Prepared WinGet manifests: $outputPath (EXE SHA256 $hash)"
