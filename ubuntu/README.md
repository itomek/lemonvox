# LemonVox on Ubuntu

Target: Ubuntu 24.04 and newer, GNOME Wayland. The initial tested environment is
Ubuntu 24.04.5 / GNOME Shell 46. X11 and other distributions are outside scope.
The extension declares only GNOME versions with native verification; a later
Ubuntu release needs its Shell version verified before support is claimed.

The same C++ core used by Windows and macOS owns model validation,
transcription, cleanup, immutable session settings and cancellation. Ubuntu
uses GTK4/libadwaita Settings, GLib/GIO, libsoup3, GStreamer and a GNOME Shell
extension. GNOME 46 lacks GlobalShortcuts, so a portal-only application cannot
provide the requested global controls on Ubuntu 24.04.

## Build and run

```sh
sudo apt install build-essential cmake pkg-config libgtk-4-dev libadwaita-1-dev \
  libsoup-3.0-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-good gstreamer1.0-pulseaudio pipewire-pulse gjs
cmake -S ubuntu -B .build/ubuntu -DCMAKE_BUILD_TYPE=Debug
cmake --build .build/ubuntu --parallel
ctest --test-dir .build/ubuntu --output-on-failure
.build/ubuntu/lemonvox --settings
```

Install and run [Lemonade](https://lemonade-server.ai/) or another
OpenAI-compatible server separately on this computer before using LemonVox.
The server must provide audio transcription, chat completions for cleanup and
model-discovery metadata supported by LemonVox. Generic OpenAI API compatibility
alone does not establish compatibility: model discovery currently requires
capability/download/recipe metadata understood by LemonVox. Lemonade 2026.44.1
is the tested baseline; other servers have not been verified.

Settings defaults to `http://127.0.0.1:13305/v1`; select an available transcription
model and an independent cleanup model after Refresh models. Local and remote HTTP/HTTPS
endpoints ending in /v1, without embedded credentials, queries or fragments, are accepted. Settings
loads asynchronously and remains usable if the server is unavailable.

A background check requests the saved endpoint's `/models` API every ten seconds,
with a three-second timeout, even while Settings is closed. Each Settings page
shows neutral checking, green API-connected, or red API-unavailable status; the
separate-server requirement appears only when unavailable. This checks API
reachability and inventory format, not model readiness or server identity.
Unsaved endpoint edits do not change the health check until Save.

The Settings window provides Start, Stop, Cancel and Copy even without the
extension. Settings changes apply after Save and to the next operation.
Auto follows the OS; Light and Dark apply native libadwaita appearance.

## Install the local Ubuntu package

```sh
bash scripts/build-ubuntu.sh --output /absolute/new/package-directory --version 0.2.0
sudo apt install /absolute/new/package-directory/lemonvox_0.2.0_amd64.deb
```

Use the architecture printed by the build. The package includes the desktop
entry, session D-Bus activation service and GNOME Shell extension. It does not
change desktop permissions or enable extensions automatically. After a first
installation, log out and back in if GNOME has not discovered the extension,
then enable it explicitly:

```sh
gnome-extensions enable lemonvox@itomek.com
```

Open LemonVox from the application launcher. The extension adds a panel menu
and a nonactivating recording bar. Default dictation is **Ctrl+Alt+Space**;
press it again or click **Stop** to finish. **Ctrl+Super+Escape** or **X** cancels
and discards. The cancellation binding is absent while idle. Shortcuts are
configurable and must be distinct regular key combinations; Alt twice is
unavailable on this integration. If a shortcut cannot be claimed, use the
visible controls and choose another combination.

When processing finishes, the GNOME extension sends Ctrl+V to whichever
application currently has focus. Moving the cursor or switching applications
during processing changes the paste destination. A recognized text field is
not required. Paste replaces the clipboard with the transcript and never sends
Enter. Copy remains available when the paste gesture cannot be dispatched.
The extension waits briefly for held shortcut modifiers to be released.

Upgrade with `sudo apt install ./new-package.deb`. Remove with
`sudo apt remove lemonvox`; local per-user settings remain in
`~/.config/lemonvox/settings.ini`. The extension must be disabled before
removal if GNOME is still running it. The app does not install or manage the separate server or remove model downloads.

Settings are private per-user files. Logs rotate at 256 KiB with one backup in
`~/.local/state/lemonvox/events.log`; default logging records errors/fallback
only. Debug adds fixed numeric events, never audio, transcripts, prompt text,
URLs, clipboard contents, credentials or provider responses. Audio stays in
memory and capture is bounded to 120 seconds. Choose the desired input device
in Ubuntu Sound settings. `LEMONVOX_AUDIO_DEVICE` can explicitly select a
PulseAudio/PipeWire source before service launch, chiefly for isolated tests.

## Remote tests

Hosted checks and CTest use synthetic sources and a fake local HTTP server;
they do not need a microphone or download models. The portable gate remains:

```sh
bash scripts/test-core.sh --output /absolute/new/core-test-directory
```

For real audio/ASR verification, install `espeak-ng pulseaudio-utils`, ensure
Lemonade has the named downloaded models, then run:

```sh
python3 ubuntu/tests/audio-loopback.py \
  --driver .build/ubuntu/lemonvox_workflow_audio_test \
  --asr Whisper-Base --cleanup Gemma-4-E2B-it-GGUF
```

The test checks ASR and cleanup independently for the expected name, number
and day. It generates speech, creates a uniquely named virtual sink, captures only
its exact monitor through the production adapter, plays the phrase into that
sink and checks the real workflow's result for the name, number and day. It
never selects the physical input or changes the default device. Temporary
files and the virtual sink are removed on success or failure. Model/server
errors are failures, not mocked passes.

The opt-in service integration test uses the production capture adapter with
a virtual source and a fake local server:

```sh
dbus-run-session -- python3 ubuntu/tests/service_test.py .build/ubuntu/lemonvox
```

The installed activation service can be tested by adding `--activate` after
`/usr/bin/lemonvox` on that private-bus command.

The native desktop proof and browser/Electron matrix are also reproducible:

```sh
LIBGL_ALWAYS_SOFTWARE=1 bash ubuntu/tests/extension_native_shell_test.sh
LIBGL_ALWAYS_SOFTWARE=1 bash ubuntu/tests/extension_native_matrix_test.sh
```

The matrix needs Chrome and a fixed Electron test runtime; it uses private
profiles and only synthetic fields. It never runs on the logged-in desktop.
See `tests/gnome-report.md` for versions, prerequisites and exact support limits.

Run inside a usable Wayland desktop environment. Use
`python3 ubuntu/probe-capabilities.py` to inventory interfaces without
requesting capture/input consent. Unknown means inspection failed, not absent.

## Distribution and remaining evidence

See `packaging/` for the local `.deb` and a development-only Snap recipe.
[Snap Store delivery #73](https://github.com/itomek/lemonvox/issues/73) tracks
confinement, host extension delivery, publisher ownership and Store review.
The strict Snap recipe is not a Store release candidate. There is no apt
repository or public release from this development build.

Native extension, focus, appearance, package-manager lifecycle and actual
keyboard tests must be recorded separately from source builds and mocked
checks. See `tests/*report.md` and the implementation PR for exact results;
unsupported or unverified behavior must not be reported as a completed
milestone acceptance criterion.

Runtime libraries GTK/libadwaita, GLib/GIO, libsoup and GStreamer are dynamically
linked system packages, generally LGPL; GNOME Shell/GJS provides the extension
runtime. Vendored yyjson is MIT and ships its license. No Qt, X11 input adapter,
privileged keyboard hook, provider SDK or second dictation engine is added.

## Model setup

Installing the inference server, downloading model files, and loading an installed
model into memory are separate steps. Install and start the server separately.
Settings → Refresh models checks its catalog without starting downloads. Choose
transcription and cleanup models independently. Use Set up transcription / Set up
cleanup and confirm the named model before allowing the server to download it;
files may occupy several GB on the server machine. The server owns storage,
network access, disk space and filesystem permissions. An installed but unloaded
model can load at first inference and is not a missing download.

Setup shows an activity state while the server works. Cancel setup requests cancellation on the server and waits until its worker
stops; the server may retain partial or completed files. If cancellation cannot
be confirmed after network loss, check the server model manager and Refresh
models before retrying. Failed setup preserves saved settings. After successful setup the chosen
compatible model is selected in Settings; Save settings applies it to later sessions.
For servers or versions without verified model-management support, open that
server’s model manager, install a compatible transcription or chat model, and
return to Refresh models. LemonVox never constructs shell commands from model IDs.

Model installation requires verified Lemonade 2026.46.2+ management support and
a compatible downloads API probe. See [shared model setup](../docs/model-setup.md)
for the API contract and verification limits.
