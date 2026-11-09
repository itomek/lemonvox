# LemonVox GNOME Shell extension

This extension is the Ubuntu 24.04+ GNOME Wayland integration for LemonVox.
It is tested against GNOME Shell 46.0 and deliberately declares only Shell 46
in `metadata.json`; a newer GNOME major version needs its own compatibility
test and metadata update.

It uses GNOME Shell's compositor keybinding API, not X11, input-device access,
or the GlobalShortcuts portal. GNOME 46 on Ubuntu 24.04 does not advertise that
portal. The default toggle is `Ctrl+Alt+Space`; Alt twice is unavailable because
the Shell API registers accelerators rather than exposing modifier-only global
events. The default cancellation chord is `Ctrl+Super+Escape`, because Ubuntu
reserves `Ctrl+Alt+Escape` for panel cycling. The cancellation key is registered
only for a non-idle operation, so the configured chord is ordinary input while
LemonVox is idle.

The extension exposes a Shell bar during active work, with Stop while recording
and Cancel while active. It keeps the active application focused. A result can
only be copied after an explicit click; it is never rendered in the Shell and
the extension calls `ClearResult` after a successful clipboard write. It never
attempts automatic cross-application text insertion.

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
