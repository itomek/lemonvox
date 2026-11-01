#include "workflow_bridge.h"

#include "workflow.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

bool valid_slice(LVProtocolSlice slice) {
    return slice.data != nullptr || slice.size == 0;
}

bool copy_slice(LVProtocolSlice slice, std::string &output) {
    if (!valid_slice(slice)) return false;
    if (slice.size == 0) {
        output.clear();
        return true;
    }
    output.assign(reinterpret_cast<const char *>(slice.data), slice.size);
    return true;
}

bool copy_bytes(LVProtocolSlice slice, std::vector<std::uint8_t> &output) {
    if (!valid_slice(slice)) return false;
    if (slice.size == 0) {
        output.clear();
        return true;
    }
    output.assign(slice.data, slice.data + slice.size);
    return true;
}

lemonvox::AdapterError adapter_error(LVWorkflowAdapterError error) {
    switch (error) {
        case LV_WORKFLOW_ADAPTER_NONE: return lemonvox::AdapterError::none;
        case LV_WORKFLOW_ADAPTER_CANCELED: return lemonvox::AdapterError::canceled;
        case LV_WORKFLOW_ADAPTER_UNAVAILABLE: return lemonvox::AdapterError::unavailable;
        case LV_WORKFLOW_ADAPTER_INVALID_INPUT: return lemonvox::AdapterError::invalid_input;
        case LV_WORKFLOW_ADAPTER_NETWORK: return lemonvox::AdapterError::network;
        case LV_WORKFLOW_ADAPTER_RESPONSE: return lemonvox::AdapterError::response;
        case LV_WORKFLOW_ADAPTER_TARGET_CHANGED: return lemonvox::AdapterError::target_changed;
        case LV_WORKFLOW_ADAPTER_INSERTION_UNAVAILABLE: return lemonvox::AdapterError::insertion_unavailable;
    }
    return lemonvox::AdapterError::invalid_input;
}

LVWorkflowAdapterError bridge_error(lemonvox::AdapterError error) {
    switch (error) {
        case lemonvox::AdapterError::none: return LV_WORKFLOW_ADAPTER_NONE;
        case lemonvox::AdapterError::canceled: return LV_WORKFLOW_ADAPTER_CANCELED;
        case lemonvox::AdapterError::unavailable: return LV_WORKFLOW_ADAPTER_UNAVAILABLE;
        case lemonvox::AdapterError::invalid_input: return LV_WORKFLOW_ADAPTER_INVALID_INPUT;
        case lemonvox::AdapterError::network: return LV_WORKFLOW_ADAPTER_NETWORK;
        case lemonvox::AdapterError::response: return LV_WORKFLOW_ADAPTER_RESPONSE;
        case lemonvox::AdapterError::target_changed: return LV_WORKFLOW_ADAPTER_TARGET_CHANGED;
        case lemonvox::AdapterError::insertion_unavailable: return LV_WORKFLOW_ADAPTER_INSERTION_UNAVAILABLE;
    }
    return LV_WORKFLOW_ADAPTER_INVALID_INPUT;
}

LVWorkflowPhase bridge_phase(lemonvox::WorkflowPhase phase) {
    switch (phase) {
        case lemonvox::WorkflowPhase::idle: return LV_WORKFLOW_PHASE_IDLE;
        case lemonvox::WorkflowPhase::preparing: return LV_WORKFLOW_PHASE_PREPARING;
        case lemonvox::WorkflowPhase::recording: return LV_WORKFLOW_PHASE_RECORDING;
        case lemonvox::WorkflowPhase::transcribing: return LV_WORKFLOW_PHASE_TRANSCRIBING;
        case lemonvox::WorkflowPhase::cleaning: return LV_WORKFLOW_PHASE_CLEANING;
        case lemonvox::WorkflowPhase::inserting: return LV_WORKFLOW_PHASE_INSERTING;
    }
    return LV_WORKFLOW_PHASE_IDLE;
}

LVWorkflowFailureSource bridge_failure_source(lemonvox::WorkflowFailureSource source) {
    return source == lemonvox::WorkflowFailureSource::protocol
        ? LV_WORKFLOW_FAILURE_PROTOCOL : LV_WORKFLOW_FAILURE_ADAPTER;
}

LVWorkflowOutcomeKind bridge_outcome(lemonvox::WorkflowOutcomeKind kind) {
    return kind == lemonvox::WorkflowOutcomeKind::copy_fallback
        ? LV_WORKFLOW_OUTCOME_COPY_FALLBACK : LV_WORKFLOW_OUTCOME_INSERTED;
}

LVWorkflowFallbackReason bridge_fallback(lemonvox::WorkflowFallbackReason reason) {
    switch (reason) {
        case lemonvox::WorkflowFallbackReason::none: return LV_WORKFLOW_FALLBACK_NONE;
        case lemonvox::WorkflowFallbackReason::accessibility_unavailable:
            return LV_WORKFLOW_FALLBACK_ACCESSIBILITY_UNAVAILABLE;
        case lemonvox::WorkflowFallbackReason::target_unavailable:
            return LV_WORKFLOW_FALLBACK_TARGET_UNAVAILABLE;
        case lemonvox::WorkflowFallbackReason::target_changed:
            return LV_WORKFLOW_FALLBACK_TARGET_CHANGED;
        case lemonvox::WorkflowFallbackReason::secure_field:
            return LV_WORKFLOW_FALLBACK_SECURE_FIELD;
        case lemonvox::WorkflowFallbackReason::unsupported_field:
            return LV_WORKFLOW_FALLBACK_UNSUPPORTED_FIELD;
        case lemonvox::WorkflowFallbackReason::paste_unavailable:
            return LV_WORKFLOW_FALLBACK_PASTE_UNAVAILABLE;
    }
    return LV_WORKFLOW_FALLBACK_TARGET_UNAVAILABLE;
}

LVWorkflowDiagnosticEvent bridge_diagnostic(lemonvox::DiagnosticEvent event) {
    switch (event) {
        case lemonvox::DiagnosticEvent::recording_started:
            return LV_WORKFLOW_DIAGNOSTIC_RECORDING_STARTED;
        case lemonvox::DiagnosticEvent::transcription_started:
            return LV_WORKFLOW_DIAGNOSTIC_TRANSCRIPTION_STARTED;
        case lemonvox::DiagnosticEvent::cleanup_started:
            return LV_WORKFLOW_DIAGNOSTIC_CLEANUP_STARTED;
        case lemonvox::DiagnosticEvent::insertion_fallback:
            return LV_WORKFLOW_DIAGNOSTIC_INSERTION_FALLBACK;
        case lemonvox::DiagnosticEvent::transport_failed:
            return LV_WORKFLOW_DIAGNOSTIC_TRANSPORT_FAILED;
        case lemonvox::DiagnosticEvent::canceled:
            return LV_WORKFLOW_DIAGNOSTIC_CANCELED;
        case lemonvox::DiagnosticEvent::insertion_succeeded:
            return LV_WORKFLOW_DIAGNOSTIC_INSERTION_SUCCEEDED;
        case lemonvox::DiagnosticEvent::dictation_failed:
            return LV_WORKFLOW_DIAGNOSTIC_DICTATION_FAILED;
    }
    return LV_WORKFLOW_DIAGNOSTIC_DICTATION_FAILED;
}

lemonvox::TargetSnapshot core_target(LVWorkflowTarget target) {
    return {{target.token}, target.editable != 0, target.secure != 0,
        target.frontmost != 0, target.matches_original != 0};
}

bool native_callbacks_complete(const LVWorkflowNativeCallbacks &callbacks) {
    return callbacks.capture_original && callbacks.capture_start && callbacks.capture_stop &&
        callbacks.transport_send && callbacks.inspect_target && callbacks.insert_text &&
        callbacks.capture_cancel && callbacks.transport_cancel && callbacks.insertion_cancel &&
        callbacks.capture_shutdown && callbacks.transport_shutdown && callbacks.insertion_shutdown;
}

} // namespace

namespace {
void destroy_deferred_if_ready(LVWorkflow *workflow);
}

struct LVWorkflow final : lemonvox::DiagnosticsSink {
    struct CaptureBridge final : lemonvox::CaptureAdapter {
        explicit CaptureBridge(LVWorkflow &owner) : owner(owner) {}
        void start(lemonvox::OperationId operation, lemonvox::VoidCompletion completion) override {
            owner.request_capture_start(operation, std::move(completion));
        }
        void stop(lemonvox::OperationId operation, lemonvox::BytesCompletion completion) override {
            owner.request_capture_stop(operation, std::move(completion));
        }
        void cancel(lemonvox::OperationId operation) override { owner.cancel_capture(operation); }
        void shutdown() override { owner.shutdown_capture(); }
        LVWorkflow &owner;
    };

    struct TransportBridge final : lemonvox::ByteTransport {
        explicit TransportBridge(LVWorkflow &owner) : owner(owner) {}
        void send(lemonvox::OperationId operation, lemonvox::TransportRequest request,
            lemonvox::TransportCompletion completion) override {
            owner.request_transport(operation, std::move(request), std::move(completion));
        }
        void cancel(lemonvox::OperationId operation) override { owner.cancel_transport(operation); }
        void shutdown() override { owner.shutdown_transport(); }
        LVWorkflow &owner;
    };

    struct InsertionBridge final : lemonvox::InsertionAdapter {
        explicit InsertionBridge(LVWorkflow &owner) : owner(owner) {}
        void capture_original(lemonvox::OperationId operation, lemonvox::TargetCompletion completion) override {
            owner.request_original_target(operation, std::move(completion));
        }
        void inspect(lemonvox::OperationId operation, lemonvox::TargetToken original,
            lemonvox::TargetCompletion completion) override {
            owner.request_inspect(operation, original, std::move(completion));
        }
        void insert(lemonvox::OperationId operation, lemonvox::TargetToken target, std::string text,
            lemonvox::VoidCompletion completion) override {
            owner.request_insert(operation, target, std::move(text), std::move(completion));
        }
        void cancel(lemonvox::OperationId operation) override { owner.cancel_insertion(operation); }
        void shutdown() override { owner.shutdown_insertion(); }
        LVWorkflow &owner;
    };

    struct EntryGuard {
        explicit EntryGuard(LVWorkflow *workflow) : workflow(workflow) { ++workflow->entry_depth; }
        ~EntryGuard() {
            auto *current = workflow;
            --current->entry_depth;
            destroy_deferred_if_ready(current);
        }
        LVWorkflow *workflow;
    };

    struct CallbackGuard {
        explicit CallbackGuard(LVWorkflow *workflow) : workflow(workflow) { ++workflow->callback_depth; }
        ~CallbackGuard() {
            auto *current = workflow;
            --current->callback_depth;
            destroy_deferred_if_ready(current);
        }
        LVWorkflow *workflow;
    };

    struct PendingVoid {
        std::uint64_t operation;
        lemonvox::VoidCompletion completion;
    };
    struct PendingBytes {
        std::uint64_t operation;
        lemonvox::BytesCompletion completion;
    };
    struct PendingTransport {
        std::uint64_t operation;
        lemonvox::TransportCompletion completion;
    };
    struct PendingTarget {
        std::uint64_t operation;
        lemonvox::TargetCompletion completion;
    };

    LVWorkflow(const LVWorkflowNativeCallbacks &native_callbacks,
        const LVWorkflowCoreCallbacks &core_callbacks, void *callback_context)
        : native(native_callbacks), callbacks(core_callbacks), context(callback_context),
          capture(*this), transport(*this), insertion(*this) {
        lemonvox::WorkflowCoreCallbacks workflow_callbacks;
        workflow_callbacks.phase_changed = [this](lemonvox::OperationId operation, lemonvox::WorkflowPhase phase) {
            if (!shutdown_requested && callbacks.phase_changed) {
                CallbackGuard guard(this);
                callbacks.phase_changed(context, this, operation, bridge_phase(phase));
            }
        };
        workflow_callbacks.text_ready = [this](lemonvox::OperationId operation, std::string text) {
            if (!shutdown_requested && callbacks.text_ready) {
                const LVProtocolSlice view{reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
                CallbackGuard guard(this);
                callbacks.text_ready(context, this, operation, view);
            }
        };
        workflow_callbacks.failed = [this](lemonvox::OperationId operation, lemonvox::WorkflowFailure failure) {
            if (!shutdown_requested && callbacks.failed) {
                const LVWorkflowFailure view{bridge_failure_source(failure.source), bridge_error(failure.adapter_error),
                    failure.protocol_status, failure.http_status,
                    {reinterpret_cast<const std::uint8_t *>(failure.safe_detail.data()), failure.safe_detail.size()}};
                CallbackGuard guard(this);
                callbacks.failed(context, this, operation, view);
            }
        };
        workflow_callbacks.completed = [this](lemonvox::OperationId operation, lemonvox::WorkflowOutcome outcome) {
            if (!shutdown_requested && callbacks.completed) {
                CallbackGuard guard(this);
                callbacks.completed(context, this, operation,
                    {bridge_outcome(outcome.kind), bridge_fallback(outcome.fallback_reason)});
            }
        };
        core = std::make_unique<lemonvox::WorkflowCore>(
            capture, transport, insertion,
            static_cast<lemonvox::DiagnosticsSink &>(*this), std::move(workflow_callbacks));
    }

    ~LVWorkflow() override {
        shutdown_requested = true;
        native_callbacks_suppressed = true;
        context = nullptr;
        callbacks = {};
        clear_pending();
        if (core) core->shutdown();
    }

    void request_original_target(lemonvox::OperationId operation, lemonvox::TargetCompletion completion) {
        if (native_callbacks_suppressed) return;
        const auto request = add_pending(original_pending, operation, std::move(completion));
        CallbackGuard guard(this);
        native.capture_original(context, this, operation, request);
    }

    void request_capture_start(lemonvox::OperationId operation, lemonvox::VoidCompletion completion) {
        if (native_callbacks_suppressed) return;
        const auto request = add_pending(start_pending, operation, std::move(completion));
        CallbackGuard guard(this);
        native.capture_start(context, this, operation, request);
    }

    void request_capture_stop(lemonvox::OperationId operation, lemonvox::BytesCompletion completion) {
        if (native_callbacks_suppressed) return;
        const auto request = add_pending(stop_pending, operation, std::move(completion));
        CallbackGuard guard(this);
        native.capture_stop(context, this, operation, request);
    }

    void request_transport(lemonvox::OperationId operation, lemonvox::TransportRequest request,
        lemonvox::TransportCompletion completion) {
        if (native_callbacks_suppressed) return;
        const auto request_id = add_pending(transport_pending, operation, std::move(completion));
        const auto method = request.method == lemonvox::HttpMethod::post
            ? LV_WORKFLOW_METHOD_POST : LV_WORKFLOW_METHOD_GET;
        const auto content_type = request.headers.empty() ? std::string() : request.headers.front().value;
        const LVWorkflowTransportRequest view{method,
            {reinterpret_cast<const std::uint8_t *>(request.url.data()), request.url.size()},
            {request.body.data(), request.body.size()},
            {reinterpret_cast<const std::uint8_t *>(content_type.data()), content_type.size()}, request.timeout_ms};
        CallbackGuard guard(this);
        native.transport_send(context, this, operation, request_id, &view);
    }

    void request_inspect(lemonvox::OperationId operation, lemonvox::TargetToken original,
        lemonvox::TargetCompletion completion) {
        if (native_callbacks_suppressed) return;
        const auto request = add_pending(inspect_pending, operation, std::move(completion));
        CallbackGuard guard(this);
        native.inspect_target(context, this, operation, request, original.value);
    }

    void request_insert(lemonvox::OperationId operation, lemonvox::TargetToken target, std::string text,
        lemonvox::VoidCompletion completion) {
        if (native_callbacks_suppressed) return;
        const auto request = add_pending(insert_pending, operation, std::move(completion));
        const LVProtocolSlice view{reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
        CallbackGuard guard(this);
        native.insert_text(context, this, operation, request, target.value, view);
    }

    void cancel_capture(lemonvox::OperationId operation) {
        erase_operation(start_pending, operation);
        erase_operation(stop_pending, operation);
        if (native_callbacks_suppressed) return;
        CallbackGuard guard(this);
        native.capture_cancel(context, this, operation);
    }

    void cancel_transport(lemonvox::OperationId operation) {
        erase_operation(transport_pending, operation);
        if (native_callbacks_suppressed) return;
        CallbackGuard guard(this);
        native.transport_cancel(context, this, operation);
    }

    void cancel_insertion(lemonvox::OperationId operation) {
        erase_operation(original_pending, operation);
        erase_operation(inspect_pending, operation);
        erase_operation(insert_pending, operation);
        if (native_callbacks_suppressed) return;
        CallbackGuard guard(this);
        native.insertion_cancel(context, this, operation);
    }

    void shutdown_capture() {
        start_pending.clear();
        stop_pending.clear();
        if (native_callbacks_suppressed) return;
        CallbackGuard guard(this);
        native.capture_shutdown(context, this);
    }

    void shutdown_transport() {
        transport_pending.clear();
        if (native_callbacks_suppressed) return;
        CallbackGuard guard(this);
        native.transport_shutdown(context, this);
    }

    void shutdown_insertion() {
        original_pending.clear();
        inspect_pending.clear();
        insert_pending.clear();
        if (native_callbacks_suppressed) return;
        CallbackGuard guard(this);
        native.insertion_shutdown(context, this);
    }

    void record(lemonvox::DiagnosticEntry entry) override {
        if (!shutdown_requested && callbacks.diagnostic) {
            CallbackGuard guard(this);
            callbacks.diagnostic(context, this, bridge_diagnostic(entry.event), bridge_error(entry.error), entry.http_status);
        }
    }

    std::uint64_t add_pending(std::unordered_map<std::uint64_t, PendingVoid> &pending,
        std::uint64_t operation, lemonvox::VoidCompletion completion) {
        const auto request = next_request++;
        pending.emplace(request, PendingVoid{operation, std::move(completion)});
        return request;
    }

    std::uint64_t add_pending(std::unordered_map<std::uint64_t, PendingBytes> &pending,
        std::uint64_t operation, lemonvox::BytesCompletion completion) {
        const auto request = next_request++;
        pending.emplace(request, PendingBytes{operation, std::move(completion)});
        return request;
    }

    std::uint64_t add_pending(std::unordered_map<std::uint64_t, PendingTransport> &pending,
        std::uint64_t operation, lemonvox::TransportCompletion completion) {
        const auto request = next_request++;
        pending.emplace(request, PendingTransport{operation, std::move(completion)});
        return request;
    }

    std::uint64_t add_pending(std::unordered_map<std::uint64_t, PendingTarget> &pending,
        std::uint64_t operation, lemonvox::TargetCompletion completion) {
        const auto request = next_request++;
        pending.emplace(request, PendingTarget{operation, std::move(completion)});
        return request;
    }

    template <typename Pending>
    bool take_pending(std::unordered_map<std::uint64_t, Pending> &pending, std::uint64_t operation,
        std::uint64_t request, Pending &result) {
        const auto iterator = pending.find(request);
        if (iterator == pending.end() || iterator->second.operation != operation || shutdown_requested) return false;
        result = std::move(iterator->second);
        pending.erase(iterator);
        return true;
    }

    template <typename Pending>
    static void erase_operation(std::unordered_map<std::uint64_t, Pending> &pending, std::uint64_t operation) {
        for (auto iterator = pending.begin(); iterator != pending.end();) {
            if (iterator->second.operation == operation) iterator = pending.erase(iterator);
            else ++iterator;
        }
    }

    void clear_pending_for_operation(std::uint64_t operation) {
        erase_operation(original_pending, operation);
        erase_operation(start_pending, operation);
        erase_operation(stop_pending, operation);
        erase_operation(transport_pending, operation);
        erase_operation(inspect_pending, operation);
        erase_operation(insert_pending, operation);
    }

    void clear_pending() {
        original_pending.clear();
        start_pending.clear();
        stop_pending.clear();
        transport_pending.clear();
        inspect_pending.clear();
        insert_pending.clear();
    }

    void abandon() {
        if (destroy_after_unwind) return;
        shutdown_requested = true;
        native_callbacks_suppressed = true;
        context = nullptr;
        callbacks = {};
        clear_pending();
        destroy_after_unwind = true;
        if (core) core->shutdown();
    }

    LVWorkflowNativeCallbacks native{};
    LVWorkflowCoreCallbacks callbacks{};
    void *context = nullptr;
    CaptureBridge capture;
    TransportBridge transport;
    InsertionBridge insertion;
    std::unique_ptr<lemonvox::WorkflowCore> core;
    std::unordered_map<std::uint64_t, PendingTarget> original_pending;
    std::unordered_map<std::uint64_t, PendingVoid> start_pending;
    std::unordered_map<std::uint64_t, PendingBytes> stop_pending;
    std::unordered_map<std::uint64_t, PendingTransport> transport_pending;
    std::unordered_map<std::uint64_t, PendingTarget> inspect_pending;
    std::unordered_map<std::uint64_t, PendingVoid> insert_pending;
    std::uint64_t next_request = 1;
    std::uint32_t entry_depth = 0;
    std::uint32_t callback_depth = 0;
    bool shutdown_requested = false;
    bool native_callbacks_suppressed = false;
    bool destroy_after_unwind = false;
};

namespace {

void destroy_deferred_if_ready(LVWorkflow *workflow) {
    if (workflow && workflow->destroy_after_unwind && workflow->entry_depth == 0 &&
        workflow->callback_depth == 0) {
        delete workflow;
    }
}

} // namespace

namespace {

bool copy_config(const LVWorkflowConfig &source, lemonvox::WorkflowConfig &destination) {
    if (!valid_slice(source.endpoint.canonical_base_url) || !copy_slice(source.preference_endpoint,
            destination.preferences.endpoint) || !copy_slice(source.asr_model, destination.preferences.asr_model) ||
        !copy_slice(source.cleanup_model, destination.preferences.cleanup_model) ||
        !copy_slice(source.cleanup_prompt, destination.preferences.cleanup_prompt)) {
        return false;
    }
    destination.preferences.insert_current_target = source.insert_current_target != 0;
    if (!copy_slice(source.endpoint.canonical_base_url, destination.endpoint.canonical_base_url)) return false;
    destination.endpoint.scheme_allowed = source.endpoint.scheme_allowed != 0;
    destination.endpoint.host_loopback = source.endpoint.host_loopback != 0;
    destination.endpoint.no_userinfo = source.endpoint.no_userinfo != 0;
    destination.endpoint.no_query_fragment = source.endpoint.no_query_fragment != 0;
    destination.endpoint.path_ends_v1 = source.endpoint.path_ends_v1 != 0;
    return true;
}

} // namespace

extern "C" LVWorkflow *lv_workflow_create(const LVWorkflowNativeCallbacks *native_callbacks,
    const LVWorkflowCoreCallbacks *core_callbacks, void *context) {
    if (!native_callbacks || !native_callbacks_complete(*native_callbacks)) return nullptr;
    try {
        return new LVWorkflow(*native_callbacks, core_callbacks ? *core_callbacks : LVWorkflowCoreCallbacks{}, context);
    } catch (...) {
        return nullptr;
    }
}

extern "C" uint64_t lv_workflow_start(LVWorkflow *workflow, const LVWorkflowConfig *config) {
    if (!workflow || !config || workflow->shutdown_requested) return 0;
    try {
        LVWorkflow::EntryGuard guard(workflow);
        lemonvox::WorkflowConfig copied;
        if (!copy_config(*config, copied)) return 0;
        return workflow->core->start(std::move(copied));
    } catch (...) {
        return 0;
    }
}

extern "C" int lv_workflow_stop(LVWorkflow *workflow, LVProtocolSlice multipart_boundary) {
    if (!workflow || workflow->shutdown_requested) return 0;
    try {
        LVWorkflow::EntryGuard guard(workflow);
        std::string boundary;
        return copy_slice(multipart_boundary, boundary) && workflow->core->stop(std::move(boundary));
    } catch (...) {
        return 0;
    }
}

extern "C" void lv_workflow_cancel(LVWorkflow *workflow) {
    if (!workflow || workflow->shutdown_requested) return;
    try {
        LVWorkflow::EntryGuard guard(workflow);
        workflow->core->cancel();
    } catch (...) {
    }
}

extern "C" void lv_workflow_shutdown(LVWorkflow *workflow) {
    if (!workflow || workflow->shutdown_requested) return;
    try {
        LVWorkflow::EntryGuard guard(workflow);
        workflow->shutdown_requested = true;
        workflow->clear_pending();
        workflow->core->shutdown();
    } catch (...) {
    }
}

extern "C" int lv_workflow_destroy(LVWorkflow *workflow) {
    if (!workflow || !workflow->shutdown_requested) return 0;
    if (workflow->entry_depth != 0 || workflow->callback_depth != 0) {
        workflow->destroy_after_unwind = true;
        return 0;
    }
    delete workflow;
    return 1;
}

extern "C" int lv_workflow_is_shutdown(const LVWorkflow *workflow) {
    return workflow && workflow->shutdown_requested;
}

extern "C" void lv_workflow_abandon(LVWorkflow *workflow) {
    if (!workflow) return;
    try {
        workflow->abandon();
        destroy_deferred_if_ready(workflow);
    } catch (...) {
    }
}

extern "C" int lv_workflow_complete_capture_original(LVWorkflow *workflow, uint64_t operation,
    uint64_t request_id, LVWorkflowAdapterError error, LVWorkflowTarget target) {
    if (!workflow) return 0;
    try {
        LVWorkflow::EntryGuard guard(workflow);
        LVWorkflow::PendingTarget pending{};
        if (!workflow->take_pending(workflow->original_pending, operation, request_id, pending)) return 0;
        pending.completion(operation, {adapter_error(error), core_target(target)});
        return 1;
    } catch (...) {
        return 0;
    }
}

extern "C" int lv_workflow_complete_capture_start(LVWorkflow *workflow, uint64_t operation,
    uint64_t request_id, LVWorkflowAdapterError error) {
    if (!workflow) return 0;
    try {
        LVWorkflow::EntryGuard guard(workflow);
        LVWorkflow::PendingVoid pending{};
        if (!workflow->take_pending(workflow->start_pending, operation, request_id, pending)) return 0;
        pending.completion(operation, {adapter_error(error)});
        return 1;
    } catch (...) {
        return 0;
    }
}

extern "C" int lv_workflow_complete_capture_stop(LVWorkflow *workflow, uint64_t operation,
    uint64_t request_id, LVWorkflowAdapterError error, LVProtocolSlice wav) {
    if (!workflow) return 0;
    try {
        LVWorkflow::EntryGuard guard(workflow);
        LVWorkflow::PendingBytes pending{};
        std::vector<std::uint8_t> bytes;
        if (!copy_bytes(wav, bytes) || !workflow->take_pending(workflow->stop_pending, operation, request_id, pending)) {
            return 0;
        }
        pending.completion(operation, {adapter_error(error), std::move(bytes)});
        return 1;
    } catch (...) {
        return 0;
    }
}

extern "C" int lv_workflow_complete_transport(LVWorkflow *workflow, uint64_t operation,
    uint64_t request_id, LVWorkflowAdapterError error, int http_status, LVProtocolSlice body) {
    if (!workflow) return 0;
    try {
        LVWorkflow::EntryGuard guard(workflow);
        LVWorkflow::PendingTransport pending{};
        std::vector<std::uint8_t> bytes;
        if (!copy_bytes(body, bytes) || !workflow->take_pending(workflow->transport_pending, operation, request_id, pending)) {
            return 0;
        }
        pending.completion(operation, {adapter_error(error), http_status, std::move(bytes)});
        return 1;
    } catch (...) {
        return 0;
    }
}

extern "C" int lv_workflow_complete_inspect(LVWorkflow *workflow, uint64_t operation,
    uint64_t request_id, LVWorkflowAdapterError error, LVWorkflowTarget target) {
    if (!workflow) return 0;
    try {
        LVWorkflow::EntryGuard guard(workflow);
        LVWorkflow::PendingTarget pending{};
        if (!workflow->take_pending(workflow->inspect_pending, operation, request_id, pending)) return 0;
        pending.completion(operation, {adapter_error(error), core_target(target)});
        return 1;
    } catch (...) {
        return 0;
    }
}

extern "C" int lv_workflow_complete_insert(LVWorkflow *workflow, uint64_t operation,
    uint64_t request_id, LVWorkflowAdapterError error) {
    if (!workflow) return 0;
    try {
        LVWorkflow::EntryGuard guard(workflow);
        LVWorkflow::PendingVoid pending{};
        if (!workflow->take_pending(workflow->insert_pending, operation, request_id, pending)) return 0;
        pending.completion(operation, {adapter_error(error)});
        return 1;
    } catch (...) {
        return 0;
    }
}
