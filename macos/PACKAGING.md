# macOS packaging and signing

The package builder creates a versioned app and ZIP in a new output directory. It builds/tests in isolated staging, preserves bundle ID `com.itomek.lemonvox.prototype` and executable `LemonVoxPrototype`, excludes the development keyboard-test fixture, signs and verifies the finished bundle, and records a checksum and source/version manifest. It does not install, launch, notarize or publish the app.

## Local verification package

```sh
bash scripts/package-macos.sh --ad-hoc \
  --output /absolute/new/path/lemonvox-package \
  --version 0.1.0 --build-number 1
bash tests/packaging_test.sh \
  /absolute/new/path/lemonvox-package/LemonVox-0.1.0-1-arm64-ad-hoc.zip
```

Use the archive architecture emitted by the script: it builds for the host architecture and macOS 26+. Outputs are the app, ZIP, `SHA256SUMS`, `signature.txt` and `manifest.json`. The manifest records the source commit and tracked dirty state. Build release candidates from a clean reviewed checkout. Existing output paths are refused; interrupted builds leave staging for inspection.

Ad-hoc mode seals Info.plist/resources and detects tampering. It provides neither a certificate-backed update identity nor a notarized distribution, and does not solve Accessibility permission churn. A self-signed identity can support local experiments, but the current certificate mode accepts Developer ID only. Privacy-grant retention must be tested rather than inferred from signature validity.

The archive test unpacks into temporary storage, verifies signature/metadata, checks fixture exclusion and confirms resource tampering is rejected. It never launches the app or changes permissions. `bash scripts/build.sh --output /absolute/new/build` also supports isolated verification without changing the normal developer app.

## Developer ID signing

For direct distribution, supply the exact fingerprint of an existing **Developer ID Application** identity with its matching private key in your local signing keychain:

```sh
security find-identity -v -p codesigning
bash scripts/package-macos.sh \
  --identity EXISTING_40_CHARACTER_SHA1_FINGERPRINT \
  --output /absolute/new/path/lemonvox-signed \
  --version 0.1.0 --build-number 1
```

Certificate mode has no ad-hoc fallback. It enables Hardened Runtime, a secure timestamp and the audio-input entitlement. It adds no debugger, JIT, unsigned-library, sandbox or permission-bypass entitlements. Handle keychain unlock/use prompts yourself. The builder creates no certificates, keys, credentials or trust overrides. Keep signing keys and passwords outside source control.

Keep the same bundle ID, executable, compatible signing team/designated requirement and installation location across updates. Matching the bundle ID alone does not establish continuity; never weaken the designated requirement to accept arbitrary code with that identifier. Verify that a second signed version satisfies the earlier version's requirement before testing an installed update.

Certificate continuity is not a guarantee that macOS will retain every privacy grant. Accessibility and microphone approvals remain user-controlled. Moving from an ad-hoc development build to a certificate-backed app can require a one-time manual approval. With the user's permission, test two signed versions at a stable installation location, manually grant required access, replace the first with the second, and check real shortcut/capture/insertion behavior. Preserve the working app and settings throughout. Do not reset TCC or copy permission databases.

## Notarization

An Apple-trusted direct release uses Developer ID signing, Hardened Runtime, a secure timestamp, Apple notarization and a stapled ticket. Notarization scans the build; it is not an App Store submission. Set up notarization authentication privately with Apple's normal Keychain flow. Never put credential values in shell arguments, logs or source files.

Submit the signed ZIP once with `notarytool`, retain its submission ID, and poll that same job to a terminal result. Inspect the notarization log. After acceptance, unpack a fresh copy of the exact submitted archive, staple the app with `stapler`, and validate the ticket, strict code signature and Gatekeeper assessment. Recreate the ZIP and checksum after stapling, then repeat validation after extraction. Preserve the submission and original archive for provenance. The builder does not automate credentials, submission or stapling.

References: [Apple code-signing requirements and update recognition](https://developer.apple.com/library/archive/technotes/tn2206/), [Apple notarization requirements](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution).

## Distribution

Homebrew cask is the direct-distribution plan. A cask needs an immutable release URL, exact version and final SHA-256, actual app-bundle name, macOS minimum and supported architecture. The current builder emits host-architecture artifacts; advertise only architectures built and tested. Public artifact hosting and release publication are separate steps. Homebrew supplies neither signing trust nor privacy approvals; preserve settings during ordinary upgrade/uninstall and do not disable quarantine or reset permissions.

Mac App Store distribution needs a separate feasibility effort. [App Review guideline 2.4.5](https://developer.apple.com/app-store/review/guidelines/) requires appropriate sandboxing, while [App Sandbox documentation](https://developer.apple.com/documentation/security/protecting-user-data-with-app-sandbox) restricts assistive Accessibility API use. Current cross-app targeting/insertion and Accessibility-dependent modifier monitoring need a supported sandbox-compatible approach before equivalent store behavior can be promised. The separate Lemonade prerequisite also needs assessment against guideline 4.2.3. No store acceptance is implied.

Homebrew references: [Cask Cookbook](https://docs.brew.sh/Cask-Cookbook), [maintaining a tap](https://docs.brew.sh/How-to-Create-and-Maintain-a-Tap).
