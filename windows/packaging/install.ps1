#Requires -Version 5.1
<#
.SYNOPSIS
Installs LemonVox for the current user (no administrator rights).

.DESCRIPTION
Copies LemonVox.exe to %LOCALAPPDATA%\Programs\LemonVox, adds a Start menu
shortcut and an Apps & features entry for this user. Running it again updates an
existing install in place; settings in HKCU\Software\LemonVox are preserved.

.PARAMETER StartAtSignIn
Also start LemonVox when you sign in to Windows.
#>
param([switch]$StartAtSignIn, [switch]$NoLaunch)
$ErrorActionPreference = 'Stop'

$source = $PSScriptRoot
$exe = Join-Path $source 'LemonVox.exe'
if (-not (Test-Path $exe)) { throw "LemonVox.exe not found next to install.ps1" }
$version = (Get-Content (Join-Path $source 'VERSION.txt') -TotalCount 1).Trim()

$target = Join-Path $env:LOCALAPPDATA 'Programs\LemonVox'
$startMenu = Join-Path ([Environment]::GetFolderPath('Programs')) 'LemonVox.lnk'
$startup = Join-Path ([Environment]::GetFolderPath('Startup')) 'LemonVox.lnk'
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\LemonVox'

# An update must not replace a running executable.
$installedExe = Join-Path $target 'LemonVox.exe'
Get-Process -Name LemonVox -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and ([IO.Path]::GetFullPath($_.Path) -eq [IO.Path]::GetFullPath($installedExe)) } |
    ForEach-Object { Stop-Process -Id $_.Id; $_.WaitForExit(5000) | Out-Null }

New-Item -ItemType Directory -Force $target | Out-Null
foreach ($name in 'LemonVox.exe', 'uninstall.ps1', 'README.txt', 'LICENSE.txt', 'THIRD_PARTY_NOTICES.txt', 'VERSION.txt') {
    $file = Join-Path $source $name
    if (Test-Path $file) { Copy-Item $file $target -Force }
}

$shell = New-Object -ComObject WScript.Shell
function New-Shortcut([string]$path) {
    $link = $shell.CreateShortcut($path)
    $link.TargetPath = $installedExe
    $link.WorkingDirectory = $target
    $link.Description = 'LemonVox dictation'
    $link.Save()
}
New-Shortcut $startMenu
if ($StartAtSignIn) { New-Shortcut $startup } elseif (Test-Path $startup) { Remove-Item $startup }

$size = [int]((Get-ChildItem $target -File | Measure-Object Length -Sum).Sum / 1KB)
New-Item -Force $uninstallKey | Out-Null
$uninstall = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$target\uninstall.ps1`""
$values = @{
    DisplayName = 'LemonVox'; DisplayVersion = $version; Publisher = 'LemonVox'
    DisplayIcon = "$installedExe,0"; InstallLocation = $target; UninstallString = $uninstall
    QuietUninstallString = "$uninstall -Quiet"; NoModify = 1; NoRepair = 1; EstimatedSize = $size
}
foreach ($entry in $values.GetEnumerator()) {
    $type = if ($entry.Value -is [int]) { 'DWord' } else { 'String' }
    New-ItemProperty -Path $uninstallKey -Name $entry.Key -Value $entry.Value -PropertyType $type -Force | Out-Null
}

Write-Host "LemonVox $version installed to $target"
Write-Host 'Lemonade must be installed and running separately (default http://127.0.0.1:13305/v1).'
if (-not $NoLaunch) { Start-Process $installedExe }
