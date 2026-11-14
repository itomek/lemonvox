# Ubuntu Advanced, accessibility and display acceptance

Baseline: PR #103 `fca3bdc0040388510f117fb58529c38da5280619`, with follow-up source changes recorded by the final verification README. Ubuntu 24.04 arm64; native GTK 4.14.5 / libadwaita; disposable headless GNOME Wayland compositor, software rendering, 1920×1600 virtual monitor. No physical microphone, inference, model downloads, package installation, host shortcut registration or host display preference changes.

## Reproducible commands

```sh
cmake --build .build/ubuntu --target lemonvox lemonvox_settings_ui_test --parallel 2
bash ubuntu/tests/settings_native_accessibility_test.sh \
  .build/ubuntu/lemonvox .build/ubuntu/lemonvox_settings_ui_test \
  docs/verification/pr103-ubuntu-followup
```

The launcher copies the selected binary byte-for-byte into its private executable directory and records both SHA-256 values. To test a package, substitute the extracted package `/usr/bin/lemonvox` for the first argument. The launcher creates private runtime, config, data, cache, state and D-Bus environments. Captures use the real SettingsWindow GTK driver with synthetic health/model data, not the packaged application. AT-SPI acceptance operates the actual application binary, with no test control hooks added to production.

`settings_atspi_test.py` serves a synthetic loopback inventory and accepts only GET `/v1/models`, `/v1/models?show_all=true`, `/v1/health`. Any POST or other request fails the final whitelist. The exact missing-file fresh case uses an audited test-only LD_PRELOAD `connect` shim: only the tested process's IPv4 `127.0.0.1:13305` connects are redirected to the fixture's ephemeral port. This leaves the production binary/default endpoint and machine networking unchanged. No shim is installed globally.

## Native results

The native AT-SPI run passed:

- Named editable API endpoint and Cleanup prompt exposure.
- Invalid endpoint retains the saved endpoint; an unrelated multiline prompt still saves. The visible validation message is linked from the actual focused GtkText via AT-SPI `described-by`, with its exact label target and `INVALID_ENTRY`. Both clear after valid recovery.
- Native virtual keyboard Tab reaches Advanced's API/thinking selector and General's Theme selector. Return/Home/Down/Return changes their native combo selections; disk values verify immediate autosave.
- Debug traces exposes the correct accessible name and autosaves when its AT-SPI action is invoked.
- Close/reopen preserves the multiline prompt and endpoint. Full process relaunch preserves endpoint, both exact synthetic model IDs, multiline prompt, dark Theme and explicit Lemonade server-default thinking, with actual selected UI labels checked.
- Exact fresh missing-file UI initially selects Lemonade thinking off, and initialization creates no settings file. An explicit server-default override saves and survives a full process relaunch.
- A legacy marker-free file exposes compatible-server default behavior; its endpoint/models/custom prompt and exact file bytes remain unchanged after opening and another full process relaunch.
- Only the allowed synthetic inventory/health GETs occurred; no inference/model-download requests.

The accessibility defect was reproduced against the initial extracted package: the endpoint's AT-SPI relations had only `labelled-by`; the regression's error association assertion failed. The fix associates the validation label with the row and actual editable delegate, sets invalid state, clears association/state after recovery, and requests a medium-priority GTK accessibility announcement. GTK 4.14's AT-SPI backend mishandles `ERROR_MESSAGE` targets, so the final fix uses the working `DESCRIBED_BY` relation. A native regression verifies the target, rather than merely checking relation presence.

## Captures and scope

Twenty real GTK captures (including four invalid-endpoint states) cover light/dark × General/Models/Advanced/Diagnostics × subprocess GDK_SCALE=1/2. The private monitor reports supported scales `[1.0, 2.0]` in retained `display-config.txt`; each driver run asserts its actual GTK widget scale equals GDK_SCALE and logs page/theme/logical size/actual scale. Filenames encode the requested scale; these are logical 680×680 GTK render textures, not compositor screenshots at doubled pixel resolution. The Advanced and Models captures show the final layout. Advanced's closed combo can ellipsize its value at this width; native selection exposes the full option labels via the popup and accessibility tree.

This establishes native keyboard operation of the two exercised combo controls and AT-SPI names/error association. It does **not** establish spoken Orca output, every control's complete keyboard route, fractional compositor scaling, arbitrary resolutions, real-server quality or physical microphone behavior. Those boundaries must remain explicit.

## Isolation audit

An early exploratory fresh-profile probe reached the already running default endpoint for inventory GETs before the synthetic fixture was enforced. It performed no audio/inference/download action. Final acceptance uses only the synthetic endpoint and the fresh-case redirect.

An early launcher revision omitted XDG_STATE_HOME; the final launcher isolates it. Read-only `stat` of the host `/home/tomas/.local/state/lemonvox/events.log` showed size 207 and modification/change time 2026-11-12 21:38:05, predating this Nov14 session, so the probe did not write that host diagnostic log. No host files were deleted or restored.
