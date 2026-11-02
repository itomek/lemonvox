#Requires -Version 5.1
<#
.SYNOPSIS
Removes the per-user LemonVox install. Settings are kept unless -RemoveSettings.
#>
param([switch]$RemoveSettings, [switch]$Quiet)
$ErrorActionPreference = 'Stop'

$target = Join-Path $env:LOCALAPPDATA 'Programs\LemonVox'
$installedExe = Join-Path $target 'LemonVox.exe'
Get-Process -Name LemonVox -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and ([IO.Path]::GetFullPath($_.Path) -eq [IO.Path]::GetFullPath($installedExe)) } |
    ForEach-Object { Stop-Process -Id $_.Id; $_.WaitForExit(5000) | Out-Null }

foreach ($link in (Join-Path ([Environment]::GetFolderPath('Programs')) 'LemonVox.lnk'),
                  (Join-Path ([Environment]::GetFolderPath('Startup')) 'LemonVox.lnk')) {
    if (Test-Path $link) { Remove-Item $link -Force }
}
Remove-Item 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\LemonVox' -Recurse -Force -ErrorAction SilentlyContinue
if ($RemoveSettings) { Remove-Item 'HKCU:\Software\LemonVox' -Recurse -Force -ErrorAction SilentlyContinue }

# The script may be running from the install directory; remove it after exit.
if (Test-Path $target) {
    $cleanup = "Start-Sleep -Seconds 2; Remove-Item -LiteralPath '$target' -Recurse -Force -ErrorAction SilentlyContinue"
    Start-Process powershell.exe -WindowStyle Hidden -ArgumentList '-NoProfile', '-Command', $cleanup
}
if (-not $Quiet) {
    $settingsNote = if ($RemoveSettings) { 'Settings deleted.' } else { 'Settings kept.' }
    Write-Host "LemonVox removed. $settingsNote"
}
