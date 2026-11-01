#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace lemonvox {

// A monotonically increasing operation ID is owned by WorkflowCore. Adapters
// echo it unchanged so the core can reject a late completion after cancellation.
using OperationId = std::uint64_t;

// These categories cross adapter boundaries. Details, dictated text, URLs, and
// credentials never enter diagnostics.
enum class AdapterError : std::uint8_t {
    none,
    canceled,
    unavailable,
    invalid_input,
    network,
    response,
    target_changed,
    insertion_unavailable,
};

struct Completion {
    AdapterError error = AdapterError::none;
};

struct BytesResult {
    AdapterError error = AdapterError::none;
    std::vector<std::uint8_t> bytes;
};

enum class HttpMethod : std::uint8_t { get, post };

struct TransportHeader {
    std::string name;
    std::string value;
};

// The core moves a request into the adapter. The adapter owns every field until
// it has started the native request, then returns an owned response body.
struct TransportRequest {
    HttpMethod method = HttpMethod::get;
    std::string url;
    std::vector<TransportHeader> headers;
    std::vector<std::uint8_t> body;
    std::uint32_t timeout_ms = 0;
};

struct TransportResponse {
    AdapterError error = AdapterError::none;
    int status = 0;
    std::vector<std::uint8_t> bytes;
};

struct TargetToken {
    std::uint64_t value = 0;
};

// A token is opaque to the core. The insertion adapter captures an original
// token at operation start, compares it with the current target at completion,
// and invalidates its token storage on cancel or shutdown. The core never
// dereferences a token; no native pointers or toolkit types cross this boundary.
struct TargetSnapshot {
    TargetToken token;
    bool editable = false;
    bool secure = false;
    bool frontmost = false;
    bool matches_original = false;
};

struct TargetResult {
    AdapterError error = AdapterError::none;
    TargetSnapshot target;
};

struct PreferencesSnapshot {
    std::string endpoint;
    std::string asr_model;
    std::string cleanup_model;
    std::string cleanup_prompt;
    bool insert_current_target = false;
};

using VoidCompletion = std::function<void(OperationId, Completion)>;
using BytesCompletion = std::function<void(OperationId, BytesResult)>;
using TransportCompletion = std::function<void(OperationId, TransportResponse)>;
using TargetCompletion = std::function<void(OperationId, TargetResult)>;

// Operations may complete synchronously or later. The native shell establishes
// one serialized core executor and invokes every core entry point there.
// Adapters marshal completions back to that executor before invoking them.
// Each request completion is called at most once. cancel() may leave a queued
// completion in flight, so WorkflowCore must reject it by ID. shutdown()
// cancels pending native work and guarantees no callback afterwards.
class CaptureAdapter {
public:
    virtual ~CaptureAdapter() = default;
    virtual void start(OperationId operation, VoidCompletion completion) = 0;
    virtual void stop(OperationId operation, BytesCompletion completion) = 0;
    virtual void cancel(OperationId operation) = 0;
    virtual void shutdown() = 0;
};

class ByteTransport {
public:
    virtual ~ByteTransport() = default;
    virtual void send(OperationId operation, TransportRequest request,
        TransportCompletion completion) = 0;
    virtual void cancel(OperationId operation) = 0;
    virtual void shutdown() = 0;
};

class InsertionAdapter {
public:
    virtual ~InsertionAdapter() = default;
    virtual void capture_original(OperationId operation, TargetCompletion completion) = 0;
    virtual void inspect(OperationId operation, TargetToken original,
        TargetCompletion completion) = 0;
    virtual void insert(OperationId operation, TargetToken target, std::string text,
        VoidCompletion completion) = 0;
    virtual void cancel(OperationId operation) = 0;
    virtual void shutdown() = 0;
};

// The native shell obtains one immutable snapshot at start. Subsequent Settings
// edits apply to the next operation, never mutate a running operation.
class PreferencesAdapter {
public:
    virtual ~PreferencesAdapter() = default;
    virtual PreferencesSnapshot snapshot() const = 0;
};

enum class DiagnosticEvent : std::uint8_t {
    recording_started,
    transcription_started,
    cleanup_started,
    insertion_fallback,
    transport_failed,
    canceled,
    // Terminal success; the Mac bridge maps this to its paste-dispatched log.
    insertion_succeeded,
    // Terminal non-cancel failure. WorkflowCore never pairs it with
    // transport_failed for the same operation.
    dictation_failed,
};

struct DiagnosticEntry {
    DiagnosticEvent event = DiagnosticEvent::canceled;
    AdapterError error = AdapterError::none;
    int http_status = 0;
};

class DiagnosticsSink {
public:
    virtual ~DiagnosticsSink() = default;
    virtual void record(DiagnosticEntry entry) = 0;
};

enum class WorkflowPhase : std::uint8_t {
    idle,
    preparing,
    recording,
    transcribing,
    cleaning,
    inserting,
};

// These callbacks run on the core's serialized executor. The native shell owns
// the callbacks and separately dispatches to its UI thread before touching
// native views. text_ready receives owned private result text and must never
// forward it to DiagnosticsSink.
struct WorkflowCallbacks {
    std::function<void(OperationId, WorkflowPhase)> phase_changed;
    std::function<void(OperationId, std::string)> text_ready;
    std::function<void(OperationId, AdapterError)> failed;
};

} // namespace lemonvox
