# Ubuntu PR 104 verification

Tested source: `bed3cdb3fae5c7c45620b2bf6dde6e3204b24fa7`, November 14, 2026.
Ubuntu 24.04.5 LTS x86_64, GNOME Shell 46.0, GTK 4.14.5,
libadwaita 1.5.0. Debug CMake build, app version 0.2.0/build bed3cdb;
these are synthetic fixtures, not a release package or live inference test.

## Commands and results

From the repository root:

```sh
cmake -S ubuntu -B .build/pr104 -DCMAKE_BUILD_TYPE=Debug
cmake --build .build/pr104 -j 4
ctest --test-dir .build/pr104 --output-on-failure
bash scripts/test-core.sh --output /tmp/lemonvox-pr104-core
```

Build succeeded; CTest **11/11 passed** (2.26 seconds); portable core gate
passed all seven reported checks. No product source changes were needed.

Six Settings captures used the production SettingsWindow, synthetic connected
status and model inventory, and no endpoint requests or preference writes.
Xvfb is unavailable on this host, so captures used the native Wayland session
with a private bus without service activation to avoid portal startup delays:

```sh
evidence=/absolute/existing/evidence
bus="$PWD/docs/assets/desktop/ubuntu-pr104-verification/capture-dbus.conf"
for appearance in light dark; do
  for page in General Models Diagnostics; do
    timeout 20s env GDK_BACKEND=wayland GSETTINGS_BACKEND=memory \
      GTK_A11Y=none GTK_USE_PORTAL=0 GIO_USE_PORTALS=0 \
      dbus-run-session --config-file="$bus" -- \
      .build/pr104/lemonvox_settings_ui_test \
      "$evidence/settings-${page,,}-$appearance.png" "$appearance" "$page" connected
  done
done
```

All six invocations exited 0. Forced Light/Dark, 680×680 logical widgets and
680×680 PNGs. GDK_SCALE/GDK_DPI_SCALE unset; Mutter GetCurrentState reported
monitor scale 1.0 and legacy UI scaling factor 1; text-scaling-factor was 1.0.
GTK_A11Y=none means these captures provide no accessibility evidence.
All six images were visually inspected. General/Models require scrolling for
lower content; the cleanup model uses native ellipsization.

```sh
mkdir -p /tmp/lemonvox-pr104-shell
LEMONVOX_TEST_PALETTE='' LEMONVOX_EVIDENCE_DIR=/tmp/lemonvox-pr104-shell \
  LIBGL_ALWAYS_SOFTWARE=1 bash ubuntu/tests/extension_native_shell_test.sh
LIBGL_ALWAYS_SOFTWARE=1 bash ubuntu/tests/extension_native_matrix_test.sh
```

Shell harness exited 0: **PASS: isolated GNOME Shell extension enable/disable
test**. It checks shortcut routing, idle cancellation passthrough, recording
focus preservation, active cancellation, error dismissal, automatic insertion
into a disposable GTK field, shortcut rebinding and teardown. Three inspected
PNGs show recording, processing and recoverable error in the private 1280×800
Wayland compositor, scale 1, native Shell palette, synthetic D-Bus backend.
No microphone, inference service or personal destination field was used.

Browser/Electron matrix exited **77**, `SKIP: required matrix command is
absent: node`. npm and `/usr/bin/google-chrome` are also absent. No matrix pass
is claimed.

## Remaining verification

Packaged app/service and end-to-end audio/inference, System appearance changes
while open, bar Light/Dark transitions, minimum sizes, high/fractional scaling,
full keyboard traversal/screen-reader/contrast/reduced motion, lower Settings
content, and browser/Electron insertion remain unverified. Portable stale-result
checks do not establish native end-to-end late-result suppression. Windows
interactive verification and dependency behavior #53/#96–98 remain open.
This evidence does not close #99 or make PR 104 ready to merge.
