#include "workflow.h"

#include <utility>

namespace lemonvox {

namespace {

ProtocolConfiguration protocol_config_from(const WorkflowConfig &config) {
    ProtocolConfiguration result;
    result.endpoint.canonical_base_url = config.endpoint.canonical_base_url;
    result.endpoint.scheme_allowed = config.endpoint.scheme_allowed;
    result.endpoint.host_loopback = config.endpoint.host_loopback;
    result.endpoint.no_userinfo = config.endpoint.no_userinfo;
    result.endpoint.no_query_fragment = config.endpoint.no_query_fragment;
    result.endpoint.path_ends_v1 = config.endpoint.path_ends_v1;
    result.asr_model = config.preferences.asr_model;
    result.cleanup_model = config.preferences.cleanup_model;
    result.cleanup_prompt = config.preferences.cleanup_prompt;
    return result;
}

bool is_fallback_error(AdapterError error) {
    return error == AdapterError::unavailable || error == AdapterError::target_changed ||
        error == AdapterError::insertion_unavailable;
}

int terminal_http_status(LVProtocolStatus status, int http_status) {
    return status == LV_PROTOCOL_HTTP || status == LV_PROTOCOL_UPSTREAM ? http_status : 0;
}

} // namespace

WorkflowCore::WorkflowCore(CaptureAdapter &capture, ByteTransport &transport,
    InsertionAdapter &insertion, DiagnosticsSink &diagnostics,
    WorkflowCoreCallbacks callbacks)
    : capture_(capture), transport_(transport), insertion_(insertion), diagnostics_(diagnostics),
      callbacks_(std::move(callbacks)) {}

WorkflowCore::~WorkflowCore() {
    shutdown();
}

OperationId WorkflowCore::start(WorkflowConfig config) {
    if (shutting_down_ || delivering_terminal_ || active_operation_ != 0) return 0;

    const auto operation = next_operation_++;
    const auto protocol_config = protocol_config_from(config);
    const auto validation = Protocol::validate_config(protocol_config);
    if (validation != LV_PROTOCOL_OK) {
        diagnostics_.record({DiagnosticEvent::dictation_failed, AdapterError::invalid_input, 0});
        // An invalid snapshot has no active native request to cancel, but it
        // still delivers the same atomic terminal callback sequence as an
        // in-flight failure. In particular, a callback cannot start a second
        // operation before this start() returns its allocated ID.
        delivering_terminal_ = true;
        set_phase(operation, WorkflowPhase::idle);
        if (!shutting_down_ && callbacks_.failed) {
            callbacks_.failed(operation, {WorkflowFailureSource::protocol, AdapterError::none,
                validation, 0, {}});
        }
        if (!shutting_down_) delivering_terminal_ = false;
        return operation;
    }

    config_ = std::move(config);
    protocol_config_ = protocol_config;
    original_target_ = {};
    original_fallback_reason_ = WorkflowFallbackReason::target_unavailable;
    active_operation_ = operation;
    set_phase(operation, WorkflowPhase::preparing);
    if (active_operation_ != operation || shutting_down_) return operation;
    request_original_target(operation);
    return operation;
}

bool WorkflowCore::stop(std::string multipart_boundary) {
    if (shutting_down_ || active_operation_ == 0 || phase_ != WorkflowPhase::recording) return false;

    const auto operation = active_operation_;
    multipart_boundary_ = std::move(multipart_boundary);
    set_phase(operation, WorkflowPhase::transcribing);
    if (active_operation_ != operation || shutting_down_) return false;
    request_capture_stop(operation);
    return true;
}

void WorkflowCore::cancel() {
    finish_canceled(active_operation_, true);
}

void WorkflowCore::shutdown() {
    if (shutting_down_) return;
    shutting_down_ = true;
    const auto operation = active_operation_;
    if (operation != 0) {
        invalidate_active();
        cancel_native(operation);
        diagnostics_.record({DiagnosticEvent::canceled, AdapterError::canceled, 0});
    }
    capture_.shutdown();
    transport_.shutdown();
    insertion_.shutdown();
}

WorkflowPhase WorkflowCore::phase() const {
    return phase_;
}

bool WorkflowCore::is_shutdown() const {
    return shutting_down_;
}

bool WorkflowCore::accepts(OperationId operation, std::uint64_t step, Step expected) const {
    return !shutting_down_ && active_operation_ == operation && pending_step_ == step && step_ == expected;
}

bool WorkflowCore::consume(OperationId operation, std::uint64_t step, Step expected) {
    if (!accepts(operation, step, expected)) return false;
    pending_step_ = 0;
    step_ = Step::none;
    return true;
}

std::uint64_t WorkflowCore::begin_step(Step step) {
    step_ = step;
    pending_step_ = next_step_++;
    return pending_step_;
}

void WorkflowCore::request_original_target(OperationId operation) {
    const auto step = begin_step(Step::original_target);
    insertion_.capture_original(operation, [this, step](OperationId completed, TargetResult result) {
        if (!consume(completed, step, Step::original_target)) return;
        if (result.error == AdapterError::canceled) {
            finish_canceled(completed, true);
            return;
        }
        if (result.error != AdapterError::none && !is_fallback_error(result.error)) {
            fail_adapter(completed, result.error);
            return;
        }
        if (result.error == AdapterError::none && result.target.secure) {
            fail_adapter(completed, AdapterError::insertion_unavailable);
            return;
        }
        if (result.error == AdapterError::none) {
            original_target_ = result.target.token;
            original_fallback_reason_ = WorkflowFallbackReason::target_unavailable;
        } else {
            original_target_ = {};
            original_fallback_reason_ = result.error == AdapterError::insertion_unavailable
                ? WorkflowFallbackReason::accessibility_unavailable
                : result.error == AdapterError::target_changed
                    ? WorkflowFallbackReason::target_changed
                    : WorkflowFallbackReason::target_unavailable;
        }
        request_models(completed, Step::first_models);
    });
}

void WorkflowCore::request_models(OperationId operation, Step gate) {
    if (Protocol::validate_config(protocol_config_) != LV_PROTOCOL_OK) {
        fail_protocol(operation, LV_PROTOCOL_CONFIGURATION, 0);
        return;
    }
    TransportRequest request;
    const auto status = Protocol::make_models_request(protocol_config_, request);
    if (status != LV_PROTOCOL_OK) {
        fail_protocol(operation, status, 0);
        return;
    }

    const auto step = begin_step(gate);
    transport_.send(operation, std::move(request), [this, step, gate](OperationId completed, TransportResponse response) {
        if (!consume(completed, step, gate)) return;
        if (response.error == AdapterError::canceled) {
            finish_canceled(completed, true);
            return;
        }
        if (response.error != AdapterError::none) {
            fail_adapter(completed, response.error);
            return;
        }
        const auto http = Protocol::classify_http(response.status, response.bytes);
        if (http.status != LV_PROTOCOL_OK) {
            fail_protocol(completed, http.status, http.http_status, std::move(http.value));
            return;
        }
        const auto models = Protocol::validate_models(response.bytes, protocol_config_);
        if (models != LV_PROTOCOL_OK) {
            fail_protocol(completed, models, response.status);
            return;
        }
        if (gate == Step::first_models) request_capture_start(completed);
        else request_transcription(completed);
    });
}

void WorkflowCore::request_capture_start(OperationId operation) {
    const auto step = begin_step(Step::capture_start);
    capture_.start(operation, [this, step](OperationId completed, Completion result) {
        if (!consume(completed, step, Step::capture_start)) return;
        if (result.error == AdapterError::canceled) {
            finish_canceled(completed, true);
            return;
        }
        if (result.error != AdapterError::none) {
            fail_adapter(completed, result.error);
            return;
        }
        diagnostics_.record({DiagnosticEvent::recording_started, AdapterError::none, 0});
        set_phase(completed, WorkflowPhase::recording);
    });
}

void WorkflowCore::request_capture_stop(OperationId operation) {
    const auto step = begin_step(Step::capture_stop);
    capture_.stop(operation, [this, step](OperationId completed, BytesResult result) {
        if (!consume(completed, step, Step::capture_stop)) return;
        if (result.error == AdapterError::canceled) {
            finish_canceled(completed, true);
            return;
        }
        if (result.error != AdapterError::none) {
            fail_adapter(completed, result.error);
            return;
        }
        if (result.bytes.empty()) {
            fail_protocol(completed, LV_PROTOCOL_RECORDING, 0);
            return;
        }
        audio_ = std::move(result.bytes);
        diagnostics_.record({DiagnosticEvent::transcription_started, AdapterError::none, 0});
        request_models(completed, Step::second_models);
    });
}

void WorkflowCore::request_transcription(OperationId operation) {
    TransportRequest request;
    const auto status = Protocol::make_transcription_request(protocol_config_, audio_, multipart_boundary_, request);
    audio_.clear();
    multipart_boundary_.clear();
    if (status != LV_PROTOCOL_OK) {
        fail_protocol(operation, status, 0);
        return;
    }

    const auto step = begin_step(Step::transcription);
    transport_.send(operation, std::move(request), [this, step](OperationId completed, TransportResponse response) {
        if (!consume(completed, step, Step::transcription)) return;
        if (response.error == AdapterError::canceled) {
            finish_canceled(completed, true);
            return;
        }
        if (response.error != AdapterError::none) {
            fail_adapter(completed, response.error);
            return;
        }
        const auto http = Protocol::classify_http(response.status, response.bytes);
        if (http.status != LV_PROTOCOL_OK) {
            fail_protocol(completed, http.status, http.http_status, std::move(http.value));
            return;
        }
        auto transcript = Protocol::parse_transcription(response.bytes);
        if (transcript.status != LV_PROTOCOL_OK) {
            fail_protocol(completed, transcript.status, response.status);
            return;
        }
        TransportRequest cleanup;
        const auto status = Protocol::make_cleanup_request(protocol_config_, transcript.value, cleanup);
        if (status != LV_PROTOCOL_OK) {
            fail_protocol(completed, status, response.status);
            return;
        }
        request_cleanup(completed, std::move(cleanup));
    });
}

void WorkflowCore::request_cleanup(OperationId operation, TransportRequest request) {
    diagnostics_.record({DiagnosticEvent::cleanup_started, AdapterError::none, 0});
    set_phase(operation, WorkflowPhase::cleaning);
    if (active_operation_ != operation || shutting_down_) return;

    const auto step = begin_step(Step::cleanup);
    transport_.send(operation, std::move(request), [this, step](OperationId completed, TransportResponse response) {
        if (!consume(completed, step, Step::cleanup)) return;
        if (response.error == AdapterError::canceled) {
            finish_canceled(completed, true);
            return;
        }
        if (response.error != AdapterError::none) {
            fail_adapter(completed, response.error);
            return;
        }
        const auto http = Protocol::classify_http(response.status, response.bytes);
        if (http.status != LV_PROTOCOL_OK) {
            fail_protocol(completed, http.status, http.http_status, std::move(http.value));
            return;
        }
        auto cleaned = Protocol::parse_cleanup(response.bytes);
        if (cleaned.status != LV_PROTOCOL_OK) {
            fail_protocol(completed, cleaned.status, response.status);
            return;
        }
        request_insertion(completed, std::move(cleaned.value));
    });
}

void WorkflowCore::request_insertion(OperationId operation, std::string text) {
    result_text_ = std::move(text);
    set_phase(operation, WorkflowPhase::inserting);
    if (active_operation_ != operation || shutting_down_) return;
    if (!config_.preferences.insert_current_target && original_target_.value == 0) {
        fallback_to_text(operation, original_fallback_reason_);
        return;
    }

    const auto step = begin_step(Step::inspect_target);
    insertion_.inspect(operation, original_target_, [this, step](OperationId completed, TargetResult result) {
        if (!consume(completed, step, Step::inspect_target)) return;
        if (result.error == AdapterError::canceled) {
            finish_canceled(completed, true);
            return;
        }
        if (result.error == AdapterError::target_changed) {
            fallback_to_text(completed, WorkflowFallbackReason::target_changed);
            return;
        }
        if (result.error == AdapterError::insertion_unavailable) {
            // Inspection itself is unavailable, so this is the same
            // Accessibility path as an unavailable original-target capture.
            // paste_unavailable is reserved for a dispatched insert that
            // could not complete.
            fallback_to_text(completed, WorkflowFallbackReason::accessibility_unavailable);
            return;
        }
        if (result.error != AdapterError::none) {
            fallback_to_text(completed, WorkflowFallbackReason::target_unavailable);
            return;
        }
        LVInsertionFacts facts{};
        facts.current_target = config_.preferences.insert_current_target ? 1 : 0;
        facts.original_matches = result.target.matches_original ? 1 : 0;
        facts.frontmost_matches = result.target.frontmost ? 1 : 0;
        facts.secure = result.target.secure ? 1 : 0;
        if (result.target.secure) {
            fallback_to_text(completed, WorkflowFallbackReason::secure_field);
            return;
        }
        if (!result.target.editable) {
            fallback_to_text(completed, WorkflowFallbackReason::unsupported_field);
            return;
        }
        if (result.target.token.value == 0) {
            fallback_to_text(completed, WorkflowFallbackReason::target_unavailable);
            return;
        }
        if (!lv_can_insert(&facts)) {
            fallback_to_text(completed, WorkflowFallbackReason::target_changed);
            return;
        }
        request_insert(completed, result.target.token);
    });
}

void WorkflowCore::request_insert(OperationId operation, TargetToken target) {
    const auto step = begin_step(Step::insert);
    insertion_.insert(operation, target, result_text_, [this, step](OperationId completed, Completion result) {
        if (!consume(completed, step, Step::insert)) return;
        if (result.error == AdapterError::none) {
            finish_success(completed);
            return;
        }
        if (result.error == AdapterError::canceled) {
            finish_canceled(completed, true);
            return;
        }
        const auto reason = result.error == AdapterError::target_changed
            ? WorkflowFallbackReason::target_changed
            : WorkflowFallbackReason::paste_unavailable;
        fallback_to_text(completed, reason);
    });
}

void WorkflowCore::fallback_to_text(OperationId operation, WorkflowFallbackReason reason) {
    if (active_operation_ != operation || shutting_down_) return;
    auto text = std::move(result_text_);
    invalidate_active();
    diagnostics_.record({DiagnosticEvent::insertion_fallback, AdapterError::none, 0});
    delivering_terminal_ = true;
    set_phase(operation, WorkflowPhase::idle);
    if (!shutting_down_ && callbacks_.text_ready) callbacks_.text_ready(operation, std::move(text));
    if (!shutting_down_ && callbacks_.completed) {
        callbacks_.completed(operation, {WorkflowOutcomeKind::copy_fallback, reason});
    }
    if (!shutting_down_) delivering_terminal_ = false;
}

void WorkflowCore::finish_success(OperationId operation) {
    if (active_operation_ != operation || shutting_down_) return;
    invalidate_active();
    diagnostics_.record({DiagnosticEvent::insertion_succeeded, AdapterError::none, 0});
    delivering_terminal_ = true;
    set_phase(operation, WorkflowPhase::idle);
    if (!shutting_down_ && callbacks_.completed) {
        callbacks_.completed(operation, {WorkflowOutcomeKind::inserted, WorkflowFallbackReason::none});
    }
    if (!shutting_down_) delivering_terminal_ = false;
}

void WorkflowCore::finish_canceled(OperationId operation, bool notify) {
    if (operation == 0 || active_operation_ != operation || shutting_down_) return;
    invalidate_active();
    cancel_native(operation);
    diagnostics_.record({DiagnosticEvent::canceled, AdapterError::canceled, 0});
    if (notify && !shutting_down_) {
        delivering_terminal_ = true;
        set_phase(operation, WorkflowPhase::idle);
        if (!shutting_down_) delivering_terminal_ = false;
    }
}

void WorkflowCore::fail_protocol(OperationId operation, LVProtocolStatus status, int http_status,
    std::string safe_detail) {
    if (active_operation_ != operation || shutting_down_) return;
    const auto reported_http_status = terminal_http_status(status, http_status);
    invalidate_active();
    cancel_native(operation);
    diagnostics_.record({DiagnosticEvent::dictation_failed, AdapterError::response, reported_http_status});
    delivering_terminal_ = true;
    set_phase(operation, WorkflowPhase::idle);
    if (!shutting_down_ && callbacks_.failed) {
        callbacks_.failed(operation, {WorkflowFailureSource::protocol, AdapterError::none,
            status, reported_http_status, std::move(safe_detail)});
    }
    if (!shutting_down_) delivering_terminal_ = false;
}

void WorkflowCore::fail_adapter(OperationId operation, AdapterError error) {
    if (active_operation_ != operation || shutting_down_) return;
    invalidate_active();
    cancel_native(operation);
    diagnostics_.record({DiagnosticEvent::dictation_failed, error, 0});
    delivering_terminal_ = true;
    set_phase(operation, WorkflowPhase::idle);
    if (!shutting_down_ && callbacks_.failed) {
        callbacks_.failed(operation, {WorkflowFailureSource::adapter, error,
            LV_PROTOCOL_OK, 0, {}});
    }
    if (!shutting_down_) delivering_terminal_ = false;
}

void WorkflowCore::invalidate_active() {
    active_operation_ = 0;
    pending_step_ = 0;
    step_ = Step::none;
    original_target_ = {};
    original_fallback_reason_ = WorkflowFallbackReason::target_unavailable;
    audio_.clear();
    multipart_boundary_.clear();
    result_text_.clear();
    config_ = {};
    protocol_config_ = {};
    phase_ = WorkflowPhase::idle;
}

void WorkflowCore::cancel_native(OperationId operation) {
    capture_.cancel(operation);
    transport_.cancel(operation);
    insertion_.cancel(operation);
}

void WorkflowCore::set_phase(OperationId operation, WorkflowPhase phase) {
    phase_ = phase;
    if (callbacks_.phase_changed) callbacks_.phase_changed(operation, phase);
}

} // namespace lemonvox
