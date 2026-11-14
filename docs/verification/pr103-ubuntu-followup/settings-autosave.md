# Ubuntu settings autosave and session boundary verification

Candidate baseline: `fca3bdc0040388510f117fb58529c38da5280619`, with the follow-up test changes in this checkout. Read issue #96 and PR #103 discussion, including the Windows follow-up and prior Ubuntu gaps. No production changes were needed by this test work.

Commands run (2026-11-14, Ubuntu arm64 / GNOME Shell 46):

```sh
cmake -S ubuntu -B /tmp/lemonvox-pr103-followup-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/lemonvox-pr103-followup-build --target lemonvox_settings_session_test lemonvox_settings_test --parallel 3
bash ubuntu/tests/settings_session_test.sh /tmp/lemonvox-pr103-followup-build/lemonvox_settings_session_test
/tmp/lemonvox-pr103-followup-build/lemonvox_settings_test
git diff --check
```

All final commands passed. Logs are in `logs/settings-session.log`, `logs/settings-session-build.log`, and `logs/settings-persistence.log`. The test runner creates a private D-Bus, XDG profile/runtime and headless GNOME Wayland compositor. Capture uses `audiotestsrc`; two loopback HTTP fixtures supply inventory and responses. No microphone, model downloads, installed packages, or logged-in desktop settings are used.

The new native integration executable constructs production SettingsWindow, SettingsStore, WorkflowCore, GStreamer capture and Soup HTTP adapters. It edits General theme and both shortcut drafts, Models endpoint/ASR/cleanup selections and prompt, Advanced execution, and Diagnostics debug while recording. Invalid endpoint drafts do not block unrelated valid selections. Twenty rapid prompt updates followed immediately by native window close persist the last draft and flush valid endpoint/model/prompt/debug changes. Recording remains active and installation/refresh callbacks remain unused.

The integration callback mirrors main.cpp's policy that shortcut rebinding is rejected while dictation is active. It asserts the original binding remains active, finishes dictation, then edits/retries both shortcuts while idle and asserts the new saved/bound pair. The binding fixture models acknowledgement; actual compositor delivery is covered separately by the native shortcut matrix.

The first ASR/cleanup request pair reaches the original server and retains the original model/prompt snapshot. A second recording reaches the newly saved server and sends the new explicit ASR/cleanup models and last prompt. The original Lemonade thinking-off request contains `enable_thinking:false`; the next server-default request omits `enable_thinking`. Synthetic WAV payload and one completed result per session are checked. The persistence suite separately covers factory defaults, legacy backend migration, explicit models/custom prompt round trips, old default-prompt migration, and private file permissions.

The persistence regression now also removes write permission from its owned temporary directory (0500), causing the actual GLib durable atomic replacement to fail. It confirms the previous INI remains readable with its original prompt and shortcut, and that the acknowledged binding is rolled back. Restoring directory permission (0700) permits retry; reloaded storage and the accepted binding both contain the new pair. Existing fault-injected bind timeout/write-failure tests remain in place. This permission-denial test runs as the ordinary desktop user.

Limits: this integration executable is a native component acceptance harness, not the packaged application process. It does not itself establish package relaunch, AT-SPI keyboard/screen-reader behavior, real inference/microphone behavior, or actual GNOME shortcut delivery. Other follow-up reports cover those checks where available. No parity or issue-completion claim is made. Early harness iterations exposed missing next-model fixture inventory and an overly permissive test binding callback; these test-fixture defects were corrected before the final recorded run.
