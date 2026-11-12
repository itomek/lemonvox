> Current behavior: the GNOME extension automatically pastes completed dictation
> once at current focus, with Copy fallback when dispatch is unavailable. Copy-only
> and original safety-policy descriptions below record the initial implementation.
> Install and run Lemonade or another OpenAI-compatible server separately
> on the same computer; audio transcription, chat completions and compatible
> model-discovery metadata are required. Only loopback endpoints are supported.
> Lemonade 2026.44.1 is the tested baseline; generic API compatibility alone is
> insufficient and other servers remain unverified.

# Ubuntu 24+ implementation starting point

## Scope

Milestone 2 on GitHub is the Ubuntu port (#11); this differs from the
numbering in docs/spec.md. Target Ubuntu 24.04 and newer GNOME Wayland.
X11 (#17), Debian and other distributions are outside the owner's requested
scope. No desktop settings, bindings or permissions have been changed.

## Capability evidence — 2026-11-08

This remote machine runs Ubuntu 24.04.5, GNOME Shell 46.0,
xdg-desktop-portal 1.18.4, GNOME backend 46.2, GTK 4.14.5,
libadwaita 1.5 and PipeWire 1.0.5. Session variables identify GNOME Wayland.
Live D-Bus introspection advertises RemoteDesktop, Clipboard and InputCapture,
but no GlobalShortcuts interface. The installed gnome.portal backend manifest
also omits GlobalShortcuts. PipeWire core connectivity passed.

Run `python3 ubuntu/probe-capabilities.py` in the desktop user's session to
repeat the inventory. Restricted execution can prevent session-bus access;
the probe reports unknown in that case, rather than incorrectly reporting
unsupported. It does not capture audio, inspect clipboard contents, register
shortcuts or request consent. Advertised interfaces are not behavioral proof.

`bash scripts/test-core.sh --output /tmp/lemonvox-ubuntu-core-20261108`
passed all session, trigger, contract, protocol and workflow tests.

## Implementation approach

Keep the existing C++17 workflow/protocol engine and use C GTK4/libadwaita
APIs through C++ adapters. Serialize callbacks on the GLib main context.
Preserve Windows/macOS behavior: separate model selections, conservative
cleanup, immutable session settings, Stop/repeat to finish, X/configurable
shortcut to cancel, late-result suppression, recoverable Settings, private
bounded diagnostics, and Auto/Light/Dark native appearance.

A portal-only design cannot supply global shortcuts on this machine.
Evaluate an explicitly installed GNOME Shell integration for Ubuntu 24.04
against consent-based portal integration on newer desktops. A Shell
integration adds GNOME-version maintenance and packaging work, but can be
investigated without privileged input devices or X11. Do not promise Alt
twice, held modifier–Escape, nonactivating positioning or secure-field
inspection until the fixed-text interaction proof passes. Never inject text
when destination safety is unknown; an explicit Copy action is required.
A newer-Ubuntu-only portal app would be smaller but would exclude the
requested 24.04 baseline. An X11 implementation conflicts with requested scope.

## Remote verification plan

Use fake capture/HTTP/insertion adapters first for success, errors, timeouts,
cancel, duplicate finish, stale replies and teardown. Then play generated
speech into an isolated virtual PipeWire sink and capture its monitor via the
production capture adapter. Select that exact virtual source; never record
the default physical microphone. Bound duration, verify nonempty PCM/WAV
format and levels, then run a known phrase through Lemonade and check the
transcript separately from conservative cleanup. A mocked transcription
response is not evidence of speech recognition. Remove virtual devices and
transient files after every run. The isolated playback/capture and actual Lemonade ASR/cleanup test passed; see `docs/evidence/milestone-2/README.md` and `ubuntu/tests/gnome-report.md` for the separate audio and native desktop evidence.

Use disposable fields to verify focus retention, secure-field refusal,
clipboard ownership, manual Copy and no automatic submission. Synthetic
keyboard events alone do not establish physical shortcut delivery. Record
exact desktop and server versions and limitations with each result.

## Delivery order

1. #16/#27: GNOME 46 fixed-text capability proof and shortcut decision.
2. #26/#68: one reusable Linux host, executor and build/CI foundation.
3. #28/#18: bounded capture, transport and remote loopback validation.
4. #19/#54: native Settings and nonactivating controls with feature parity.
5. #20: package, install/upgrade/remove and desktop integration checks.
6. Separate Snap Store submission work described below.

## Snap Store submission follow-up (#73)

Tracked separately in [#73](https://github.com/itomek/lemonvox/issues/73). Milestone 2 retains its original first-installable-build scope; public Store submission is a separate action.

Existing #20 covers packaging/validation but not store submission. Target
Ubuntu 24.04+ GNOME Wayland, initially the tested architecture. Prefer strict
confinement; prove audio, network, Wayland, portal and any Shell-integration
requirements in the installed snap before committing to this route. Classic
confinement requires a separate Store review and is not an assumed workaround.
Use a core24-compatible build and reproducible source/version/checksum mapping.
Document Lemonade as a separate prerequisite. Verify clean install, launch,
settings retention on refresh, cancellation, Copy, removal and permissions.
Prepare reviewable metadata and a candidate artifact; publisher ownership and
Store credentials are external prerequisites. Publication is a separate action.
Do not make Debian support or an apt repository part of acceptance.

References:
- https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.GlobalShortcuts.html
- https://snapcraft.io/docs/explanation/security/snap-confinement/
- https://snapcraft.io/docs/reference/administration/reviewing-classic-confinement-snaps/

## Implementation result

The GTK4/libadwaita host, GStreamer capture, bounded libsoup transport and GNOME Shell extension are implemented. GNOME 46 uses Ctrl+Alt+Space and active-only Ctrl+Super+Escape; Alt twice and automatic insertion remain unavailable. Native GTK, Chrome 157 and Electron 40.10.2 Wayland tests verify focus, explicit Copy/paste and no submission. Software-rendered tests wait for an active client and announced keyboard before injection. Product review and sanitizer checks pass; final clean-source package and PR/CI delivery are recorded separately.
