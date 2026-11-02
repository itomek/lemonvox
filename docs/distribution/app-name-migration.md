# LemonVox app-name migration (issue #4)

New packages contain `LemonVox.app`, `CFBundleName=LemonVox`, and the existing lemon/waveform icon. The internal executable remains `LemonVoxPrototype`: changing that is unnecessary for the visible name and would add an identity variable. Bundle identifier `com.itomek.lemonvox.prototype`, preference keys, signing commands/identity selection, and Sparkle public key are unchanged.

## Sparkle 2.10.1: first update retains the old path

Pinned official source:

- [SUHost.m](https://github.com/sparkle-project/Sparkle/blob/2.10.1/Sparkle/SUHost.m): `name` prefers `SUBundleName` (already LemonVox in the old release).
- [SUInstaller.m](https://github.com/sparkle-project/Sparkle/blob/2.10.1/Autoupdate/SUInstaller.m): incoming application discovery accepts the host filename, host display name plus `.app`, or matching bundle ID. Thus the old host can discover `LemonVox.app`.
- [ConfigCommon.xcconfig](https://github.com/sparkle-project/Sparkle/blob/2.10.1/Configurations/ConfigCommon.xcconfig): `SPARKLE_NORMALIZE_INSTALLED_APPLICATION_NAME = 0`. The installer uses `host.bundlePath`. Even optional normalization uses the current host, and avoids overwriting an occupied normalized path.
- [SUPlainInstaller.m](https://github.com/sparkle-project/Sparkle/blob/2.10.1/Autoupdate/SUPlainInstaller.m): installs at the selected installation path. [Publishing guidance](https://sparkle-project.org/documentation/publishing/) recommends retaining the archive app name; the pinned matching-ID/display-name code is the specific evidence for discovery here.

**Shipping a renamed archive does not rename an existing Sparkle installation on disk.** An update begun from `LemonVox Prototype.app` installs new contents at `LemonVox Prototype.app`. Its bundle name becomes LemonVox, but Applications still has the old filename. A new installation, or update begun from `LemonVox.app`, uses the new path. No custom updater or automatic filesystem rename is introduced.

## One-time migration route

After a final signed/notarized artifact exists, update the existing cask token `lemonvox` with its exact version, URL/checksum and `app "LemonVox.app"`; use [the staged reference](lemonvox.rb.in). Do not reuse the old ZIP checksum. The observed live cask at review was 0.2.0 with `app "LemonVox Prototype.app"`; this task does not publish a cask update.

Homebrew source reviewed at commit `c6069d2a943b71b0a90d91fc1ac93cb9c1634dcc`:
[upgrade.rb](https://github.com/Homebrew/brew/blob/c6069d2a943b71b0a90d91fc1ac93cb9c1634dcc/Library/Homebrew/cask/upgrade.rb) loads the installed old cask definition, backs up/removes its artifacts before installing the new cask's artifacts.
[artifact/moved.rb](https://github.com/Homebrew/brew/blob/c6069d2a943b71b0a90d91fc1ac93cb9c1634dcc/Library/Homebrew/cask/artifact/moved.rb) uses the old target for `move_back`; different new target names do not match the empty-directory optimization. Existing occupied new targets cause a conflict rather than an authorized overwrite. Ordinary upgrade does not zap preferences. Because this cask has `auto_updates true`, explicitly request its upgrade with `brew upgrade --cask --greedy itomek/lemonvox/lemonvox` after publication; do not assume a routine upgrade includes it.

For a manually installed copy, the one-time owner migration is to quit it, verify the exact old and new bundles, back up the old bundle outside Applications, then place the verified new bundle at `LemonVox.app`. Leave one active bundle in Applications. Do not run a blind uninstall/zap, delete preferences, reset privacy permissions, or overwrite an existing candidate. These steps are documentation, not actions performed by this task.

## Evidence and release gates

- Isolated ad-hoc package `/private/tmp/lemonvox-rename-package3`: test-only version 0.1.1/build 5, arm64; visible name LemonVox; existing icon visually inspected; executable/bundle ID unchanged. ZIP SHA-256 `c8563a5664a81a299b3337fe28234c6b4711ffe6ac29dbbd469955b6d588c39d`. Built from the working diff over recovery head `4a1562942e1987641471c942581918346c0a1a4d` (`source_dirty=true`). This is not a release artifact/version proposal.
- Build suite, portable-core gate, package signature/roundtrip/tamper checks, pinned Sparkle dependency checks, historical/new release-name reader regression, and fixture-key feed/signature/monotonic-build checks passed. Packaging needed an approved sandbox-free rerun for iconutil; feed generation needed its ordinary cache access.
- `tests/app_name_migration_test.py` compiles the exact SHA-pinned upstream source-selector and destination blocks with lightweight host stubs. It confirms renamed-bundle discovery and old-path retention, then models filesystem replacement and one-time migration in temporary directories. Final modeled Applications has only `LemonVox.app`; the isolated preference-domain plist stays byte-identical. Run it with downloaded official `SUInstaller.m`, `ConfigCommon.xcconfig`, and the isolated new app path. It does not execute the production installer or Homebrew transaction.
- Existing `/Applications/LemonVox Prototype.app` was inspected read-only: observed 0.2.0/build 5, designated requirement identifier `com.itomek.lemonvox.prototype`, Apple generic anchor and team `BNVA36GJRJ`. The new local artifact is ad-hoc: it does **not** establish that certificate-backed requirement or notarization. Release signing configuration is unchanged.

**Still required before release/migration acceptance:** verified final artifact with an increasing build/version, Developer ID designated requirement and Gatekeeper/notarization checks; actual old-to-new Sparkle transaction/relaunch and Homebrew upgrade in a disposable environment; conflict/rollback validation; real preference persistence; owner verification of microphone, Accessibility/event posting and dictation/paste continuity after migration. No TCC continuity claim follows from ID preservation or unit tests. Earlier recovery evidence distinguishes automated fixed-text checks from owner-reported real dictation/paste success; neither proves rename compatibility. The installed release and running test candidate were not moved, installed, relaunched, or granted permissions here.
