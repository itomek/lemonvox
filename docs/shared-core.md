# Shared core boundary

The shared core is a C++17 library for dictation policy and workflow. Native
shells retain all presentation and operating-system work. This boundary is a
decision for incremental extraction, not a choice of a cross-platform UI
toolkit or an authorization to change Mac behavior.

| Area | Shared C++ core | Native adapter |
| --- | --- | --- |
| Workflow | Operation generations, start/stop sequencing, phase and error policy, stale-result rejection | UI dispatch and presentation |
| Configuration and protocol | Immutable operation snapshot, endpoint/model policy, request/response mapping | Preference storage, URLSession byte delivery on Mac |
| Capture | Requests start/stop/cancel and consumes owned WAV bytes | Permissions, AVFoundation/device selection, temporary storage and cleanup |
| Target and insertion | Safe-insertion decision from supplied facts | Focus/secure-field inspection, clipboard, paste and copy fallback |
| Diagnostics | Fixed events, error categories and status codes | Log storage and user-visible error text |

The current Mac shell remains Swift/AppKit. Windows is proposed to use C++/WinRT
with WinUI 3 and Win32 adapters; Ubuntu is proposed to use C++ with GTK4,
libadwaita and GLib/GIO portal adapters. Those are proposals until separate
platform build proofs select bindings and dependency versions.

## Adapter contract

[`core/adapters.h`](../core/adapters.h) is a direct C++ contract with no
platform toolkit types. It supplies `CaptureAdapter`, `ByteTransport`,
`InsertionAdapter`, `PreferencesAdapter`, and `DiagnosticsSink`, plus owned
request/result values, workflow callbacks, and content-free diagnostics.

- The native shell establishes one serialized core executor and invokes every
  `WorkflowCore` entry point there. An adapter may complete synchronously or
  asynchronously, but it marshals its completion back to that executor before
  calling the core. `WorkflowCore` creates a monotonic `OperationId`; adapters
  return it unchanged and the core accepts a completion only when its ID is
  current.
- An adapter invokes a request completion at most once. `cancel(id)` asks it to
  stop work, but an already queued completion can still arrive and must be
  ignored by the core. `shutdown()` cancels pending work and is the teardown
  barrier: it returns only once the adapter will not invoke another callback.
- Requests and responses own their strings and byte buffers. `ByteTransport`
  receives a moved `TransportRequest`; capture and transport results move owned
  bytes into their completion. The capture adapter owns and removes any native
  temporary-audio storage on success, failure, cancellation, and shutdown.
- At operation start, the core asks `InsertionAdapter` to `capture_original`.
  The adapter owns that opaque `TargetToken`; at completion it inspects the
  current target against the supplied original token and returns only safe
  facts. The core cannot dereference tokens. `cancel()` and `shutdown()`
  invalidate adapter token storage, so a late completion cannot revive a
  previous target.
- The native shell captures one `PreferencesSnapshot` at operation start.
  Settings edits apply to a later operation, which preserves the existing
  Settings-during-recording behavior until issue #6 changes it explicitly.
- `WorkflowCallbacks` run on the core's serialized executor. Each native shell
  separately dispatches them to its own UI thread before touching views. The
  core never calls AppKit, WinUI, GTK, or a platform run loop. `text_ready`
  carries owned private result text on that executor; it is never logged or
  passed to `DiagnosticsSink`.
- `DiagnosticEntry` carries only a fixed event, error category, and HTTP status.
  Audio, transcripts, prompts, URLs, credentials, and provider responses remain
  outside diagnostics.

Mac keeps URLSession as the first `ByteTransport` implementation, including its
loopback, no-redirect, proxy, cookie and credential-storage restrictions, plus
the current 60-second request and 120-second resource timeouts. This contract
neither selects a shared HTTP library nor changes that policy. This slice adds
no shared HTTP dependency. URLSession remains responsible for byte delivery,
timeouts, cancellation, and its ephemeral privacy configuration.

## Protocol policy and JSON

[`core/protocol.h`](../core/protocol.h) provides the protocol mapper in two
forms: a direct C++17 `lemonvox::Protocol` facade that produces owned
`TransportRequest` values for `ByteTransport`, and a narrow C-compatible Swift
bridge. It owns configuration/model validation, fixed request paths, multipart
WAV construction, cleanup JSON, inventory eligibility, response validation and
safe error categories. It never implements HTTP or parses URLs.

Native shells normalize an endpoint with their URL API and supply a canonical
base URL plus `LVEndpointFacts`. The protocol verifies those facts and appends
only its fixed paths. Mac uses `URLComponents`; another native shell must
provide equivalent canonical URL and loopback/scheme/userinfo/query/fragment/
`v1` facts before it can use the core.

`LVProtocolSlice` inputs are borrowed only for each synchronous C call; a null
data pointer is permitted only with zero size. Returned bytes, requests,
results, and inventories are core-owned and must be released with their named
`lv_protocol_*_free` function after the bridge copies them. The C bridge retains
no callback or input storage. The direct C++ facade converts those values into
the owned strings and vectors from [`adapters.h`](../core/adapters.h).

The parser is vendored [yyjson 0.13.0 at
`6447536015f3d600f3d65323b10976103b337ca7`](../third_party/yyjson/README.md),
an MIT dependency with pinned checksums and unchanged attribution. It compiles
as C17; C++ consumers compile as C++17. Both use
`YYJSON_READER_DEPTH_LIMIT=513` and the default `YYJSON_READ_NOFLAG` mode—no
relaxed syntax, feature stripping, or stop-when-done behavior is enabled.
Protocol fixtures preserve the observed Foundation rules: first duplicate key
wins; trailing data, raw NUL, invalid UTF-8, BOM, NaN/Infinity and lone
surrogates reject; valid surrogate pairs accept; 512 nested containers accept
and 513 reject. yyjson's configured depth guard excludes the root container, so
the core performs a post-parse 512-container check solely to retain that Mac
behavior. It adds no audio, response, transcript, or byte-size cap.

## Swift ABI boundary

[`core/session.h`](../core/session.h) is the narrow C-compatible bridge used by
Swift. Its ABI version is explicit and its C++ declarations assert standard
layout, trivial copying, field order, and alignment for `LVSession`. Swift and
the C++ core are compiled and linked into the same app with the supported Apple
toolchain; this is not a promise that arbitrary compilers, C++ standard
libraries, enum layouts, or binaries can interoperate. Other shells integrate
the direct C++ contract instead.

The bridge takes caller-owned scalar/session storage, copies inputs
synchronously, and returns scalars. It does not transfer allocations or retain
callbacks across languages. Future callback-bearing adapters stay in the direct
C++ layer unless a separately reviewed C bridge defines their lifetime.

## Build evidence

Run the portable source target with a new absolute output directory:

```sh
bash scripts/test-core.sh --output /absolute/new/core-build
```

It compiles `core/session.cpp`, `core/protocol.cpp`, and yyjson into
`liblemonvox_core.a`, then runs the C++ session, trigger, protocol, and
fake-adapter contract tests with C++17/C17 compilers on the available Unix
toolchain. The later Mac-adapter step compiles its Swift bridge parity tests
against these headers/objects. This target proves source/library isolation
there; a Windows build proof remains separate and is not implied.
