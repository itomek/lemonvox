# Strict Snap evaluation status

This recipe is **not a release candidate**. It uses strict confinement and
`grade: devel` deliberately.

Ubuntu 24.04 GNOME does not advertise the GlobalShortcuts portal. LemonVox
therefore needs a host GNOME Shell extension for its supported global shortcut
and nonactivating dictation bar. A strict Snap cannot install or update that
host extension under the GNOME Shell extension directories, and the Snap recipe
does not claim to do so. The D-Bus activation file is likewise a host package
integration, not a Snap capability: the current Shell extension expects the
host `com.itomek.LemonVox` session service, while the recipe does not declare a
matching Snap D-Bus slot or a supported activation route.

`audio-record` is intentionally requested and is normally a manual connection;
it requires an explicit installed-snap test and consent flow. The recipe also
requests `audio-playback` because Snap documents recording as a companion to
playback. These plugs do not solve host-extension delivery.

Do not work around this with classic confinement. A future distribution change
must first prove a supported host-extension delivery path, the required audio,
network and Wayland interfaces, and installed-snap behavior. Until then, use
the Ubuntu `.deb` artifact from the reproducible package build for the tested
Ubuntu 24.04+ GNOME Wayland path.
