# Ubuntu Wayland Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver the Ubuntu 24.04+ GNOME Wayland dictation workflow with remote synthetic-audio tests and installable packaging.

**Architecture:** Reuse WorkflowCore with GLib-serialized native adapters. GTK4/libadwaita settings and a GNOME Shell extension provide native controls and supported global combinations on GNOME 46, which lacks GlobalShortcuts. Automatic insertion is unavailable until secure destination inspection is proven; explicit Copy is the supported result path.

**Tech Stack:** C++17/C17, GTK4/libadwaita, GLib/GIO, libsoup3, GStreamer, GNOME Shell ES modules.

**Spec:** ubuntu/IMPLEMENTATION.md and docs/spec.md; GitHub #11/#16/#26/#27/#28/#18/#19/#54/#20.

## Global Constraints

- Ubuntu 24.04+ GNOME Wayland; no X11, Debian or privileged input-device access.
- Existing C++ core owns workflow/protocol/cancellation; preserve macOS/Windows.
- Stop and repeat finish; X and active-only configurable cancel discard; idle input remains ordinary input.
- Audio/transcripts remain transient, excluded from logs. Settings snapshots apply to next operation.
- Never auto-submit or guess secure-field safety. Explicit Copy remains available.
- No public package publication or credential changes during development.

## Review Focus

- Late callbacks after shutdown must not touch destroyed UI/core.
- Missing model/device/server and empty capture must recover to idle.
- Shortcut conflicts and extension disable must remove bindings/indicator.
- Settings changes during recording must preserve the active snapshot.
- Snap confinement and Shell extension integration must be tested as installed, not assumed from an unpackaged run.

### Task 1: Native adapter foundation (#26/#28/#18, shared #68)
Files: ubuntu/adapters.{h,cpp}, ubuntu/CMakeLists.txt, ubuntu/tests/adapters_test.cpp.
Interfaces: CaptureAdapter, ByteTransport, InsertionAdapter from core/adapters.h; WorkflowConfig normalization.
- [x] Write failing tests for endpoint loopback normalization, fake-server HTTP and cancellation, transient bounded WAV capture and explicit Copy fallback.
- [x] Implement libsoup asynchronous transport, GStreamer capture, and conservative insertion adapter; all callbacks serialized on GLib context.
- [x] Compile with warnings as errors and run ctest plus portable core tests. Verify teardown and no callback after shutdown.

### Task 2: GNOME integration (#16/#27)
Files: ubuntu/extension/*, ubuntu/tests/extension*.
Interfaces: session bus com.itomek.LemonVox, /com/itomek/LemonVox, interface com.itomek.LemonVox; Toggle/Stop/Cancel/ShowSettings methods; State(s), Message(s), Result(s) readable properties; PropertiesChanged signals.
- [x] Test lifecycle and command routing with synthetic backend.
- [x] Implement configurable Ctrl+Alt+Space toggle and active-only Ctrl+Super+Escape cancellation; document Alt twice unavailable. Native Shell nonactivating bar exposes Stop/X/Copy; panel menu retains access without tray extension.
- [x] Verify enable/disable/rebind, idle cancellation passthrough and actual GNOME 46 behavior without installing privileged hooks.

### Task 3: Application and Settings (#19/#54)
Files: ubuntu/main.cpp, ubuntu/settings.{h,cpp}, ubuntu/tests/application*.
Interfaces: adapters from Task 1; D-Bus contract from Task 2. GtkApplication application ID com.itomek.LemonVox.
- [x] Add tests for persisted configuration, immutable active snapshot, model discovery, finish/cancel/error recovery.
- [x] Implement native General/Models/Diagnostics, Auto/Light/Dark, version/build, configurable distinct shortcuts, content-free bounded logs and Copy fallback.
- [x] Verify settings and bar using disposable desktop tests and screenshots; main context owns all core calls.

### Task 4: Audio end-to-end and packaging (#20)
Files: ubuntu/tests/audio-loopback*, ubuntu/packaging/*, scripts/build-ubuntu.sh, .github/workflows/ubuntu.yml, .vscode/{tasks,launch}.json, ubuntu/README.md.
- [x] Generate known synthetic audio; play into isolated virtual source and capture production adapter; verify format/levels, cancellation and cleanup. Mock HTTP failure cases; separately verify actual Lemonade ASR when available.
- [x] Build installable package and desktop/session activation files; install/launch/upgrade/remove with disposable XDG settings. Prepare Snap metadata and separate Store submission issue, report confinement/integration evidence.
- [ ] Add Ubuntu hosted CI; run full Linux gates and independent final review; deliver PR and update issue evidence accurately. Do not close acceptance criteria lacking actual native proof.

## Progress

- Baseline: portable core tests passed on Ubuntu 24.04.5; live portal inspection confirms no GlobalShortcuts, PipeWire reachable.
- Owner approved proceeding to completion and explicitly removed X11.

- Native adapter and host implementation compile; preference/log tests and real audio/ASR workflow pass. Native service tests cover Stop, cancel/late reply, error recovery and running Auto appearance updates.
- Default cancellation changed to Ctrl+Super+Escape after read-only inspection found Ubuntu reserves Ctrl+Alt+Escape for cycle-panels; OS bindings unchanged.
- Native desktop proof, package lifecycle and independent review passed; PR #74 tracks final hosted CI and integration.

- Final independent product review PASS after fixing transport and queued-WAV shutdown retention, with no-drain lifetime regressions and ASAN/UBSan.
- Native GNOME 46 headless Wayland proof passes with editable-field focus, actual Copy/Ctrl+V, no submit, active/idle/rebound cancel and disable cleanup. Software rendering uses condition-based keyboard readiness.
- Actual apt install 0.0.0 → upgrade 0.2.0 → removal passed; disposable XDG settings hash remained unchanged. Clean-source A/B packages are byte-identical; reinstall/activation and the native Chrome/Electron matrix passed. PR #74 remains the final CI/integration gate.
