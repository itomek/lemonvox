#Requires -Version 5.1
<#
.SYNOPSIS
Builds a Release LemonVox for Windows and packages a per-user installable zip.

.DESCRIPTION
Produces LemonVox-<version>-windows-x64.zip plus a .sha256 file in -Output.
VERSION.txt in the zip records the version, source commit and toolchain so the
artifact can be traced to its source. No signing is performed.

.EXAMPLE
powershell -ExecutionPolicy Bypass -File windows\scripts\package.ps1 -Output C:\temp\lv-pkg -Version 0.1.0
#>
param(
    [Parameter(Mandatory = $true)][string]$Output,
    [Parameter(Mandatory = $true)][ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
    [string]$ExcludeLabels = 'interactive|lemonade|audio-device'
)
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$outputPath = [IO.Path]::GetFullPath($Output)
if (Test-Path $outputPath) { throw "Output must be a new directory; $outputPath exists." }
$build = Join-Path $outputPath 'build'
& (Join-Path $PSScriptRoot 'build.ps1') -Output $build -Configuration Release -ExcludeLabels $ExcludeLabels

$exe = Join-Path $build 'src\app\LemonVox.exe'
if (-not (Test-Path $exe)) { throw "LemonVox.exe was not produced at $exe" }

$commit = (git -C $root rev-parse HEAD).Trim()
$dirty = if (git -C $root status --porcelain) { ' (uncommitted changes)' } else { '' }
$compiler = (Get-Content (Join-Path $build 'CMakeCache.txt') | Select-String '^CMAKE_CXX_COMPILER:' | Select-Object -First 1).Line

$stage = Join-Path $outputPath 'LemonVox'
New-Item -ItemType Directory $stage | Out-Null
Copy-Item $exe $stage
Copy-Item (Join-Path $build 'src\ui\lemonvox.ico') $stage
Copy-Item (Join-Path $root 'windows\packaging\install.ps1') $stage
Copy-Item (Join-Path $root 'windows\packaging\uninstall.ps1') $stage
Copy-Item (Join-Path $root 'windows\packaging\README.txt') $stage
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.md') (Join-Path $stage 'THIRD_PARTY_NOTICES.txt')
@(
    $Version
    "source: $commit$dirty"
    "architecture: x64"
    "toolchain: $compiler"
    "built: $([DateTime]::UtcNow.ToString('u'))"
) | Set-Content -Encoding ASCII (Join-Path $stage 'VERSION.txt')

$zip = Join-Path $outputPath "LemonVox-$Version-windows-x64.zip"
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
"$hash  $(Split-Path -Leaf $zip)" | Set-Content -Encoding ASCII "$zip.sha256"

# WinGet's portable path downloads this exact EXE, not the script-install ZIP.
$standalone = Join-Path $outputPath "LemonVox-$Version-windows-x64.exe"
Copy-Item -LiteralPath $exe -Destination $standalone
Copy-Item -LiteralPath (Join-Path $stage 'VERSION.txt') -Destination (Join-Path $outputPath 'VERSION-windows-x64.txt')
$exeHash = (Get-FileHash -Algorithm SHA256 $standalone).Hash.ToLowerInvariant()
@("$hash  $(Split-Path -Leaf $zip)", "$exeHash  $(Split-Path -Leaf $standalone)") |
    Set-Content -Encoding ASCII (Join-Path $outputPath 'SHA256SUMS-windows-x64.txt')

$exeSize = (Get-Item $exe).Length
$zipSize = (Get-Item $zip).Length
Write-Host "Packaged $zip"
Write-Host ("LemonVox.exe {0:N0} bytes; zip {1:N0} bytes; sha256 {2}" -f $exeSize, $zipSize, $hash)
