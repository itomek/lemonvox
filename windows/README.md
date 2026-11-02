# LemonVox for Windows

## Overview

The Windows host is a native C++17 tray app built on the shared portable core
(`core/`). Windows-specific code lives in `windows/src/<module>`; each module has
its own `CMakeLists.txt` and is picked up automatically by `windows/CMakeLists.txt`.
Presentation (`ui`) is kept separate from OS adapters, and every native completion
reaches the core through the single serialized `UiThreadExecutor` (`common/executor.h`).

## Requirements

- Windows 10 or 11, x64.
- Visual Studio 2022 Build Tools with the "Desktop development with C++" workload
  (MSVC x64 toolset). CMake and Ninja are used from that installation.
- Windows SDK 10.0.26100.
- No other SDKs or runtimes: the host uses only in-box Win32 libraries.

## Build & test

```powershell
# Configure, build (Release) and run the non-interactive tests.
powershell -NoProfile -ExecutionPolicy Bypass -File windows\scripts\build.ps1 -Output $env:TEMP\lv-build

# Also run tests labeled "interactive" (they show windows and inject input; needs an unlocked desktop).
powershell -NoProfile -ExecutionPolicy Bypass -File windows\scripts\build.ps1 -Output $env:TEMP\lv-build -Interactive
```

Options: `-Configuration Debug|Release`, `-SkipTests`. Builds use `/W4 /WX /permissive-`.
Interactive tests hold `lemonvox::win::test::InteractiveLock` (`tests/support/interactive_lock.h`)
and fail instead of injecting input when their own window is not foreground.

`lemonvox_ui_demo.exe` (in `<output>\src\ui`) shows the tray icon and indicator cycling through
every state; `--smoke` runs the cycle once and verifies the indicator never became foreground,
active or focused (ctest `ui_demo_smoke`).

Tested on Windows 11 Enterprise 26H2 (build 26300.9711), x64, with VS 2022 Build Tools and
Windows SDK 10.0.26100.

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
| prefs | `src/prefs` | Persistent preferences |
| net | `src/net` | HTTP transport adapter |
| capture | `src/capture` | Microphone capture adapter |
| insertion | `src/insertion` | Text insertion and clipboard fallback |
| trigger | `src/trigger` | Global shortcut / push-to-talk |
| ui | `src/ui` | Theme, lemon icon, indicator bar, tray icon, demo host; `res/` holds the app manifest and icon resource |
| app | `src/app` | Composition root: wires adapters, core and UI |

GUI executables call `lemonvox_win_app_resources(<target>)` (defined in `src/ui/CMakeLists.txt`)
to embed the manifest (PerMonitorV2 DPI awareness, common controls v6, `asInvoker`) and the
build-time-rendered app icon (resource ID 1); `lemonvox_win_manifest(<target>)` embeds only the
manifest.
