# Ubuntu implementation evidence

Environment: Ubuntu 24.04.5, GNOME Shell 46.0, Wayland, GTK 4.14.5,
libadwaita 1.5.0, GLib 2.80.0, GStreamer 1.24.2, libsoup 3.4.4,
PipeWire 1.0.5, GCC 13.3.0. Lemonade 2026.45.3 at
`http://127.0.0.1:13305/v1`.

The live portal inventory advertises RemoteDesktop, Clipboard and InputCapture,
but not GlobalShortcuts. No desktop permissions or security settings were
changed. X11 is outside scope; #17 was retired as not planned.

## Verified during implementation

- Portable C++ core session, trigger, protocol, contract and workflow gate passed.
- Native Ubuntu CMake build uses warnings as errors; CTest passed preference,
  diagnostics, adapter, production capture/HTTP/shared-workflow and package
  input checks. Local socket tests need execution outside the tool sandbox;
  EPERM inside that sandbox was not treated as an application defect.
- Real synthetic voice test: generated “Send Alex the fourteen invoices on
  Friday.” with espeak-ng, played into a unique virtual sink, captured only
  that monitor through the production GStreamer adapter, and ran the shared
  core against Whisper-Base and Gemma-4-E2B-it-GGUF through local Lemonade.
  The actual ASR response and returned Copy result independently preserved
  the name, number and day. The driver
  prints no transcript. Virtual devices and generated audio were removed.
- Native GtkApplication service on a private session bus with fake HTTP,
  Settings-only portal fixture and exact virtual audio input passed start,
  Stop, result/ClearResult, recording cancel, processing cancel with late HTTP
  reply, empty-transcript recovery, restart, Quit, visible error flags, and a running Auto light→dark→light
  native semantic palette update. No WAV files remained
  and no dictated text entered the diagnostic log. The fake portal only
  prevents unrelated desktop-service activation in this isolated test; it is
  not evidence of real portal keyboard or insertion support.
- Settings rendered directly through GTK on the Wayland session in Light and
  Dark; Models layout was visually inspected. Images contain only default
  configuration and synthetic test values. No desktop screenshot was taken.

## Native integration and delivery gates

The GNOME 46 native keyboard/focus/Copy proof passed under normal and software
rendering, including editable-field focus and manual paste with no submit.
Chrome 157 and Electron 40.10.2 were verified as actual Wayland clients in a
separate private-compositor matrix. See `ubuntu/tests/gnome-report.md`.
Actual apt installation, upgrade to 0.2.0, D-Bus activation and removal passed
with disposable settings retained. Independent product review passed after
transport and queued-WAV lifetime fixes and focused ASAN/UBSan verification.
Clean-source package reproducibility and public CI/merge remain delivery gates.
Public Snap publication is tracked in #73 and requires confinement/extension
integration proof plus publisher access and Store review. Physical microphone
and keyboard hardware are not required for the remote tests and have not been
claimed as tested.
