# Windows distribution design

## Intent and scope

Make LemonVox easy to install and launch on Windows, with Microsoft Store and
WinGet distribution included in the Windows milestone. The user selected MSIX
for the Store, a separate WinGet community package, and a portable EXE for direct
downloads on 2026-11-08. A Partner Center developer account and app reservation
do not exist yet. The user approved this design and inline implementation without
further design/plan check-ins; publication remains a separate final step.

## Current state

The native x64 EXE, per-user PowerShell installer, Start menu shortcut and ZIP
packager already exist. The installer preserves settings and supports uninstall.
The Windows CI workflow builds and tests but does not publish release artifacts.
The protected draft release workflow currently packages macOS only. GitHub
issues #10 and #15 are closed for the earlier Windows port; their completion
does not establish Store or WinGet availability.

## Approaches

The selected Store route is MSIX around the existing native Win32 executable.
Microsoft signs and hosts certified Store MSIX packages and supplies updates.
The alternative, listing a hosted EXE/MSI installer, requires publisher-managed
hosting, signing and updates. Retain the portable EXE for immediate testing and
direct downloads, and use it for an initial WinGet community manifest. These
channels do not require a UI framework rewrite.

## Proposed implementation

- Build Release x64 from an identified source commit. Produce the existing ZIP,
  a standalone EXE, checksums and source/version metadata. Local previews use
  version 0.0.0 and are explicitly separate from public release versions.
- Add an MSIX packaging script using the installed Windows SDK MakeAppx tool,
  an explicit package identity supplied as parameters, and image assets derived
  from the existing lemon icon. Keep the app a normal medium-integrity desktop
  process with the required full-trust declaration. Do not introduce elevation
  or change its microphone, shortcut or insertion behavior.
- Generate a schema-valid unsigned MSIX for structural validation before a
  Store account is available. Once the name is reserved, use the exact Partner
  Center Name, Publisher and publisher display name. Store signing follows
  certification; unsigned structural validation is not installation validation.
  Keep Application Id `LemonVox` stable. Require an explicit Store package
  version `X.Y.Z.0`, with a nonzero first component and bounded numeric
  components; validate increasing versions before publication. Record the
  product release version separately rather than silently deriving the Store
  version from a macOS build number or a 0.0.0 local preview.
- Generate WinGet community manifests from an explicit versioned HTTPS URL,
  the actual standalone EXE and its SHA256, with publisher/package metadata.
  Use portable EXE support initially. Validate the manifests and perform an
  isolated install/launch/upgrade/uninstall check before submission. A local
  preview or placeholder URL must never be submitted.
  Use `InstallerType: portable` and stable command alias `lemonvox`. Request user
  scope at installation (`--scope user`); WinGet rejects a manifest `Scope` field
  for portable installers, so leave it out of generated YAML.
  This path does not execute `install.ps1` or inherit its Start menu and startup
  shortcuts. Test and document its own launch and uninstall behavior separately.
- Extend CI to validate packaging without signing credentials. Add Windows
  artifacts to the protected draft release process without weakening its
  existing approval gates or changing macOS release behavior.
  Build and validate Windows assets before the existing job creates the one
  draft release, then upload them to that same draft with distinct asset and
  checksum filenames. Do not create a competing draft or overwrite Mac hashes.
- Document launch and removal for each channel, the need to install/run
  Lemonade separately, and the difference between the Store and community
  WinGet sources. Public installation commands are documented only after real
  identifiers and published artifacts exist.

## Acceptance and validation

- [ ] Direct package: build, checksum, install, Start menu launch, settings kept
  on upgrade, and uninstall tested; optional startup remains opt-in.
- [ ] MSIX: manifest/schema and package contents validated, correct assets and
  account identity, Windows App Certification Kit run, and installed-package
  behavior checked on an authorized test machine.
- [ ] Installed MSIX: confirm tray, global shortcuts, microphone capture,
  loopback Lemonade, guarded insertion and clipboard restoration still work.
  Check settings across package updates and coexistence with a direct install.
  Preserve the `Local\LemonVox.Windows.SingleInstance` guard. Explicitly test
  adoption/isolation of `HKCU\Software\LemonVox` under packaged registry
  virtualization, including uninstall; do not assume cross-channel settings or
  upgrades are shared. Installed-package testing is a release gate, using an
  owner-authorized signing/test deployment route or Store flight when available.
- [ ] Store: accurate listing, screenshots, support/privacy information and
  separate Lemonade prerequisite; account and app reservation completed;
  certification passed; installed Store package verified.
- [ ] WinGet: real public URL and SHA256, local validation and install lifecycle,
  community submission accepted, and public install/upgrade verified.
- [ ] Release artifacts trace to the reviewed source commit and agreed version;
  previews, unsigned packages and unpublished manifests are labeled honestly.

## Owner-dependent publication steps

Partner Center registration/verification, the app reservation and publisher
identity, and review of the final Store listing are needed before submission.
Public release, Store submission and WinGet submission remain separate final
actions after their concrete artifacts are prepared and reviewed. This design
does not require buying or creating a code-signing certificate for Store MSIX
submission. No signing key, certificate trust or Windows security setting is
changed as part of preparing the local artifacts.

## References

Checked 2026-11-08:

- [Microsoft Store distribution options](https://learn.microsoft.com/en-us/windows/apps/distribute-through-store/how-to-distribute-your-win32-app-through-microsoft-store)
- [MSIX package identity and signing requirements](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/app-package-requirements)
- [Manual MSIX package components](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-manual-conversion)
- [WinGet manifest authoring](https://learn.microsoft.com/en-us/windows/package-manager/package/manifest)
- [WinGet submission](https://learn.microsoft.com/en-us/windows/package-manager/package/)
