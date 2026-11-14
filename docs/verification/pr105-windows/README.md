# PR 105 Windows verification

Tested product commit: `249db66f40acb57c4127f0a52b0eabca3d8d2ce3`.
Host: Windows Enterprise 26H2, build 26300.9974 (registry product name reports Windows 10 Enterprise). MSVC x64 Release build, running on this Windows desktop.

Commands:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File windows/scripts/build.ps1 -Output C:/temp/lemonvox-pr105 -ExcludeLabels 'interactive|lemonade|audio-device'
ctest --test-dir C:/temp/lemonvox-pr105 --output-on-failure -L interactive -LE lemonade
ctest --test-dir C:/temp/lemonvox-pr105 --output-on-failure -L audio-device
ctest --test-dir C:/temp/lemonvox-pr105 --output-on-failure -L lemonade
```

CMake/CTest were invoked from the Visual Studio Build Tools installation. Logs in this directory preserve results.

- Build: all 106 steps succeeded.
- Noninteractive tests: 17/17 passed.
- Desktop tests: 6/6 passed (native insertion, shortcut, tray/indicator demo, UI, Settings, application smoke).
- Capture-device probe: 1/1 passed; it initializes the device without recording microphone audio.
- Native insertion also passed a separate targeted run, including queued and pre-dispatch cancellation and clipboard restoration.
- Real Lemonade pipeline and application E2E tests skipped because no server was reachable at 127.0.0.1:13305. `lemonade_net_test` reports this skip in its text but exits 0, so its CTest Passed result is not real-server acceptance.

Two parallel agents audited Windows clipboard ownership/tray behavior and prepared/reviewed a disposable production-app acceptance harness. Product source was not changed.

Real microphone/server acceptance remains pending. Full native cancel-during-insertion followed by explicit recovery copy and observation for delayed overwrite is not established by the insertion unit of acceptance alone. This evidence does not justify merging PR 105 or closing issue 102.

## Production tray recovery with disposable fixtures

`recovery.py` launches the unmodified production binary with a temporary registry profile, generated silent WAV and a loopback HTTP fixture. It uses the real global shortcut, native insertion into its own EDIT control, and the production tray popup/copy callback. This is synthetic acceptance, not speech recognition or microphone acceptance. Every copy starts with a distinct clipboard sentinel, then asserts exact expected clipboard text.

The final recorded run passed initial disabled menu state, first copy, repeated copy, new result replacement, cleanup failure recovering raw ASR, ASR failure preserving the previous text, restart disabling the action, and a final copy for the screenshot. `recovery.log` records each result.

Native cancellation was left pending after an ambiguous test-harness Cancel selection. Insertion failure and cancel-during-insertion followed by explicit recovery copy also remain pending. Core/controller and native insertion suites cover related pieces, but do not substitute for these complete production-app scenarios.

The screenshot shows the real enabled production tray menu over the disposable native target after a successful recovery copy. Text is fixture-generated. No user text/settings were used as test input. The harness removed its temporary registry profile and closed its app process after capture. Its clipboard cleanup restores the prior Unicode text only, not arbitrary rich clipboard formats.

Harness command (requires desktop and no other LemonVox instance):

```powershell
python recovery.py C:/temp/lemonvox-pr105/src/app/LemonVox.exe --hold
```

Remove any old `release` file from the harness directory before `--hold`; create that file to release the screenshot hold and clean up.
