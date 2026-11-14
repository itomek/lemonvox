# Windows development

Run these commands from the repository root.

## Requirements

- Build: Visual Studio 2022 Build Tools with the "Desktop development with C++" workload
  (MSVC x64 toolset 14.44 tested). CMake and Ninja are used from that installation.
- Windows SDK 10.0.26100.
- No other SDKs or runtimes: the host uses only in-box Win32 libraries and links the C runtime
  statically, so `LemonVox.exe` has no redistributable dependency.
- Run: Windows 11; the released x64 executable also supports ARM64 emulation.
- Install and run Lemonade separately, or another compatible server on this PC or a remote machine.
  HTTP/HTTPS endpoints ending in /v1 (including /api/v1) are supported. The server must provide audio transcription, chat
  completions and discoverable compatible model metadata; OpenAI API compatibility alone
  does not guarantee model discovery or successful dictation. LemonVox neither installs nor
  starts the server. Lemonade is the tested baseline below; other servers need verification.

## Build & test

```powershell
# Configure, build (Release) and run the non-interactive tests.
powershell -NoProfile -ExecutionPolicy Bypass -File windows\scripts\build.ps1 -Output $env:TEMP\lv-build

# Also run tests labeled "interactive" (they show windows and inject input; needs an unlocked desktop).
powershell -NoProfile -ExecutionPolicy Bypass -File windows\scripts\build.ps1 -Output $env:TEMP\lv-build -Interactive
```

Options: `-Configuration Debug|Release`, `-SkipTests`, `-ExcludeLabels <regex>` (default
`interactive`). Builds use `/W4 /WX /permissive-`. Test labels:

| Label | Needs | Examples |
| --- | --- | --- |
| *(none)* | nothing | core tests, controller, net (loopback server), capture conversion, prefs |
| `interactive` | unlocked desktop; shows windows and injects input | trigger, insertion, indicator, Settings, `app_smoke`, `app_e2e_test` |
| `lemonade` | Lemonade at `127.0.0.1:13305` with Whisper-Base, kokoro-v1 and a chat model | `lemonade_net_test`, `pipeline_lemonade_test`, `app_e2e_test` |
| `audio-device` | a capture endpoint; initializes it but never starts recording | `capture_device_test` |

Hosted CI (`.github/workflows/windows.yml`) excludes `interactive|lemonade|audio-device`.
Interactive tests hold `lemonvox::win::test::InteractiveLock` (`tests/support/interactive_lock.h`)
and fail instead of injecting input when their own window is not foreground. Tests needing
Lemonade exit 77 (skipped) when it is unreachable.

No automated test records the microphone. `pipeline_lemonade_test` and `app_e2e_test` use speech
synthesized by Lemonade's TTS model and replayed through `FileCapture`, which runs the same 16 kHz
conversion as live capture. `app_e2e_test` launches the real `LemonVox.exe` with
`--settings-key Software\LemonVoxTest\e2e-<pid> --sample-wav <file>`, presses Alt twice to start
and stop through the real global hook, and checks the cleaned text lands once in its own field with
no Enter, focus unchanged and the clipboard restored. To check a real microphone, run
`<output>\src\capture\lemonvox_mic_probe.exe --seconds 5` yourself; it prints only duration and
levels.

`insertion_interactive_test` covers native EDIT, RichEdit, password controls,
focus changes between fields/windows after inspection, an unknown control with
no caret or accessibility provider, and no selected text field. It checks normal
Ctrl+V dispatch, exact text without Enter, focus preservation, clipboard restoration,
dispatch refusal and cancellation (including immediately before dispatch).

`lemonvox_ui_demo.exe` (in `<output>\src\ui`) shows the tray icon and indicator cycling through
every state; `--smoke` runs the cycle once and verifies the indicator never became foreground,
active or focused (ctest `ui_demo_smoke`).

## Packaging

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File windows\scripts\package.ps1 -Output $env:TEMP\lv-pkg -Version 0.1.0
```

builds Release, runs the hosted-safe tests and writes `LemonVox-<version>-windows-x64.zip`,
its `.sha256`, the standalone `LemonVox-<version>-windows-x64.exe`, and
`SHA256SUMS-windows-x64.txt`. `VERSION.txt` in the zip (also exported as
`VERSION-windows-x64.txt`) records the version, source commit (flagging uncommitted
changes), architecture and compiler. The zip contains `LemonVox.exe`, `install.ps1`,
`uninstall.ps1`, `README.txt`, license files.

`install.ps1` installs per user to `%LOCALAPPDATA%\Programs\LemonVox` (no elevation), adds a Start
menu shortcut, an Apps & features entry and optionally `-StartAtSignIn`. Re-running it updates in
place and keeps settings. `uninstall.ps1` removes files, shortcuts and the entry, keeping settings
unless `-RemoveSettings`.

The direct-download artifact is unsigned; SmartScreen may warn. Public release and signing
remain separate final steps after the artifacts are prepared and reviewed.
GitHub EXE/ZIP releases and the `LemonVox.LemonVox` WinGet community package are part of the Windows milestone;
Store/MSIX publication is optional.
See [distribution commands and publication checklist](DISTRIBUTION.md) for the MSIX packager,
WinGet generator, local tests and protected draft release inputs.

