#Requires -Version 5.1
<#
.SYNOPSIS
Configures, builds and tests the LemonVox Windows host with MSVC and Ninja.

.EXAMPLE
powershell -ExecutionPolicy Bypass -File windows\scripts\build.ps1 -Output C:\temp\lv-build
#>
param(
    [Parameter(Mandatory = $true)][string]$Output,
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [switch]$SkipTests,
    # Interactive tests inject input or open windows; they need an unlocked desktop.
    [switch]$Interactive,
    # ctest label regex excluded from the run; hosted CI also excludes local-resource tests.
    [string]$ExcludeLabels = 'interactive'
)
$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found; install Visual Studio 2022 Build Tools with the C++ workload.' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio installation with MSVC x64 tools was found.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$source = Join-Path $root 'windows'
$outputPath = [System.IO.Path]::GetFullPath($Output)
$testArgs = if ($Interactive) { '' } elseif ($ExcludeLabels) { "-LE `"$ExcludeLabels`"" } else { '' }
$steps = @(
    "call `"$vcvars`" >nul",
    "cmake -S `"$source`" -B `"$outputPath`" -G Ninja -DCMAKE_BUILD_TYPE=$Configuration",
    "cmake --build `"$outputPath`""
)
if (-not $SkipTests) { $steps += "ctest --test-dir `"$outputPath`" --output-on-failure $testArgs" }
& $env:ComSpec /d /c ($steps -join ' && ')
if ($LASTEXITCODE -ne 0) { throw "Windows build failed with exit code $LASTEXITCODE" }
