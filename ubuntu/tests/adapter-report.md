# Ubuntu adapter foundation report

Status: **PASS**

## Owned artifacts

- `ubuntu/adapters.h`
- `ubuntu/adapters.cpp`
- `ubuntu/CMakeLists.txt`
- `ubuntu/tests/adapters_test.cpp`

The adapter target is `lemonvox_ubuntu_adapters`. It links the existing C++
workflow/protocol core with GLib/GIO, libsoup 3 and GStreamer app APIs.

## Behavior covered

- `endpoint_facts()` applies the shared loopback `/v1` endpoint rules and
  canonicalizes the accepted URL before the core uses it.
- `PipeWireCaptureAdapter` uses `pulsesrc` by default, accepts an explicit
  Pulse/PipeWire source name for a virtual monitor, and accepts `audiotestsrc`
  for deterministic tests. It holds at most the configured duration in RAM
  and returns 16 kHz mono S16LE RIFF/WAVE bytes. It writes no audio files.
- A completed-but-not-yet-dispatched capture stop keeps its WAV and completion
  in adapter State rather than in an idle-lambda capture. `cancel()` and
  `shutdown()` synchronously zero that WAV and clear its completion before a
  stopped GLib context can retain the queued callback.
- `SoupByteTransport` uses a direct proxy resolver, refuses non-loopback URLs,
  redirects, unsafe headers and cached authentication. It reads response data
  in 64 KiB chunks and enforces both `Content-Length` and aggregate response
  limits before exposing bytes to the core.
- `ManualCopyInsertionAdapter` deliberately reports target insertion as
  unavailable; `WorkflowCore` delivers the protected text through `text_ready`
  for the host's explicit Copy action.
- Cancellation and shutdown suppress pending capture, transport and insertion
  callbacks. The adapter test cancels an already-queued failed capture start,
  cancels/shuts down queued real `audiotestsrc` WAV stops, and cancels/shuts
  down already-queued transport validation failures. A live, paused local HTTP
  request and a queued WAV stop both prove that shutdown releases State and a
  completion capture before its GLib context is pumped again.
- Transport State owns active requests; requests hold State weakly. Native
  callback capsules hold weak requests, so shutdown can clear the request map,
  owned response bytes, completion closure, timeout source, message, stream,
  cancellable and session without a State/request ownership cycle or a raw
  request pointer surviving into a late callback.

## Checks run

```sh
cmake -S ubuntu -B /tmp/lemonvox-ubuntu-adapters-build -DBUILD_TESTING=ON
cmake --build /tmp/lemonvox-ubuntu-adapters-build --parallel 2
ctest --test-dir /tmp/lemonvox-ubuntu-adapters-build --output-on-failure

cmake -S ubuntu -B /tmp/lemonvox-ubuntu-adapters-sanitize -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build /tmp/lemonvox-ubuntu-adapters-sanitize --target lemonvox_ubuntu_adapters_test --parallel 2
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  /tmp/lemonvox-ubuntu-adapters-sanitize/lemonvox_ubuntu_adapters_test
```

The full CTest run passed 9/9: adapter endpoint/insertion/loopback/synthetic
audio/teardown contracts; settings; diagnostics; native workflow; packaging;
version; and Shell-extension policy, D-Bus and contract tests. The dedicated
AddressSanitizer/UBSan adapter run passed with leak detection disabled for
GLib/GStreamer process caches.

The adapter test alone uses `-fno-access-control` to observe the private State
weak reference in this lifecycle regression. Production libraries and the app
retain normal C++ access control; no test API is exported.

The loopback-server cases require execution outside the Codex filesystem
sandbox because it prohibits even an isolated local socket bind. No external
network was used.

## Limits to retain in release evidence

- A 100 ms GStreamer state wait catches immediate missing-device/startup
  failures. A later source failure is reported when capture stops; it must not
  be presented as a successful dictation result.
- The packaged runtime must include the GStreamer PulseAudio/PipeWire source,
  conversion, resampling and appsink plugins. Runtime availability is checked
  at capture start.
- This adapter cannot establish Wayland target safety, so automatic insertion
  stays disabled. The host must expose Copy without logging dictated text.
- After shutdown, a GLib async operation whose context is permanently stopped
  can retain its own cancellation callback capsule until GLib dispatches it.
  That capsule has only a weak request reference; a pending read additionally
  owns one fixed 64 KiB buffer that is zeroed when its callback runs. It cannot
  retain adapter State, the libsoup session, accumulated response bytes, or an
  application completion closure.
- Capture audio is bounded by `max_capture_seconds` (120 seconds by default,
  3.84 MB for the default 16 kHz mono S16LE format). A pending stop result is
  wiped synchronously on cancel/shutdown; it remains only until the normal
  serialized completion callback consumes it.
