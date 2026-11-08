# Windows Distribution Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Produce local MSIX and WinGet artifacts and prepare the protected Windows release path.

**Architecture:** Keep the native EXE and existing ZIP installer. Build MSIX with the installed
Windows SDK and generate WinGet portable manifests from the exact EXE/checksum. CI validates
unsigned artifacts; the existing protected release job remains the sole draft creator.

**Tech Stack:** PowerShell 5.1+, Windows SDK MakeAppx, WinGet, GitHub Actions, existing CMake/MSVC.

**Spec:** [Approved distribution design](../specs/2026-11-08-windows-distribution-design.md)

## Global constraints

No public publication, signing material, certificate trust or security-setting changes.
Partner Center identity is explicit input; preview identity is never a claimed Store identity.
Keep macOS release approvals, reviewed-source checks and assets intact. No new runtime dependency.

## Review focus

Reject malformed versions/identities/URLs before creating output. Do not overwrite artifacts.
WinGet hashes the exact downloaded EXE, not the ZIP. Portable installation has its own lifecycle.
MSIX must target x64 desktop/full trust and retain app identity across updates.
Unsigned structural checks cannot substitute for installed-package validation.
Release jobs must use the same reviewed source and never race to create drafts.

## Task 1: Local distribution artifacts

Files: `windows/scripts/package-msix.ps1`, `windows/scripts/new-winget-manifest.ps1`,
`windows/scripts/package.ps1`, `windows/tests/distribution_test.ps1`.

Interfaces: MSIX consumes `-PackageDirectory`, explicit identity/publisher/store version and
`-Output`; WinGet consumes `-Executable`, `-InstallerUrl`, product version, publisher/id and
`-Output`. Both validate inputs and require new output directories.

- [x] Add executable tests for manifest generation, invalid inputs, no overwrite, exact hash,
  MSIX pack/unpack validation and included assets. Observe failures before implementation.
- [x] Implement SDK MSIX packaging with assets derived from the existing icon and full trust.
- [x] Implement three-file WinGet portable manifests with explicit HTTPS EXE URL and real hash.
- [x] Extend the ZIP packager to export a versioned standalone EXE, icon, and checksum manifest.
- [x] Run PowerShell tests, SDK structural validation and WinGet manifest validation.

## Task 2: CI and protected release integration

Files: `.github/workflows/windows.yml`, `.github/workflows/release.yml`,
`windows/tests/distribution_test.ps1`, `windows/README.md`.

- [x] Add hosted-safe distribution validation using the same tools and test script.
- [x] Build Windows artifacts at the reviewed commit before the existing protected Mac release
  job; pass artifacts through pinned Actions and upload into its one draft release.
- [x] Include optional account-derived MSIX identity/version inputs; do not silently publish
  preview identity packages. Keep Store MSIX and WinGet submission as manual final actions.
- [x] Validate workflow structure and commands locally where possible; hosted signing remains
  unrun. Record exact manual installed-MSIX and WinGet lifecycle gates in the README.

## Task 3: Verification and handoff

- [x] Run the full Windows suite and the distribution tests on the final source.
- [x] Request an independent review, address material findings and verify affected checks.
- [x] Rebuild local preview artifacts and verify hashes; leave the installed app usable.
- [x] Report artifacts, tested behavior and account/publication-dependent work honestly.

## Execution ledger

- 2026-11-08: User approved the design and inline implementation without further planning
  check-ins. Public release/Store/WinGet submission remain separate final steps.
- Ruling: Continue in the existing checkout on `codex/windows-distribution` to preserve the
  active Windows debugging environment; do not create another checkout or interrupt the app.
- Target-selection fix committed separately after 24/24 Windows tests and independent review.
- Existing Release ZIP preview installed per-user; Start menu target and EXE smoke validated.
- Ruling: Use per-command local author `Codex <codex@localhost>` for local commits because
  this checkout has no configured identity; do not change the user's Git configuration.
- Task 1 RED: distribution test failed because MSIX script did not exist. First real run
  found PowerShell Icon.ToBitmap cannot decode the existing PNG ICO frame; extract it directly.
- Ruling: Omit manifest Scope for portable WinGet. Real `winget validate` rejects it with
  a warning/nonzero result; user scope is an install command option instead.

- Task 1 GREEN: real MakeAppx pack/unpack checks, native WinGet 1.29 validation,
  input rejection, standalone bytes/checksums and official pinned-schema validation passed.
- Task 2: actionlint 1.7.12 passed both workflows; schema fallback also rejects a
  deliberately mismatched cross-file version. Hosted release execution remains pending.
- Task 3: full Windows suite passed 24/24; fresh Release packaging passed 14/14.
  Independent review found no actionable defects. Final previews are rebuilt from the
  saved commit; installed MSIX, final-URL WinGet lifecycle and public submissions remain
  explicit account/publication gates, not claimed as completed.

- Final preview regression: native WinGet rejected PREVIEW.txt beside manifests. The
  new Version 0.0.0 -Preview regression failed before the fix and passed afterwards;
  preview warnings now live in YAML comments. Native and JSON schema paths pass.
- Final Release build (not only Debug) passed all 24 Windows tests. Final preview
  binaries were built from clean source 03a4659; the follow-up changes affect only
  manifest comments, their test and documentation. Independent follow-up review is clean.

- Scope update: owner authorized WinGet-only plus GitHub publication, using ID
  LemonVox.LemonVox and publisher LemonVox. Store/Partner Center are optional.
- Added independent protected Windows draft workflow so the Windows release does not
  depend on the macOS pipeline or replace the older v0.5.0 macOS draft. Preparing
  Windows v0.5.1, then anonymous download/hash/run checks and upstream WinGet submission.
- New-ID native WinGet and pinned schema tests passed; workflow lint passed and
  independent read-only review found no blocking defects. Publication is still pending.
