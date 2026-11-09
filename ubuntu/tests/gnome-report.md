# GNOME Shell integration evidence

## Integrated CI synchronization regression

After PR #74 passed all checks, its main-branch GNOME rerun failed waiting for
the synthetic backend to become idle. The original log does not identify which
cancellation phase failed. Eight fresh local runs passed. Code review found
that the rebound-cancel and disable-cancel steps waited only for a backend
property read, which can precede delivery to the extension's asynchronous proxy.

Delaying the synthetic service's property notifications by 500 ms reproduced
the same failure with the old test: backend remained recording. Waiting for
the extension snapshot, successful native binding registration and completed
settings rebind made that same delayed test pass. Production extension/app
code is unchanged. CI now exercises the delayed case; failures include the
current interaction phase. This establishes the test synchronization defect,
without claiming that the original failure's exact phase is known.

## Environment and scope

Verified on Ubuntu 24.04.5 with GNOME Shell 46.0 and Mutter 46.2, using a
headless Wayland compositor. The test starts a fresh `dbus-run-session` with
private `XDG_RUNTIME_DIR`, `XDG_DATA_HOME`, `XDG_CONFIG_HOME`, and
`XDG_CACHE_HOME`; it stages the extension under that private data directory.
It neither enables an extension nor changes a shortcut in the logged-in GNOME
session. It uses `--wayland --headless --no-x11`; X11 is not exercised.

Run the proof outside a restrictive sandbox that blocks local D-Bus sockets:

```bash
bash ubuntu/tests/extension_native_shell_test.sh
```

The same proof also passed with `LIBGL_ALWAYS_SOFTWARE=1`, the renderer mode
used by the hosted native gate. Before launching its private clients it unsets
`DISPLAY` and `XAUTHORITY`, sets `WAYLAND_DISPLAY=wayland-0` and
`GDK_BACKEND=wayland`, and starts Shell with `--no-x11`.

The test uses `--unsafe-mode` only on its disposable headless Shell process so
that its private `org.gnome.Shell.Eval` endpoint can inspect native actors and
inject a virtual Clutter keyboard. It never enables unsafe mode on the user's
Shell.

## Verified result

The command passed on 2026-11-08. It validates all of the following in the
actual GNOME 46 extension loader rather than importing source files directly:

- GNOME discovers the staged `lemonvox@itomek.com` extension, enables it,
  exposes an active extension object, disables it, and returns it to inactive.
- A synthetic service implementing the production
  `com.itomek.LemonVox` D-Bus interface connects through name watching and
  standard property changes. No microphone, audio capture, or application
  binary is started by this test.
- `Ctrl+Alt+Space` routes to `Toggle`; the synthetic service reaches
  `recording`, the native Shell bar appears with Stop and X, and focus remains
  on the disposable GTK Wayland fixture. The bar and its controls are
  non-focusable.
- Bare Escape reaches the fixture during recording. `Ctrl+Super+Escape`
  reaches the fixture while idle, is withheld while active, and routes to
  `Cancel`. This proves the cancellation binding is active-only.
- The private extension schema rebinds both shortcuts to
  `Ctrl+Super+F10` and `Ctrl+Super+F11`; the former default toggle no longer
  fires, while the rebound toggle/cancel do.
- A synthetic idle `Error=true` produces a visible nonactivating bar with X
  and Settings, keeps the cancellation binding inactive, and its X dispatches
  `Cancel` to dismiss the error.
- A synthetic result makes the actual bar Copy callback populate the Shell
  clipboard. The test sends only `Ctrl+V` to the still-focused, initially empty
  GTK entry and verifies the exact synthetic text appears there, `ClearResult`
  clears the service result, the bar hides, and the entry is not activated.
- Disabling during a second active operation cancels the service before the
  extension removes its controls. The rebound toggle no longer fires after
  disable.

Ubuntu reserves `Ctrl+Alt+Escape` for panel cycling, so the extension default
is intentionally `Ctrl+Super+Escape`; the host's binding is not altered.

## Browser and Electron field matrix

The following separate private-compositor proof passed on 2026-11-08 with
software rendering:

```bash
LIBGL_ALWAYS_SOFTWARE=1 bash ubuntu/tests/extension_native_matrix_test.sh
```

It used Google Chrome `157.0.8103.38` and a test-only, fixed Electron
`v40.10.2` runtime unpacked under the temporary test root. Electron's test
`BrowserWindow` has `sandbox: true`, `contextIsolation: true`, and
`nodeIntegration: false`; no project dependency or user profile is added.
Each client loads the same local `127.0.0.1` HTML fixture with a titled,
autofocused empty input. The fixture POSTs only synthetic focus, blur, input,
and submit events to its temporary local collector; the proof uses no CDP.

For both actual clients, Shell verifies the matching `Meta.Window` is Wayland,
then activates it and checks that it remains the focused window while the
native non-focusable Copy bar is visible. It clears the private clipboard,
causes a synthetic D-Bus `Result`, clicks the actual Shell Copy control, and
confirms that the page reports no input before the test injects its single
ordinary `Ctrl+V`. That paste produces exactly `Synthetic manual copy text`.
The form records no submit event, so the proof injects neither text keys nor
Enter and does not claim automatic cross-application insertion. The test also
checks `ClearResult` hides the bar after Copy.

Both harnesses lazily detach portal/GVFS mounts only under their exact private
runtime roots before removal; successful runs leave no `lemonvox-extension-*`
test directory in `/tmp`.

## Visual evidence

The headless test captured only its synthetic fixture and Shell chrome:

- `docs/evidence/milestone-2/gnome-shell-recording.png`
  (`sha256:b86254948fa0d712d5d04bf61449eb6fb31e661e12fd1426b4e3b25f581514e8`)
- `docs/evidence/milestone-2/gnome-shell-error.png`
  (`sha256:dba5c646842ef9c454c39ba6b6d24098d6473644c68316df67a6df437d86cd7f`)

They show the native semantic Shell surface in recording and recoverable-error
states. The recording image includes the focused empty synthetic target entry.
They are not captures of the logged-in desktop and contain no dictated content.

## Limits

This is behavioral proof for the GNOME 46 compositor API and virtual keyboard
event path, not a claim of physical-keyboard hardware testing. The extension
declares Shell 46 only; a later GNOME major needs a new compatibility run and
metadata update. It intentionally provides explicit Copy rather than automatic
cross-application insertion. The browser matrix covers Chrome and Electron
versions named above, not every browser or Electron release.
