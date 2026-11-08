# Windows distribution

Windows distribution is part of the Windows milestone. The supported build target is
Windows 11 x64. Lemonade and its downloaded transcription/chat models are separate
prerequisites in every channel. The app runs at normal user integrity.

The required public channels are GitHub Releases (standalone EXE and installer ZIP)
and the WinGet community package `LemonVox.LemonVox`, publisher `LemonVox`.
Microsoft Store publication and Partner Center registration are not required.

## Windows-only publication

Use **Protected Windows draft release** (`windows-release.yml`) on `main`, supplying
the full reviewed main source SHA and a new product version. It uses the existing
protected `release` environment, builds/tests the exact source, and creates a draft
containing the EXE, ZIP, source/version metadata, Windows checksums and a labeled
WinGet candidate. It never overwrites an existing tag or release. Reconcile an
uncertain/failed run before retrying.

Review and publish that Windows draft, download the public EXE and verify its hash,
test it, then regenerate the WinGet manifests without preview comments using those
exact published bytes. Submit the three manifests under
`manifests/l/LemonVox/LemonVox/<version>/` in `microsoft/winget-pkgs`.
WinGet availability follows acceptance/indexing, not merely opening the PR.

Windows publication does not submit to Apple, change the macOS update feed, replace
the older macOS `v0.5.0` draft, or require Microsoft Store credentials. Optional MSIX
tooling and the older combined draft workflow are described below for reference.

## Direct download and local launch

```powershell
windows\scripts\package.ps1 -Output .build\windows-package -Version 0.0.0
```

Use a new output directory. Version 0.0.0 is an unpublished local preview. The output
contains the standalone EXE, the per-user installer ZIP, a Windows-only checksum file
and source/version metadata. The ZIP includes the existing icon for MSIX packaging.

The standalone EXE launches directly. Extract the ZIP and run `install.ps1` for a Start
menu entry and Apps & features registration; optionally pass `-StartAtSignIn`. Existing
settings are preserved on updates. `uninstall.ps1` keeps settings unless explicitly
passed `-RemoveSettings`. This path is unsigned; it is not a Store-signed installer.

For remote testing, the EXE accepts `--sample-wav <path>` and an isolated
`--settings-key Software\LemonVoxRemoteDebug` profile. Those options replace the
microphone input, not the transcription/cleanup/insertion pipeline.

## Optional Microsoft Store MSIX

The SDK tool comes with the Windows C++ build requirements; no WinUI runtime is added.
Prepare an unsigned, schema-validated local package:

```powershell
windows\scripts\package-msix.ps1 -PackageDirectory .build\windows-package\LemonVox `
  -PackageName LemonVox.LocalPreview -Publisher 'CN=LemonVox Preview' `
  -PublisherDisplayName 'LemonVox preview' -StoreVersion 1.0.0.0 `
  -Output .build\windows-msix-preview
```

These example identity values are local placeholders. For a Store candidate, replace
them with the exact Name, Publisher and publisher display name from Partner Center.
Keep that identity and Application Id `LemonVox` stable. Supply an explicit increasing
`X.Y.Z.0` Store version; the last component is reserved and the first must be nonzero.
Verify the next version against the highest version already uploaded in Partner Center.
The product release version is separately recorded in VERSION.txt.

The script copies the existing EXE and notices, derives PNG tiles from the existing
ICO, declares the desktop/full-trust app and microphone capability, and calls MakeAppx
with schema/semantic validation enabled. It emits an `.unsigned.msix`, checksum and
`MSIX-VALIDATION.json`. It does not sign, install or publish the package. An unsigned
MakeAppx success does not prove installation or Store certification.

Before Store submission:

- Complete developer account registration and reserve LemonVox in Partner Center.
- Supply the reserved package identity, listing text, screenshots, privacy/support
  information and an explicit explanation that Lemonade is installed separately.
- Test a signed/test-deployed package through an owner-authorized route, or a Store
  flight when available; run the Windows App Certification Kit.
- Verify Start menu launch, tray, microphone and loopback Lemonade, shortcuts, target
  checks, password refusal, clipboard restoration, cancellation and normal user integrity.
- Verify settings on package upgrade. Test registry virtualization of
  `HKCU\Software\LemonVox`, initial adoption/isolation relative to a direct install,
  and uninstall behavior. Cross-channel settings are not assumed to be shared.
- Keep the single-instance guard across direct and packaged copies. Check that launching
  the other channel opens the existing instance rather than running both.
- Review the final package/listing and submit for certification. Store MSIX signing is
  provided by Microsoft during publication; no CA certificate purchase is required for
  that submission. Direct sideloading has different signing requirements.

No test signing key, trusted certificate or Windows security setting is created/changed
by these scripts. Installed-package and certification gates remain pending until a
suitable identity and authorized deployment path exist.

## WinGet community package

The initial community package uses WinGet portable EXE support. It downloads the
standalone EXE, not the ZIP, and does not run `install.ps1`. Its alias and uninstall
registration are managed by WinGet; do not assume the ZIP's Start menu/startup behavior.
The intended package id is `LemonVox.LemonVox` and command alias is `lemonvox`; the id is
not claimed to be available in the public index yet.

```powershell
windows\scripts\new-winget-manifest.ps1 `
  -Executable .build\windows-package\LemonVox-0.0.0-windows-x64.exe -Version 0.0.0 `
  -PackageIdentifier LemonVox.LemonVox -Publisher 'LemonVox' `
  -InstallerUrl 'https://github.com/itomek/lemonvox/releases/download/v0.0.0/LemonVox-0.0.0-windows-x64.exe' `
  -Output .build\winget-preview -Preview
winget validate --manifest .build\winget-preview
```

That URL is intentionally unpublished. Every preview YAML starts with an unpublished
preview warning and must not be submitted. For a real release, regenerate using the final versioned public EXE
URL and the exact published EXE bytes, without `-Preview`. The generator requires HTTPS,
a version in the URL path, no embedded credentials/query/fragment, an actual x64 PE
executable, and a new output directory. It computes the EXE's SHA256 itself.

Before submission, confirm the public URL downloads exactly those bytes, run native
`winget validate`, and test install, launch, upgrade/settings preservation and uninstall
on an isolated machine. Select user scope via `--scope user` at install time; WinGet does
not support a `Scope` field in portable manifests. Installing a local manifest may
require machine configuration; these scripts do not enable that setting.

Submit the three YAML files to `microsoft/winget-pkgs` only after review and lifecycle
validation. After acceptance, verify public `winget install --id LemonVox.LemonVox --exact
--scope user`, launch `lemonvox`, and verify upgrade/uninstall. Those commands are not
claimed to work before acceptance. The Microsoft Store `msstore` source is a separate
distribution source with its own future product identifier.

## Validation and CI

```powershell
windows\tests\distribution_test.ps1 -PackageDirectory .build\windows-package\LemonVox `
  -IconPath .build\windows-package\LemonVox\lemonvox.ico -Output .build\distribution-tests
```

This checks real MakeAppx validation, unpacked identity/EXE/assets, input rejection,
no overwrite, exact WinGet checksum and native `winget validate`. It never installs
the MSIX or records the microphone. Run the normal interactive suite separately.

Hosted Windows Server runners do not ship WinGet. CI installs the pinned development
dependencies PyYAML 6.0.2 and jsonschema 4.23.0 and supplies `-SchemaPython python`.
When native WinGet is absent, official schemas pinned to a Microsoft source commit
and cross-file identity checks are used. Native WinGet and install lifecycle checks
still remain mandatory before public submission. These are build/test dependencies,
not app runtime dependencies.

## Protected draft releases

The independent Windows-only workflow above is the current Windows publishing path.
The remainder of this section describes the older combined macOS/Windows workflow.

The existing release workflow still requires the protected `release` environment and
an exact reviewed main commit. A read-only Windows job builds and validates artifacts
first. The macOS job depends on it, verifies the Windows source/version/checksums, and
creates the one existing draft release containing both platforms' assets. It preserves
Apple signing/notarization and the Mac update-feed checks. Nothing is automatically
published to Microsoft Store or WinGet.

Without a Store account, leave the optional `windows_store_version` input empty; the
draft contains the direct EXE/ZIP, metadata/checksums and a labeled WinGet candidate.
To add an unsigned Store candidate later, configure the release environment variables
`WINDOWS_STORE_PACKAGE_NAME`, `WINDOWS_STORE_PUBLISHER`, and
`WINDOWS_STORE_PUBLISHER_DISPLAY_NAME` with the reserved identity and supply the next
explicit Store version. No preview identity is silently attached to a public release.

Review all installed-package/publication gates above before publishing the draft or
submitting either channel. The workflow integration is locally linted/tested; an actual
protected hosted release run still requires review and authorization.

References checked 2026-11-08: [Store distribution](https://learn.microsoft.com/en-us/windows/apps/distribute-through-store/how-to-distribute-your-win32-app-through-microsoft-store),
[MSIX requirements](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/app-package-requirements),
[WinGet submission](https://learn.microsoft.com/en-us/windows/package-manager/package/).
