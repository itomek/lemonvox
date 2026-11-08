#Requires -Version 5.1
param(
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [Parameter(Mandatory = $true)][string]$IconPath,
    [Parameter(Mandatory = $true)][string]$Output,
    # Hosted Server images do not ship WinGet. Supply Python with the pinned
    # dev dependencies to run official JSON schema checks there instead.
    [string]$SchemaPython
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$msixScript = Join-Path $root 'windows\scripts\package-msix.ps1'
$wingetScript = Join-Path $root 'windows\scripts\new-winget-manifest.ps1'
function Assert([bool]$condition, [string]$message) {
    if (-not $condition) { throw $message }
}
function Rejects([scriptblock]$action, [string]$message) {
    $rejected = $false
    try { & $action | Out-Null } catch { $rejected = $true }
    Assert $rejected $message
}
Assert (Test-Path $msixScript) 'MSIX packaging script is missing'
Assert (Test-Path $wingetScript) 'WinGet manifest generator is missing'
Assert (-not (Test-Path $Output)) 'Tests require a new output directory'
$sourceVersion = (Get-Content (Join-Path $PackageDirectory 'VERSION.txt') -TotalCount 1).Trim()
$packageRoot = Split-Path ([IO.Path]::GetFullPath($PackageDirectory)) -Parent
$standalone = Join-Path $packageRoot "LemonVox-$sourceVersion-windows-x64.exe"
Assert (Test-Path -LiteralPath $standalone -PathType Leaf) 'Packager did not export standalone EXE'
$packagedExeHash = (Get-FileHash (Join-Path $PackageDirectory 'LemonVox.exe') -Algorithm SHA256).Hash
Assert ((Get-FileHash $standalone -Algorithm SHA256).Hash -eq $packagedExeHash) 'Standalone EXE differs from ZIP-stage EXE'
foreach ($line in Get-Content (Join-Path $packageRoot 'SHA256SUMS-windows-x64.txt')) {
    $digest, $filename = $line -split '  ', 2
    Assert ((Get-FileHash (Join-Path $packageRoot $filename) -Algorithm SHA256).Hash -eq $digest) 'Direct artifact checksum mismatch'
}
New-Item -ItemType Directory $Output | Out-Null
$fixture = Join-Path $Output 'source'
New-Item -ItemType Directory $fixture | Out-Null
foreach ($name in 'LemonVox.exe', 'LICENSE.txt', 'THIRD_PARTY_NOTICES.txt', 'VERSION.txt') {
    Copy-Item -LiteralPath (Join-Path $PackageDirectory $name) -Destination $fixture
}
Copy-Item -LiteralPath $IconPath -Destination (Join-Path $fixture 'lemonvox.ico')
$msixArgs = @{
    PackageDirectory = $fixture; PackageName = 'LemonVox.LocalPreview'
    Publisher = 'CN=LemonVox Preview & Co'; PublisherDisplayName = 'Preview & Co'
    StoreVersion = '1.2.3.0'; Output = (Join-Path $Output 'msix')
}
foreach ($invalid in '0.1.0.0', '1.0.0.1', '1.65536.0.0', '1.0.0') {
    $bad = $msixArgs.Clone(); $bad.StoreVersion = $invalid
    Rejects { & $msixScript @bad } "Accepted invalid Store version $invalid"
    Assert (-not (Test-Path $bad.Output)) 'Invalid input created output'
}
& $msixScript @msixArgs
$package = @(Get-ChildItem $msixArgs.Output -Filter '*.msix')
Assert ($package.Count -eq 1) 'Expected one unsigned MSIX'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$unpacked = Join-Path $Output 'unpacked'
[IO.Compression.ZipFile]::ExtractToDirectory($package[0].FullName, $unpacked)
[xml]$manifest = Get-Content (Join-Path $unpacked 'AppxManifest.xml') -Raw
Assert ($manifest.Package.Identity.Name -eq 'LemonVox.LocalPreview') 'Wrong package identity'
Assert ($manifest.Package.Identity.Publisher -eq 'CN=LemonVox Preview & Co') 'Publisher XML escaping failed'
Assert ($manifest.Package.Identity.Version -eq '1.2.3.0') 'Wrong Store version'
Assert ($manifest.Package.Identity.ProcessorArchitecture -eq 'x64') 'Wrong MSIX architecture'
Assert ($manifest.Package.Applications.Application.Id -eq 'LemonVox') 'Unstable application id'
Assert ($manifest.Package.Applications.Application.EntryPoint -eq 'Windows.FullTrustApplication') 'Not a desktop app'
Assert ($manifest.Package.Capabilities.Capability.Name -contains 'runFullTrust') 'Missing full trust capability'
$sourceHash = (Get-FileHash (Join-Path $fixture 'LemonVox.exe') -Algorithm SHA256).Hash
Assert ((Get-FileHash (Join-Path $unpacked 'LemonVox.exe') -Algorithm SHA256).Hash -eq $sourceHash) 'MSIX EXE differs'
Assert (-not (Test-Path (Join-Path $unpacked 'AppxSignature.p7x'))) 'Local MSIX should not be signed'
Add-Type -AssemblyName System.Drawing
foreach ($asset in @(@('Square44x44Logo.png', 44), @('Square150x150Logo.png', 150), @('StoreLogo.png', 50))) {
    $png = [Drawing.Image]::FromFile((Join-Path $unpacked "Assets\$($asset[0])"))
    try { Assert ($png.Width -eq $asset[1] -and $png.Height -eq $asset[1]) 'Wrong asset dimensions' }
    finally { $png.Dispose() }
}
$originalPackageHash = (Get-FileHash $package[0].FullName -Algorithm SHA256).Hash
Rejects { & $msixScript @msixArgs } 'Overwrote MSIX output'
Assert ((Get-FileHash $package[0].FullName -Algorithm SHA256).Hash -eq $originalPackageHash) 'Existing MSIX changed'

$wingetArgs = @{
    Executable = (Join-Path $fixture 'LemonVox.exe'); Version = '1.2.3'
    PackageIdentifier = 'LemonVox.LemonVox'; Publisher = 'LemonVox'
    InstallerUrl = 'https://github.com/itomek/lemonvox/releases/download/v1.2.3/LemonVox-1.2.3-windows-x64.exe'
    Output = (Join-Path $Output 'winget')
}
foreach ($invalid in 'http://example.com/app.exe', 'https://user:password@example.com/app.exe', 'https://example.com/app.zip', 'https://example.com/latest.exe') {
    $bad = $wingetArgs.Clone(); $bad.InstallerUrl = $invalid
    Rejects { & $wingetScript @bad } "Accepted invalid installer URL $invalid"
    Assert (-not (Test-Path $bad.Output)) 'Invalid URL created output'
}
$fakeExe = Join-Path $Output 'fake.exe'
[IO.File]::WriteAllText([IO.Path]::GetFullPath($fakeExe), 'not a PE executable')
$bad = $wingetArgs.Clone(); $bad.Executable = $fakeExe
Rejects { & $wingetScript @bad } 'Accepted a non-executable as x64'
Assert (-not (Test-Path $bad.Output)) 'Invalid EXE created output'
& $wingetScript @wingetArgs
$installer = Get-Content (Join-Path $wingetArgs.Output 'LemonVox.LemonVox.installer.yaml') -Raw
Assert ($installer.Contains($sourceHash)) 'WinGet does not hash the actual EXE'
Assert ($installer.Contains($wingetArgs.InstallerUrl)) 'WinGet URL changed'
Assert ($installer -match 'InstallerType: portable' -and $installer -notmatch 'Scope:') 'Wrong WinGet portable lifecycle'
Assert ($installer -match 'Commands:\s*\r?\n- lemonvox') 'Missing portable command alias'
$winget = Get-Command winget.exe -ErrorAction SilentlyContinue
if ($winget) {
    & $winget.Source validate --manifest $wingetArgs.Output --disable-interactivity
    Assert ($LASTEXITCODE -eq 0) 'WinGet manifest validation failed'
} elseif (-not $SchemaPython) {
    throw 'WinGet is required, or supply -SchemaPython on a hosted runner.'
}
if ($SchemaPython) {
    & $SchemaPython (Join-Path $PSScriptRoot 'validate_winget.py') $wingetArgs.Output
    Assert ($LASTEXITCODE -eq 0) 'WinGet JSON schema validation failed'
    $mismatched = Join-Path $Output 'mismatched-manifests'
    New-Item -ItemType Directory $mismatched | Out-Null
    Copy-Item (Join-Path $wingetArgs.Output '*.yaml') $mismatched
    $locale = Join-Path $mismatched 'LemonVox.LemonVox.locale.en-US.yaml'
    (Get-Content $locale -Raw).Replace('"1.2.3"', '"1.2.4"') | Set-Content -Encoding UTF8 $locale
    # The documents remain individually schema-valid; only our cross-file
    # consistency check should reject this version mismatch.
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $SchemaPython (Join-Path $PSScriptRoot 'validate_winget.py') $mismatched 2>$null | Out-Null
        $mismatchExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previousPreference }
    Assert ($mismatchExit -ne 0) 'Accepted mismatched manifest versions'
}
Rejects { & $wingetScript @wingetArgs } 'Overwrote WinGet output'
$previewArgs = $wingetArgs.Clone()
$previewArgs.Version = '0.0.0'; $previewArgs.Preview = $true
$previewArgs.InstallerUrl = 'https://github.com/itomek/lemonvox/releases/download/v0.0.0/LemonVox-0.0.0-windows-x64.exe'
$previewArgs.Output = Join-Path $Output 'preview-manifests'
& $wingetScript @previewArgs
Assert (@(Get-ChildItem $previewArgs.Output -File).Count -eq 3) 'Preview output contains non-manifest files'
foreach ($yamlFile in Get-ChildItem $previewArgs.Output -Filter '*.yaml') {
    Assert ((Get-Content $yamlFile.FullName -TotalCount 1) -match 'UNPUBLISHED PREVIEW') 'Preview manifest is unlabeled'
}
if ($winget) {
    & $winget.Source validate --manifest $previewArgs.Output --disable-interactivity
    Assert ($LASTEXITCODE -eq 0) 'Preview label breaks native WinGet validation'
}
if ($SchemaPython) {
    & $SchemaPython (Join-Path $PSScriptRoot 'validate_winget.py') $previewArgs.Output
    Assert ($LASTEXITCODE -eq 0) 'Preview label breaks JSON schema validation'
}
$bad = $wingetArgs.Clone(); $bad.Output = Join-Path $Output 'invalid-version'; $bad.Version = '0.0.0'
Rejects { & $wingetScript @bad } 'Preview version accepted without -Preview'
Write-Output 'distribution_test passed: SDK MSIX validation, package contents, assets, input rejection, no overwrite and WinGet schema'
