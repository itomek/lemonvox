# LemonVox for Windows

## Overview

The Windows host is a native C++17 tray app built on the shared portable core
(`core/`). Windows-specific code lives in `windows/src/<module>`; each module has
its own `CMakeLists.txt` and is picked up automatically by `windows/CMakeLists.txt`.
Presentation (`ui`) is kept separate from OS adapters, and every native completion
reaches the core through the single serialized `UiThreadExecutor` (`common/executor.h`).

## Requirements

- Build: Visual Studio 2022 Build Tools with the "Desktop development with C++" workload
  (MSVC x64 toolset 14.44 tested). CMake and Ninja are used from that installation.
- Windows SDK 10.0.26100.
- No other SDKs or runtimes: the host uses only in-box Win32 libraries and links the C runtime
  statically, so `LemonVox.exe` has no redistributable dependency.
- Run: Windows 11 x64 is the only tested configuration (see [Validation](#validation)).
  Windows 10 and Arm64 are untested.
- Lemonade installed and running separately, with a downloaded transcription model and a chat
  model. LemonVox neither installs, starts nor version-gates Lemonade.

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

`insertion_interactive_test` also uses a real HWND-backed UI Automation provider to reproduce
transient `UIA_E_ELEMENTNOTAVAILABLE` failures. Target acquisition retries this specific failure
up to three times at 50 ms intervals, allowing an automatic input-host-to-editor focus handoff
while the same window remains foreground and no new user input arrives. After acquisition,
normal exact-field checks still apply. Regressions cover recovery during capture and inspection,
persistent failures, changed focus/window, password and read-only fields, and cancellation.
The listener counts keyboard and mouse events in both double-tap and regular-shortcut modes;
event timestamps are not used to decide whether input intervened. Without complete input
observation, a retry conservatively requires the original focused HWND.

`lemonvox_ui_demo.exe` (in `<output>\src\ui`) shows the tray icon and indicator cycling through
every state; `--smoke` runs the cycle once and verifies the indicator never became foreground,
active or focused (ctest `ui_demo_smoke`).

## Using LemonVox

- Press **Alt twice** to start recording and Alt twice again to stop. Ordinary Alt shortcuts, holds,
  repeats, AltGr and any intervening key or mouse click cancel a pending double tap; keys are never
  swallowed. Settings offers Ctrl/Shift/Windows-key double taps or a regular shortcut
  (`RegisterHotKey`; a conflict keeps the previous shortcut).
- The bar shows preparing, recording (level and time), transcribing, cleaning and inserting without
  taking focus; it can be dragged and remembers its position. Stop, Cancel and Copy are also in the
  tray menu. Left-click the tray icon for Settings.
- Text is pasted into the field that was focused when recording started, while it stays focused
  (Settings can instead use the field focused when processing finishes). Enter is never sent.
- If the field changed, is a password field, cannot be inspected, or belongs to a higher-integrity
  (administrator) app, LemonVox offers **Copy** for manual paste. Do not run LemonVox as
  administrator to work around this; Windows UIPI blocks input to elevated windows by design.
- Recording into a password field is refused at start.
- Microphone errors are specific (privacy setting off, no device, in use, disconnected), with an
  action to open `ms-settings:privacy-microphone` or Settings.
- Debug/test launch options: `--settings` (open Settings), `--settings-key <HKCU subkey under
  Software\>`, `--sample-wav <file>` (replay a WAV instead of the microphone), `--smoke`.

Settings persist in `HKCU\Software\LemonVox`. Audio and transcripts stay in memory; the clipboard
is used briefly with history/cloud exclusion formats and restored only if nothing else changed it;
diagnostics (tray > Open diagnostics) contain fixed events, error categories and HTTP status codes
only.

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
the [distribution design](../docs/superpowers/specs/2026-11-08-windows-distribution-design.md)
records the earlier channel design; the owner subsequently made Store/MSIX publication optional.
See [distribution commands and publication checklist](DISTRIBUTION.md) for the MSIX packager,
WinGet generator, local tests and protected draft release inputs.

## Validation

Machine: Windows 11 Enterprise 26H2, build 26300.9711, x64 (AMD), MSVC 14.44, SDK 10.0.26100.
Lemonade 2026.44.1 at `http://127.0.0.1:13305/v1` with Whisper-Base (whispercpp), Gemma-4-E4B-it-GGUF
and Qwen3 GGUF chat models (llamacpp), kokoro-v1 (TTS, tests only).

| Area | Result |
| --- | --- |
| Shared core tests under MSVC | Pass (session, trigger, contracts, protocol, workflow) |
| Global trigger | Alt twice (left or right) fires once; Alt chords, holds, repeats, AltGr, intervening keys/mouse do not; typing passes through; hotkey conflict keeps previous binding; suspended while recording a shortcut |
| Indicator | Never foreground/active/focused across all states, clicks and drags |
| Fixed-text insertion | Native EDIT and RichEdit once, no newline; password field rejected; changed focus detected; clipboard restored; cancel inserts nothing |
| Real apps | See [`tests/insertion-matrix.md`](tests/insertion-matrix.md): Notepad, Edge, Chrome and VS Code (Electron) insert; changed focus and password fields in Edge/Chrome are refused; elevated windows fall back to Copy (manual check steps documented) |
| Pipeline | Synthesized "Um, send Alex the fourteen invoices on Friday." → "Send Alex the 14 invoices on Friday." through WinHTTP, Whisper-Base and Gemma cleanup |
| Full app | `app_e2e_test`: Alt twice → real app → Lemonade → pasted into another process's field ("Note: Send Alex the 14 invoices on Friday."), focus kept, no Enter, clipboard restored |
| Failure paths | Unreachable endpoint, HTTP errors, malformed/empty responses, timeouts, cancel and stale results, non-loopback endpoint, microphone denial/no device/device loss (fake device) |
| Microphone | Endpoint enumeration and initialization (48 kHz stereo) verified without recording; owner-run `lemonvox_mic_probe --seconds 5`: 5.000 s at 16 kHz mono, RMS -27.2 dBFS, peak -2.1 dBFS |
| Live dictation | Owner-verified with a real microphone and Alt twice |
| Administrator window | Owner-verified: Alt twice is not detected in an elevated terminal (UIPI), and the "Administrator window" notice appears when it becomes active; only the elevated terminal matched on the open desktop |
| Package | Install, launch, update with settings retained, uninstall with and without settings; exe 922,624 bytes, zip 437,702 bytes |
| Idle | 0 ms CPU over 30 s, 17.6 MB working set, 4.3 MB private bytes, 12 threads |

## Known limitations

- Google Chrome exposes page fields to UI Automation only after an MSAA client asks; LemonVox
  makes one MSAA focus query for Chromium windows, so the first dictation into a Chrome window can
  take up to 300 ms longer. A field Chrome still does not expose falls back to Copy.
- Higher-integrity (administrator) windows are not supported. Windows (UIPI) does not deliver their
  keystrokes to LemonVox's keyboard hook, so a modifier double tap such as Alt twice does nothing there
  (confirmed by the owner in an elevated terminal). When such a window becomes active, the bar briefly
  says so. A regular shortcut (`RegisterHotKey`) or tray > Start dictation still works, and the result
  is offered for Copy because input cannot be sent to elevated windows. Comparable dictation apps that
  run at normal integrity have the same limit. Running LemonVox as administrator, or a signed
  `uiAccess` build installed under Program Files, would lift it; both were rejected for now because
  they widen LemonVox's privileges, and running it elevated is unsupported.
- A lone Alt tap puts classic Win32 apps into menu mode; a double tap enters and leaves it, so the
  target is unchanged, but a menu bar may flash. Choose another trigger if that is distracting.
- If Windows silently removes the keyboard hook (`LowLevelHooksTimeout`) a health check reinstalls it
  within about 2 s; this path could not be forced in tests.
- Only Windows 11 x64 has been tested. Dark mode was verified visually but is not covered by automated
  tests on this light-mode machine. The Settings window does not scroll on very short screens.

## Native UI decision

The UI uses plain Win32 (user32, GDI/GDI+, shell32, dwmapi, uxtheme, comctl32 v6) instead of
the C++/WinRT + WinUI 3 stack originally proposed:

- **Why Win32:** the Windows App SDK is not installed on the build machines and adding a
  framework dependency (plus its runtime installer) was not approved. Win32 ships in-box, keeps
  the binary self-contained, and gives direct control over the activation rules the dictation bar
  depends on (`WS_EX_NOACTIVATE`, `MA_NOACTIVATE`, layered per-pixel alpha).
- **What WinUI 3 would add:** Fluent controls and Mica/Acrylic backdrops, built-in UI Automation
  for every control, XAML layout and theming, and easier high-quality typography.
- **Revisit when:** the Windows App SDK runtime can be shipped (approved dependency and installer
  story), the Settings window outgrows comctl32 controls, or accessibility audits need richer UIA
  than the current window-title/MSAA level. The `ui` module's interfaces (`IndicatorWindow`,
  `TrayIcon`, `Theme`) are view-level so a WinUI implementation could replace them without touching
  adapters or the core.

The indicator is custom-drawn and cannot take keyboard focus by design; every action it offers
(Stop, Cancel, Copy) is also available from the tray menu. Its window title is kept equal to the
current status for Narrator, and error/copy states raise `EVENT_SYSTEM_ALERT`. Left-clicking the
tray icon opens Settings; dictation commands are in the right-click menu.

## Layout

| Module | Path | Purpose |
| --- | --- | --- |
| common | `src/common` | UI-thread executor, UTF-8/UTF-16 conversion, trigger binding |
| prefs | `src/prefs` | Registry-backed preferences (`HKCU\Software\LemonVox`) |
| net | `src/net` | Endpoint facts, WinHTTP `ByteTransport` (no proxy/redirects/cookies/credentials), model catalog |
| capture | `src/capture` | WASAPI capture adapter, 16 kHz WAV conversion, device list |
| insertion | `src/insertion` | UIA/Win32 target inspection, integrity check, clipboard transaction, paste and Copy fallback |
| trigger | `src/trigger` | Low-level keyboard hook double tap via the shared detector, `RegisterHotKey`, shortcut recorder |
| ui | `src/ui` | Theme, lemon icon, indicator bar, tray icon, Settings window, demo host; `res/` holds the app manifest and icon resource |
| app | `src/app` | `DictationController` (binds `WorkflowCore` to the adapters, user messages, diagnostics), `FileCapture`, `LemonVox.exe` composition root |

GUI executables call `lemonvox_win_app_resources(<target>)` (defined in `src/ui/CMakeLists.txt`)
to embed the manifest (PerMonitorV2 DPI awareness, common controls v6, `asInvoker`) and the
build-time-rendered app icon (resource ID 1); `lemonvox_win_manifest(<target>)` embeds only the
manifest.
