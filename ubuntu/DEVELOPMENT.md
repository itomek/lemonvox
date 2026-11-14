# Ubuntu development

Run commands from the repository root. Build dependencies below are only needed
when compiling from source; APT installs runtime dependencies for package users.

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


## Packaging

See [package and repository tooling](packaging/README.md).

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
These checks exercise a private test desktop, not the logged-in session.

Run inside a usable Wayland desktop environment. Use
`python3 ubuntu/probe-capabilities.py` to inventory interfaces without
requesting capture/input consent. Unknown means inspection failed, not absent.

