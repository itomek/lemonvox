#Requires -Version 5.1
<#
.SYNOPSIS
Semi-automated insertion matrix for windows/tests/insertion-matrix.md.

.DESCRIPTION
Launches its own Notepad (on a temporary file) and its own Edge/Chrome windows
(fresh temporary --user-data-dir, data: URL textarea), runs
insertion_matrix_probe against each, then closes them and deletes every
temporary file and profile. It holds Local\LemonVoxInteractiveTestLock for the
whole run and the probe refuses to type unless the launched window is the
foreground window. Pre-existing user windows are never targeted: windows are
matched by a unique title and by the process IDs this script started.

.EXAMPLE
powershell -ExecutionPolicy Bypass -File windows\tests\insertion-matrix.ps1 -Probe C:\temp\lv\src\insertion\insertion_matrix_probe.exe
#>
param(
    [Parameter(Mandatory = $true)][string]$Probe,
    [string[]]$Targets = @('notepad', 'edge', 'chrome')
)
$ErrorActionPreference = 'Stop'
$expected = "LemonVox fixed text $([char]0x2713) 123"
$work = Join-Path $env:TEMP ("lv-insertion-matrix-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $work | Out-Null
$results = [ordered]@{}

function Invoke-Probe([string[]]$arguments) {
    $output = & $Probe @arguments 2>&1 | Out-String
    Write-Host $output.Trim()
    return @{ Output = $output; Code = $LASTEXITCODE }
}

function Stop-Owned([int[]]$ids) {
    foreach ($id in $ids) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
}

function Get-ProfileProcessIds([string]$profileDir, [string]$exe) {
    $escaped = $profileDir.Replace('\', '\\')
    @(Get-CimInstance Win32_Process -Filter "Name = '$([System.IO.Path]::GetFileName($exe))' AND CommandLine LIKE '%$escaped%'" | ForEach-Object { [int]$_.ProcessId })
}

function Test-Notepad {
    if (Get-Process -Name Notepad -ErrorAction SilentlyContinue) {
        # A running Notepad may adopt the file as a tab in a person's window.
        return 'skipped (Notepad already running for the user)'
    }
    $name = 'lv-matrix-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
    $file = Join-Path $work "$name.txt"
    [System.IO.File]::WriteAllText($file, '')
    $launcher = Start-Process -FilePath notepad.exe -ArgumentList "`"$file`"" -PassThru
    Start-Sleep -Seconds 3
    $ids = @(Get-Process -Name Notepad -ErrorAction SilentlyContinue | ForEach-Object Id) + $launcher.Id
    try {
        $run = Invoke-Probe @('insert', '--title', $name, '--pids', ($ids -join ','), '--no-lock')
        if ($run.Code -ne 0) { return 'fail' }
        # Save into the temporary file so closing discards nothing and the
        # file proves exactly what Notepad received.
        Invoke-Probe @('keys', '--title', $name, '--pids', ($ids -join ','), '--combo', 'ctrl+s', '--no-lock') | Out-Null
        Start-Sleep -Seconds 1
        $saved = [System.IO.File]::ReadAllText($file, [System.Text.Encoding]::UTF8)
        Write-Host "notepad.saved=[$saved]"
        Invoke-Probe @('keys', '--title', $name, '--pids', ($ids -join ','), '--combo', 'ctrl+w', '--no-lock') | Out-Null
        Start-Sleep -Seconds 1
        if ($saved -ceq $expected) { 'pass' } else { 'fail (saved file differs)' }
    } finally {
        Stop-Owned $ids
        Start-Sleep -Milliseconds 500
        Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
    }
}

function Test-VSCode {
    $exe = "$env:ProgramFiles\Microsoft VS Code\Code.exe"
    if (-not (Test-Path $exe)) { return 'skipped (VS Code not installed)' }
    $name = 'lv-matrix-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
    $file = Join-Path $work "$name.txt"
    [System.IO.File]::WriteAllText($file, '')
    $profileDir = Join-Path $work 'vscode-profile'
    $arguments = @("--user-data-dir=`"$profileDir`"", "--extensions-dir=`"$profileDir\ext`"", '--disable-extensions',
        '--disable-workspace-trust', '--skip-welcome', '--skip-release-notes', '--new-window', "`"$file`"")
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru
    Start-Sleep -Seconds 10
    try {
        $ids = @(Get-ProfileProcessIds $profileDir $exe) + $process.Id
        $run = Invoke-Probe @('insert', '--title', $name, '--pids', ($ids -join ','), '--no-lock')
        if ($run.Output -match 'insert.error=none' -and $run.Output -match 'clipboard.restored=1') {
            # The editor's accessible text is a window around the caret, so
            # save and compare the file instead of the UI Automation readback.
            Invoke-Probe @('keys', '--title', $name, '--pids', ($ids -join ','), '--combo', 'ctrl+s', '--no-lock') | Out-Null
            Start-Sleep -Seconds 2
            $saved = [System.IO.File]::ReadAllText($file, [System.Text.Encoding]::UTF8)
            Write-Host "vscode.saved=[$saved]"
            if ($saved -ceq $expected) { return 'pass' }
            return 'fail (saved file differs)'
        }
        if ($run.Output -match 'sends=0' -and $run.Output -match 'clipboard.restored=1' -and
            $run.Output -match 'capture.error=unavailable') {
            return 'copy fallback (editor not exposed to UI Automation)'
        }
        'fail'
    } finally {
        Stop-Owned (@(Get-ProfileProcessIds $profileDir $exe) + $process.Id)
        Start-Sleep -Seconds 2
        Remove-Item -LiteralPath $profileDir -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
    }
}

function Test-Browser([string]$label, [string]$exe, [string[]]$extra = @(), [switch]$ExpectFallback) {
    if (-not (Test-Path $exe)) { return "skipped ($label not installed)" }
    $title = 'LVMatrix' + [guid]::NewGuid().ToString('N').Substring(0, 8)
    $profileDir = Join-Path $work "$label-profile"
    # A window activated after its page loaded may keep focus on the address
    # bar or the document body. The page re-focuses its textarea (its title
    # reports that it did) and the probe waits until UI Automation reports
    # that named field as focused.
    $page = "<title>$title</title><textarea id=t aria-label=LemonVoxMatrixField rows=4 autofocus></textarea>" +
        "<script>setInterval(()=>{if(document.activeElement!==t)t.focus();" +
        "document.title='$title '+(document.activeElement===t)},200)</script>"
    $url = 'data:text/html,' + [Uri]::EscapeDataString($page)
    $arguments = @("--user-data-dir=`"$profileDir`"", '--no-first-run', '--no-default-browser-check',
        '--new-window') + $extra + @($url)
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru
    Start-Sleep -Seconds 5
    try {
        $ids = @(Get-ProfileProcessIds $profileDir $exe) + $process.Id
        if ($ExpectFallback) {
            # The textarea has DOM focus (title ends in 'true') but the browser
            # does not expose it to UI Automation, so LemonVox must not paste.
            $run = Invoke-Probe @('insert', '--title', "$title true", '--pids', ($ids -join ','), '--no-lock')
            $fallback = $run.Output -match 'capture.error=unavailable' -and $run.Output -match 'sends=0' -and
                $run.Output -match 'clipboard.restored=1'
            if ($fallback) { 'pass (copy fallback: web content not exposed to UI Automation)' } else { 'fail' }
        } else {
            $run = Invoke-Probe @('insert', '--title', "$title true", '--pids', ($ids -join ','), '--focus-name',
                'LemonVoxMatrixField', '--no-lock')
            if ($run.Code -eq 0) { 'pass' } else { 'fail' }
        }
    } finally {
        Stop-Owned (@(Get-ProfileProcessIds $profileDir $exe) + $process.Id)
        Start-Sleep -Seconds 2
        Remove-Item -LiteralPath $profileDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$mutex = New-Object System.Threading.Mutex($false, 'Local\LemonVoxInteractiveTestLock')
$owned = $false
try {
    try { $owned = $mutex.WaitOne([TimeSpan]::FromMinutes(15)) } catch [System.Threading.AbandonedMutexException] { $owned = $true }
    if (-not $owned) { throw 'Timed out waiting for the interactive test lock.' }
    foreach ($target in $Targets) {
        switch ($target) {
            'notepad' { $results.notepad = Test-Notepad }
            'vscode' { $results.vscode = Test-VSCode }
            # InPrivate avoids the implicit sign-in and first-run panes of a fresh profile.
            'edge' { $results.edge = Test-Browser 'edge' "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe" @('--inprivate') }
            'chrome' { $results.chrome = Test-Browser 'chrome' "$env:ProgramFiles\Google\Chrome\Application\chrome.exe" -ExpectFallback }
            default { $results[$target] = 'unknown target' }
        }
    }
} finally {
    if ($owned) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

$os = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
Write-Host "windows=$($os.DisplayVersion) build $($os.CurrentBuild).$($os.UBR)"
$results.GetEnumerator() | ForEach-Object { Write-Host "$($_.Key)=$($_.Value)" }
if ($results.Values | Where-Object { $_ -like 'fail*' }) { exit 1 }
