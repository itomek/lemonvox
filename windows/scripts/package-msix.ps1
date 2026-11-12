#Requires -Version 5.1
<#
.SYNOPSIS
Creates an unsigned x64 desktop MSIX from an existing LemonVox distribution.
.DESCRIPTION
Identity values must be supplied explicitly. Use Partner Center's exact values
for a Store candidate. This script does not sign, install or publish packages.
#>
param(
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [Parameter(Mandatory = $true)][ValidatePattern('^[A-Za-z0-9][A-Za-z0-9.-]{2,49}$')][string]$PackageName,
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$Publisher,
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$PublisherDisplayName,
    [Parameter(Mandatory = $true)][string]$StoreVersion,
    [Parameter(Mandatory = $true)][string]$Output
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'distribution-common.ps1')
if ($StoreVersion -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.0$') {
    throw 'StoreVersion must be X.Y.Z.0 (fourth component reserved for the Store).'
}
$parts = $StoreVersion.Split('.') | ForEach-Object { [long]$_ }
if ($parts[0] -lt 1 -or @($parts | Where-Object { $_ -gt 65535 }).Count) {
    throw 'StoreVersion components must be <= 65535 and the first must be nonzero.'
}
foreach ($value in $Publisher, $PublisherDisplayName) {
    if ([string]::IsNullOrWhiteSpace($value) -or $value -match '[\x00-\x1F]') { throw 'Publisher values cannot be blank or contain control characters.' }
}
if ($Publisher -notmatch '^CN=') { throw 'Publisher must be the full certificate subject / Partner Center publisher (CN=...).' }
$source = (Resolve-Path -LiteralPath $PackageDirectory).Path
foreach ($name in 'LemonVox.exe', 'lemonvox.ico', 'LICENSE.txt', 'THIRD_PARTY_NOTICES.txt', 'VERSION.txt') {
    if (-not (Test-Path -LiteralPath (Join-Path $source $name) -PathType Leaf)) { throw "Package is missing $name" }
}
Assert-X64Executable (Join-Path $source 'LemonVox.exe')
$outputPath = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $outputPath) { throw "Output must be a new directory: $outputPath" }
$sdkRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -Name KitsRoot10).KitsRoot10
$sdk = Get-ChildItem (Join-Path $sdkRoot 'bin') -Directory |
    Where-Object { $_.Name -match '^10\.0\.\d+\.0$' } |
    Sort-Object { [version]$_.Name } -Descending |
    Where-Object { Test-Path (Join-Path $_.FullName 'x64\makeappx.exe') } | Select-Object -First 1
if (-not $sdk) { throw 'Windows SDK x64 MakeAppx is required.' }
$makeappx = Join-Path $sdk.FullName 'x64\makeappx.exe'
$stage = Join-Path $outputPath 'package'
$assets = Join-Path $stage 'Assets'
New-Item -ItemType Directory $outputPath | Out-Null
New-Item -ItemType Directory $assets | Out-Null
foreach ($name in 'LemonVox.exe', 'LICENSE.txt', 'THIRD_PARTY_NOTICES.txt', 'VERSION.txt') {
    Copy-Item -LiteralPath (Join-Path $source $name) -Destination $stage
}
# Reuse the existing mark; no new branding or image-generation dependency.
Add-Type -AssemblyName System.Drawing
$icoBytes = [IO.File]::ReadAllBytes((Join-Path $source 'lemonvox.ico'))
if ($icoBytes.Length -lt 6 -or [BitConverter]::ToUInt16($icoBytes, 2) -ne 1) { throw 'Invalid app icon.' }
$count = [BitConverter]::ToUInt16($icoBytes, 4)
if ($icoBytes.Length -lt 6 + 16 * $count) { throw 'Truncated app icon directory.' }
$pngBytes = $null
for ($index = 0; $index -lt $count; ++$index) {
    $entry = 6 + 16 * $index
    if ($icoBytes[$entry] -ne 0 -or $icoBytes[$entry + 1] -ne 0) { continue }
    $length = [BitConverter]::ToUInt32($icoBytes, $entry + 8)
    $offset = [BitConverter]::ToUInt32($icoBytes, $entry + 12)
    if ([long]$offset + $length -gt $icoBytes.Length) { throw 'Truncated app icon frame.' }
    $pngBytes = New-Object byte[] $length
    [Array]::Copy($icoBytes, $offset, $pngBytes, 0, $length)
    break
}
if (-not $pngBytes) { throw 'App icon must contain the existing 256px PNG frame.' }
$stream = [IO.MemoryStream]::new($pngBytes, $false)
$image = [Drawing.Image]::FromStream($stream)
try {
    foreach ($asset in @(@('Square44x44Logo.png', 44), @('Square150x150Logo.png', 150), @('StoreLogo.png', 50))) {
        $bitmap = New-Object Drawing.Bitmap([int]$asset[1], [int]$asset[1])
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.DrawImage($image, 0, 0, $bitmap.Width, $bitmap.Height)
            $bitmap.Save((Join-Path $assets $asset[0]), [Drawing.Imaging.ImageFormat]::Png)
        } finally { $graphics.Dispose(); $bitmap.Dispose() }
    }
} finally { $image.Dispose(); $stream.Dispose() }
$publisherXml = [Security.SecurityElement]::Escape($Publisher)
$displayXml = [Security.SecurityElement]::Escape($PublisherDisplayName)
$manifest = @"
<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
 xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
 xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"
 IgnorableNamespaces="uap rescap">
 <Identity Name="$PackageName" Publisher="$publisherXml" Version="$StoreVersion" ProcessorArchitecture="x64" />
 <Properties><DisplayName>LemonVox</DisplayName><PublisherDisplayName>$displayXml</PublisherDisplayName><Logo>Assets\StoreLogo.png</Logo></Properties>
 <Resources><Resource Language="en-US" /></Resources>
 <Dependencies><TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.22000.0" MaxVersionTested="10.0.26100.0" /></Dependencies>
 <Applications><Application Id="LemonVox" Executable="LemonVox.exe" EntryPoint="Windows.FullTrustApplication">
  <uap:VisualElements DisplayName="LemonVox" Description="Requires separate local Lemonade or a compatible OpenAI server"
   BackgroundColor="transparent" Square150x150Logo="Assets\Square150x150Logo.png" Square44x44Logo="Assets\Square44x44Logo.png" />
 </Application></Applications>
 <Capabilities><Capability Name="internetClient" /><rescap:Capability Name="runFullTrust" /><DeviceCapability Name="microphone" /></Capabilities>
</Package>
"@
[IO.File]::WriteAllText((Join-Path $stage 'AppxManifest.xml'), $manifest, (New-Object Text.UTF8Encoding($false)))
$package = Join-Path $outputPath "LemonVox-$StoreVersion-windows-x64.unsigned.msix"
# Do not use /nv: schema and semantic validation are part of this command.
& $makeappx pack /d $stage /p $package /h SHA256
if ($LASTEXITCODE -ne 0) { throw "MakeAppx validation/pack failed: $LASTEXITCODE" }
$hash = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $(Split-Path -Leaf $package)" | Set-Content -Encoding ASCII "$package.sha256"
@{
    PackageName = $PackageName; Publisher = $Publisher; StoreVersion = $StoreVersion
    ApplicationId = 'LemonVox'; Architecture = 'x64'; Signed = $false
    Validation = 'MakeAppx schema/semantic validation only; installed-package tests and Store certification remain required.'
} | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $outputPath 'MSIX-VALIDATION.json')
Write-Output "Prepared unsigned MSIX: $package"
