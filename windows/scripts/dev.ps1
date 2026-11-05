#Requires -Version 5.1
<#
.SYNOPSIS
Developer loop used by the VS Code tasks: quits a running .build copy, builds Debug without tests, optionally launches it.

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File windows\scripts\dev.ps1 -Run
#>
param(
    # Launch the built app and wait until it quits.
    [switch]$Run
)
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$output = Join-Path $root '.build\windows-debug'
$exe = Join-Path $output 'src\app\LemonVox.exe'

Get-Process LemonVox -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -eq $exe } |
    Stop-Process -Force

& (Join-Path $PSScriptRoot 'build.ps1') -Output $output -Configuration Debug -SkipTests

if ($Run) {
    $app = Start-Process -FilePath $exe -PassThru
    $app.WaitForExit()
    exit $app.ExitCode
}
