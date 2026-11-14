# LemonVox GNOME Shell extension

This extension is the Ubuntu 24.04+ GNOME Wayland integration for LemonVox.
It is tested against GNOME Shell 46.0 and deliberately declares only Shell 46
in `metadata.json`; a newer GNOME major version needs its own compatibility
test and metadata update.

It uses GNOME Shell's compositor keybinding API, not X11, input-device access,
or the GlobalShortcuts portal. GNOME 46 on Ubuntu 24.04 does not advertise that
portal. The default toggle is `Ctrl+Alt+Space`; Alt twice is unavailable because
the Shell API registers accelerators rather than exposing modifier-only global
events. Single-tap modifiers are also unavailable in this adapter. Settings
explicitly explains this restriction and accepts only key combinations. No
modifier-only option is advertised as working; Ubuntu acceptance for #98 remains
incomplete until a compositor adapter delivers standalone press/release events
and native input-delivery tests verify suppression of chords, holds and repeats. The default cancellation chord is `Ctrl+Super+Escape`, because Ubuntu
reserves `Ctrl+Alt+Escape` for panel cycling. The cancellation key is registered
only for a non-idle operation, so the configured chord is ordinary input while
LemonVox is idle.

Settings saves valid fields automatically. Shortcut changes use the extension's
`com.itomek.LemonVox.Shortcuts.SetShortcuts` acknowledgement interface on
`org.gnome.Shell` at `/com/itomek/LemonVox/Shortcuts`. Both bindings must register
before the application persists them; cancellation is then released while idle.
A write failure requests restoration of the previous pair. Shortcut changes are
refused during dictation and when this extension interface is unavailable;
other valid settings still save. The disposable GNOME 46 native Shell test verifies
accepted pairs, conflicts in either binding, preservation of working delivery,
enabled older-interface simulation, disabled-extension refusal and re-enable recovery. Application persistence and
older-extension behavior also require the settings integration tests.

The extension exposes a Shell bar during active work, with Stop while recording
and Cancel while active. When processing finishes, it writes the result to the
clipboard and sends Ctrl+V to the application that currently has focus.
It preserves focus and sends no Enter. The destination handles the ordinary
paste, including when no text field is selected. The extension clears the result
after dispatch and suppresses duplicate delivery from property refreshes.

Copy remains available if no application window has focus or the paste shortcut
cannot be dispatched. A result already present when the extension starts remains
available for Copy. Physical modifiers still held when processing finishes delay
the paste for up to five seconds, then leave Copy available.

The application must own this session D-Bus API, preferably through standard
D-Bus activation:

- Bus name: `com.itomek.LemonVox`
- Object: `/com/itomek/LemonVox`
- Interface: `com.itomek.LemonVox`
- Methods: `Toggle`, `Stop`, `Cancel`, `ShowSettings`, `ClearResult`, `Quit`
- Read-only properties: string `State`, `Message`, `Result`, `Palette`, and
  boolean `Error`, with standard `PropertiesChanged` notification.

`State` is one of `idle`, `preparing`, `recording`, `transcribing`, `cleaning`,
or `inserting`. The extension treats every other value as unavailable and does
not enable cancellation. An idle `Error=true` keeps a nonactivating bar visible
with Settings and an explicit X/dismiss, but does not register the cancellation
shortcut. The D-Bus service must be installed with the app; the extension never
starts a binary directly.

The extension schema carries `toggle-shortcut`, `cancel-shortcut`, and
`appearance` (`auto`, `light`, `dark`) for the app Settings store. The backend
also provides a `Palette` property of exactly four CSS-safe semantic colors:
`#rrggbbaa #rrggbbaa #rrggbbaa #rrggbbaa` for window background/foreground and
card background/foreground. The extension rejects every other form and falls
back to the system Shell theme. Valid values apply only semantic colors to the
bar and its buttons; Shell layout, metrics, focus behavior and accessibility
semantics remain native.

## Standalone modifier capability evidence

Run `KEEP_LEMONVOX_GNOME_TEST=1 bash ubuntu/tests/extension_native_shell_test.sh`
from the repository root. This stages the extension in private XDG directories
and starts a headless GNOME Shell 46 Wayland compositor with a disposable GTK
client and synthetic backend. It uses no microphone, inference model, logged-in
Shell, or user preferences. The retained test directory contains the exact
`modifier-*-probe.log` diagnostics and `session.log`.

On Ubuntu 24.04 / GNOME Shell 46.0, the focused external client receives both
press and release for left/right Alt, Control and Shift. The Shell stage's
`captured-event` observer receives no key events for that same focused input.
Bare modifier `grab_accelerator` requests can return action IDs, but that is not
standalone-tap support: the allowed Alt_L accelerator activates on **press**,
produces no release activation, and also activates for Alt+a. Feeding it into
the shared release-only detector would lose the release boundary and intervening
client input, allowing ordinary shortcuts to trigger dictation. The probe also
records inherited display/backend/seat signals and relevant public methods.
The GNOME 46 display has `accelerator-activated`, `overlay-key` and
`modifiers-accelerator-activated`, but no `accelerator-deactivated` signal.
Backend notifications concern keymap/layout and last-device changes; seat
notifications concern device/accessibility state. They do not supply ordinary
client key press/release events. `get_keymap`/`query_state` are snapshots, so
polling them would miss intervening input and cannot enforce the shared policy.
The Shell's reserved Super overlay behavior is not an arbitrary modifier event
stream and is left untouched.

`modifiers-accelerator-activated` is also not a release stream. In the
[Mutter 46.0 implementation](https://github.com/GNOME/mutter/blob/46.0/src/core/keybindings.c#L2080-L2122),
`process_iso_next_group` explicitly ignores key releases. It emits this signal
for configured `ISO_Next_Group` keyboard-layout-switch combinations; the
[combo loader](https://github.com/GNOME/mutter/blob/46.0/src/core/keybindings.c#L438-L458)
resolves that special keysym. The native test connects a listener during the
ordinary six-modifier probe and records zero emissions in
`modifier-special-probe.log`. Reconfiguring the user's layout switching or
reserved overlay is not a general way to obtain all normalized press, release
and intervening input required by the shared detector.

This evidence keeps #98 incomplete on this adapter; it does not establish that
a future Mutter API or native compositor integration could never support it.
