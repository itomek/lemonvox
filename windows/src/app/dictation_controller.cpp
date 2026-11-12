#include "app/dictation_controller.h"

#include "common/utf.h"
#include "net/endpoint_facts.h"

#include <objbase.h>

#include <cstdio>
#include <utility>

namespace lemonvox::win {

namespace {

const wchar_t *event_name(DiagnosticEvent event) {
    switch (event) {
    case DiagnosticEvent::recording_started: return L"recording_started";
    case DiagnosticEvent::transcription_started: return L"transcription_started";
    case DiagnosticEvent::cleanup_started: return L"cleanup_started";
    case DiagnosticEvent::insertion_fallback: return L"insertion_fallback";
    case DiagnosticEvent::transport_failed: return L"transport_failed";
    case DiagnosticEvent::canceled: return L"canceled";
    case DiagnosticEvent::insertion_succeeded: return L"insertion_succeeded";
    case DiagnosticEvent::dictation_failed: return L"dictation_failed";
    }
    return L"unknown";
}

const wchar_t *error_name(AdapterError error) {
    switch (error) {
    case AdapterError::none: return L"none";
    case AdapterError::canceled: return L"canceled";
    case AdapterError::unavailable: return L"unavailable";
    case AdapterError::invalid_input: return L"invalid_input";
    case AdapterError::network: return L"network";
    case AdapterError::response: return L"response";
    case AdapterError::target_changed: return L"target_changed";
    case AdapterError::insertion_unavailable: return L"insertion_unavailable";
    }
    return L"unknown";
}

UserMessage capture_message(CaptureProblem problem) {
    switch (problem) {
    case CaptureProblem::permission_denied:
        return {L"Allow microphone access in Settings > Privacy & security > Microphone",
            FailureAction::open_microphone_privacy};
    case CaptureProblem::no_device:
        return {L"No microphone found; connect one or choose another in Settings", FailureAction::open_settings};
    case CaptureProblem::device_busy:
        return {L"Microphone is in use by another app", FailureAction::none};
    case CaptureProblem::device_lost:
        return {L"Microphone disconnected or changed; try again", FailureAction::none};
    case CaptureProblem::other:
    case CaptureProblem::none:
        break;
    }
    return {L"Couldn't start the microphone", FailureAction::open_settings};
}

} // namespace

void DiagnosticsRing::record(DiagnosticEntry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.size() == kCapacity) entries_.pop_front();
    entries_.push_back(entry);
}

std::vector<DiagnosticEntry> DiagnosticsRing::entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {entries_.begin(), entries_.end()};
}

std::wstring DiagnosticsRing::format() const {
    std::wstring text;
    for (const auto &entry : entries()) {
        text += event_name(entry.event);
        text += L" error=";
        text += error_name(entry.error);
        text += L" http=" + std::to_wstring(entry.http_status) + L"\r\n";
    }
    return text;
}

UserMessage describe_failure(const WorkflowFailure &failure, CaptureProblem capture) {
    if (failure.source == WorkflowFailureSource::adapter) {
        switch (failure.adapter_error) {
        case AdapterError::network:
            return {L"Lemonade unavailable; check that it is running at the configured endpoint",
                FailureAction::open_settings};
        case AdapterError::insertion_unavailable:
            return {L"Paste unavailable \u00B7 try again or copy the text", FailureAction::none};
        case AdapterError::unavailable:
        case AdapterError::invalid_input:
            return capture_message(capture);
        case AdapterError::response:
            return {L"Lemonade returned an unsupported response", FailureAction::none};
        case AdapterError::target_changed:
            return {L"The target field changed; dictation stopped", FailureAction::none};
        case AdapterError::none:
        case AdapterError::canceled:
            break;
        }
        return {L"Dictation failed; try again", FailureAction::none};
    }
    switch (failure.protocol_status) {
    case LV_PROTOCOL_CONFIGURATION:
        return {L"Configure a loopback Lemonade endpoint and both models in Settings", FailureAction::open_settings};
    case LV_PROTOCOL_MODEL_UNAVAILABLE:
        return {L"Selected models are not available for transcription and cleanup through Lemonade",
            FailureAction::open_settings};
    case LV_PROTOCOL_EMPTY_TRANSCRIPT:
        return {L"No transcript returned; try another recording", FailureAction::none};
    case LV_PROTOCOL_EMPTY_CLEANUP:
        return {L"Cleanup returned no text; try again", FailureAction::none};
    case LV_PROTOCOL_HTTP:
        return {L"Lemonade request failed (HTTP " + std::to_wstring(failure.http_status) + L")", FailureAction::none};
    case LV_PROTOCOL_UPSTREAM: {
        std::wstring text = L"Lemonade HTTP " + std::to_wstring(failure.http_status);
        const std::wstring detail = utf8_to_wide(failure.safe_detail);
        if (!detail.empty()) text += L": " + detail;
        return {text, FailureAction::none};
    }
    case LV_PROTOCOL_RECORDING:
        if (capture != CaptureProblem::none) return capture_message(capture);
        return {L"Recording unavailable or empty; check microphone input", FailureAction::none};
    case LV_PROTOCOL_RESPONSE:
    case LV_PROTOCOL_OK:
        break;
    }
    return {L"Lemonade returned an unsupported response", FailureAction::none};
}

std::wstring describe_fallback(WorkflowFallbackReason) {
    return L"Paste unavailable \u00B7 copy to paste manually";
}

std::string make_multipart_boundary() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return {};
    char text[64];
    std::snprintf(text, sizeof(text), "LemonVox-%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2],
        guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return text;
}

DictationController::DictationController(CaptureAdapter &capture,
    ByteTransport &transport, InsertionAdapter &insertion, const PreferencesAdapter &preferences,
    ControllerEvents events, std::function<CaptureProblem()> capture_problem)
    : preferences_(preferences), events_(std::move(events)),
      capture_problem_(std::move(capture_problem)) {
    WorkflowCoreCallbacks callbacks;
    callbacks.phase_changed = [this](OperationId operation, WorkflowPhase phase) {
        if (!accept(operation)) return;
        if (events_.phase) events_.phase(phase);
    };
    callbacks.text_ready = [this](OperationId operation, std::string text) {
        if (accept(operation)) staged_text_ = std::move(text);
    };
    callbacks.failed = [this](OperationId operation, WorkflowFailure failure) {
        if (!accept(operation)) return;
        active_ = 0;
        const CaptureProblem problem = capture_problem_ ? capture_problem_() : CaptureProblem::none;
        if (events_.failed) events_.failed(describe_failure(failure, problem));
        if (events_.finished) events_.finished(operation);
    };
    callbacks.completed = [this](OperationId operation, WorkflowOutcome outcome) {
        if (!accept(operation)) return;
        active_ = 0;
        if (outcome.kind == WorkflowOutcomeKind::inserted) {
            staged_text_.clear();
            if (events_.inserted) events_.inserted();
            if (events_.finished) events_.finished(operation);
            return;
        }
        pending_text_ = std::move(staged_text_);
        staged_text_.clear();
        if (events_.copy_fallback) events_.copy_fallback(describe_fallback(outcome.fallback_reason));
        if (events_.finished) events_.finished(operation);
    };
    core_ = std::make_unique<WorkflowCore>(capture, transport, insertion, diagnostics_, std::move(callbacks));
}

DictationController::~DictationController() {
    shutdown();
}

void DictationController::toggle() {
    if (!core_ || core_->is_shutdown()) return;
    if (core_->phase() == WorkflowPhase::recording) {
        stop();
        return;
    }
    if (!busy()) start();
}

bool DictationController::start() {
    if (!core_ || busy()) return false;
    discard_pending();
    WorkflowConfig config;
    config.preferences = preferences_.snapshot();
    // Malformed text leaves the facts invalid; the core reports a configuration failure.
    endpoint_facts(config.preferences.endpoint, config.endpoint);
    // The core may deliver phase or validation-failure callbacks before start()
    // returns the new ID, so accept() adopts the first ID seen in this window.
    active_ = 0;
    starting_ = true;
    const OperationId operation = core_->start(std::move(config));
    starting_ = false;
    return operation != 0;
}

bool DictationController::accept(OperationId operation) {
    if (operation == 0) return false;
    if (starting_ && active_ == 0) active_ = operation;
    return operation == active_;
}

bool DictationController::stop() {
    if (!core_ || core_->phase() != WorkflowPhase::recording) return false;
    return core_->stop(make_multipart_boundary());
}

void DictationController::cancel() {
    if (core_) core_->cancel();
    active_ = 0;
    staged_text_.clear();
}

void DictationController::capture_failed() {
    if (core_ && core_->phase() == WorkflowPhase::recording) core_->stop(make_multipart_boundary());
}

void DictationController::shutdown() {
    if (core_) core_->shutdown();
    active_ = 0;
    staged_text_.clear();
}

WorkflowPhase DictationController::phase() const {
    return core_ ? core_->phase() : WorkflowPhase::idle;
}

bool DictationController::busy() const {
    return core_ && core_->phase() != WorkflowPhase::idle;
}

bool DictationController::copy_pending(const std::function<bool(const std::string &)> &copy) {
    if (pending_text_.empty() || !copy || !copy(pending_text_)) return false;
    discard_pending();
    return true;
}

void DictationController::discard_pending() {
    // Overwrite before release so retained dictation does not linger in freed memory.
    for (auto &character : pending_text_) character = '\0';
    pending_text_.clear();
}

} // namespace lemonvox::win
