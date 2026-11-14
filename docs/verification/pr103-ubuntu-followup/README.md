# PR 103 Ubuntu follow-up — 2026-11-14

Started from `fca3bdc0040388510f117fb58529c38da5280619`, including the concurrent Windows fix. The final PR comment records the exact combined tested commit. This report supplements the earlier Ubuntu report; it does not convert missing acceptance evidence into a pass.

Environment: Ubuntu 24.04.5 LTS, aarch64, GNOME Shell 46.0, Wayland; GTK 4.14.5, libadwaita 1.5.0. Logged-in desktop scaling-factor is automatic (0), text scale 1.0. Native automation uses disposable headless GNOME compositors, private session buses and XDG directories. No extension installation/restart or preference changes on the logged-in desktop. Audio is `audiotestsrc` or a uniquely named null-sink monitor; HTTP responses are loopback fixtures. No real microphone, inference, or model download is required.

## Changes and evidence

- Fixed accessible validation: invalid fields expose invalid state and a description relation pointing to their inline error, announce the error, and clear state/relation on recovery. AdwEntryRow's editable delegate receives the same state. GTK 4.14's broken error-message target bridge is avoided.
- [Autosave/session evidence](settings-autosave.md): all categories during synthetic recording; original versus next-session endpoint/models/prompt/thinking payloads; rapid text close-flush; invalid-draft independence; active shortcut refusal and idle recovery; real filesystem write failure, saved-value preservation, binding rollback and retry.
- [GNOME logs](gnome/native-session.log): current extension acknowledgement, conflicts in either binding, preserved working delivery, enabled older-interface simulation, disabled-interface failure and re-enable recovery. Existing focus, cancellation, result insertion and teardown regressions remain covered.
- [Modifier probes](gnome/): focused client receives modifier presses/releases, Shell stage receives none. Bare Alt accelerator fires on press and also for Alt+a; release produces no activation. `modifiers-accelerator-activated` is a keyboard-layout-switch hook, not arbitrary modifier release delivery. See the [extension capability explanation](../../../ubuntu/extension/README.md).

## Reproduction

Supply Node on PATH to include the acknowledgement adapter regression. This host reused Node 22.16.0 and pactl from private `/tmp` tools, without installing packages.

```sh
cmake -S ubuntu -B .build/ubuntu -DCMAKE_BUILD_TYPE=Debug
cmake --build .build/ubuntu --parallel 4
ctest --test-dir .build/ubuntu --output-on-failure
bash scripts/test-core.sh --output /tmp/NEW-core-results
node ubuntu/tests/keybinding_ack_test.mjs
LIBGL_ALWAYS_SOFTWARE=1 KEEP_LEMONVOX_GNOME_TEST=1 bash ubuntu/tests/extension_native_shell_test.sh
bash ubuntu/tests/settings_session_test.sh .build/ubuntu/lemonvox_settings_session_test
dbus-run-session -- python3 ubuntu/tests/service_test.py /tmp/EXTRACTED/usr/bin/lemonvox
bash ubuntu/tests/settings_native_accessibility_test.sh /tmp/EXTRACTED/usr/bin/lemonvox .build/ubuntu/lemonvox_settings_ui_test /tmp/NEW-ui-evidence
bash scripts/build-ubuntu.sh --output /tmp/NEW-package --version 0.7.0 --build-dir /tmp/NEW-release-build
```

The `.deb` is extracted with `dpkg-deb -x`, never installed over the user's app. Package UI tests and component tests have separate evidence boundaries; the latter uses a fixture binding callback and is not evidence of actual compositor registration.

## Results and acceptance boundaries

- **#96 PASS for the exercised Ubuntu synthetic acceptance:** packaged close/reopen and full-process persistence; every category during native synthetic recording; rapid typing followed by immediate real GTK focus leave and close; independent valid edits beside invalid drafts; actual filesystem persistence failure and retry; compositor conflict/acknowledgement/older-interface/recovery checks; AT-SPI names, invalid state and navigable error association; native keyboard operation; both monitor-supported scales (100% and 200%). Storage rollback uses the production coordinator with a binding fixture; the extension's real compositor rollback/delivery is tested separately. These are explicit test boundaries, not a claim of one combined end-to-end storage-failure/compositor run.
- **#97 PASS for Ubuntu:** fresh missing-file, legacy and custom packaged-process relaunch; exact model/prompt preservation; synthetic thinking-off/server-default/generic payload regressions; final Advanced and Models captures.
- **#98 BLOCKED:** the input-delivery limitation and all remaining modifier acceptance gaps are listed below. No production modifier capability is exposed.

Final CTest: **12/12, zero failures**. Portable core: all seven executables passed. Native GNOME extension regression passed. Native Settings/session regression passed, including immediate blur and close flush. Packaged service regression passed. Packaged AT-SPI acceptance and **20/20** native GTK captures passed. Retained logs contain commands/results; `native-settings.log` records actual compositor and GTK widget scales.

The first scale assertion correctly rejected the environment-only `GDK_SCALE=2` setup: Wayland still supplied scale 1. The final launcher temporarily changes only its sole private MetaVirtualMonitor through Mutter DisplayConfig and asserts both compositor and actual widget scale. Earlier scale-labelled captures were replaced with the verified captures.

Remaining verification outside the exercised scope: spoken Orca output, every control's complete keyboard route, fractional compositor scaling (not advertised by this private monitor), other monitor resolutions, and a single integrated packaged-app filesystem-failure plus real-compositor rollback run. The applicable components are covered separately; these broader checks are not claimed. No live microphone/inference or model download was requested or performed. Other operating systems retain their own acceptance requirements; these Ubuntu verdicts do not close the cross-platform issues.

## Standalone modifiers: BLOCKED (#98)

The existing GNOME 46 extension has no reliable ordinary focused-client release/intervening-key stream to feed `lv_single_tap_event`. Implementing from bare activation alone would falsely trigger on modifier chords. No unsafe gesture is exposed, and defaults/cancellation remain unchanged. This is a blocker for this adapter/version, not a claim that every future Mutter/native integration is impossible.

Remaining #98 acceptance: persisted modifier/side choices and actual standalone start/finish exactly once; native chord/pre-held/intervening/repeat/hold rejection; modifier-side selection; focus and Settings recording transitions; modifier conflicts/recovery; modifier relaunch persistence. Portable detector passes do not establish native delivery. Keep issue #98 and PR #103 open.
