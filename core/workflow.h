#pragma once

#include "protocol.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lemonvox {

// The native shell normalizes endpoint facts before start(). WorkflowCore copies
// this value, so Settings edits cannot mutate an operation already in flight.
struct WorkflowConfig {
    PreferencesSnapshot preferences;
    ProtocolEndpointFacts endpoint;
};

enum class WorkflowFailureSource : std::uint8_t { adapter, protocol };

struct WorkflowFailure {
    WorkflowFailureSource source = WorkflowFailureSource::adapter;
    AdapterError adapter_error = AdapterError::none;
    LVProtocolStatus protocol_status = LV_PROTOCOL_OK;
    int http_status = 0;
    // Only Protocol::classify_http may supply this already-redacted, bounded text.
    // It is for native display only and never reaches DiagnosticsSink.
    std::string safe_detail;
};

enum class WorkflowOutcomeKind : std::uint8_t { inserted, copy_fallback };

enum class WorkflowFallbackReason : std::uint8_t {
    none,
    // Native Accessibility/target inspection could not be performed.
    accessibility_unavailable,
    target_unavailable,
    target_changed,
    secure_field,
    unsupported_field,
    // A target was available but native paste dispatch did not complete.
    paste_unavailable,
};

// This carries only terminal state, never dictated text. copy_fallback text is
// delivered separately through text_ready so native shells can retain it for a
// copy action without exposing it to diagnostics.
struct WorkflowOutcome {
    WorkflowOutcomeKind kind = WorkflowOutcomeKind::inserted;
    WorkflowFallbackReason fallback_reason = WorkflowFallbackReason::none;
};

struct WorkflowCoreCallbacks {
    std::function<void(OperationId, WorkflowPhase)> phase_changed;
    std::function<void(OperationId, std::string)> text_ready;
    std::function<void(OperationId, WorkflowFailure)> failed;
    std::function<void(OperationId, WorkflowOutcome)> completed;
};

class WorkflowCore {
public:
    WorkflowCore(CaptureAdapter &capture, ByteTransport &transport,
        InsertionAdapter &insertion, DiagnosticsSink &diagnostics,
        WorkflowCoreCallbacks callbacks = {});
    ~WorkflowCore();

    WorkflowCore(const WorkflowCore &) = delete;
    WorkflowCore &operator=(const WorkflowCore &) = delete;

    // Returns 0 when an operation is active, shut down, or delivering terminal
    // callbacks. A validation failure returns an allocated operation ID after
    // synchronously delivering its idle/failed terminal sequence; reentrant
    // start() from that sequence is rejected until delivery finishes.
    OperationId start(WorkflowConfig config);
    // The native shell supplies its already-generated UUID boundary. The core
    // copies it before asking the capture adapter to stop.
    bool stop(std::string multipart_boundary);
    void cancel();
    // Native bridge adapters must make shutdown() a no-callback barrier. The
    // destructor relies on that contract before their owning bridge is released.
    void shutdown();
    WorkflowPhase phase() const;
    bool is_shutdown() const;

private:
    enum class Step : std::uint8_t {
        none,
        original_target,
        first_models,
        capture_start,
        capture_stop,
        second_models,
        transcription,
        cleanup,
        inspect_target,
        insert,
    };

    bool accepts(OperationId operation, std::uint64_t step, Step expected) const;
    bool consume(OperationId operation, std::uint64_t step, Step expected);
    std::uint64_t begin_step(Step step);
    void request_original_target(OperationId operation);
    void request_models(OperationId operation, Step gate);
    void request_capture_start(OperationId operation);
    void request_capture_stop(OperationId operation);
    void request_transcription(OperationId operation);
    void request_cleanup(OperationId operation, TransportRequest request);
    void request_insertion(OperationId operation, std::string text);
    void request_insert(OperationId operation, TargetToken target);
    void fallback_to_text(OperationId operation, WorkflowFallbackReason reason);
    void finish_success(OperationId operation);
    void finish_canceled(OperationId operation, bool notify);
    void fail_protocol(OperationId operation, LVProtocolStatus status, int http_status,
        std::string safe_detail = {});
    void fail_adapter(OperationId operation, AdapterError error);
    void invalidate_active();
    void cancel_native(OperationId operation);
    void set_phase(OperationId operation, WorkflowPhase phase);

    CaptureAdapter &capture_;
    ByteTransport &transport_;
    InsertionAdapter &insertion_;
    DiagnosticsSink &diagnostics_;
    WorkflowCoreCallbacks callbacks_;
    WorkflowConfig config_;
    ProtocolConfiguration protocol_config_;
    TargetToken original_target_;
    WorkflowFallbackReason original_fallback_reason_ = WorkflowFallbackReason::target_unavailable;
    std::vector<std::uint8_t> audio_;
    std::string multipart_boundary_;
    std::string result_text_;
    WorkflowPhase phase_ = WorkflowPhase::idle;
    OperationId next_operation_ = 1;
    OperationId active_operation_ = 0;
    std::uint64_t next_step_ = 1;
    std::uint64_t pending_step_ = 0;
    Step step_ = Step::none;
    // Terminal callbacks are an atomic sequence: a synchronous restart is
    // rejected until phase, text/outcome, or failure delivery has completed.
    bool delivering_terminal_ = false;
    bool shutting_down_ = false;
};

} // namespace lemonvox
