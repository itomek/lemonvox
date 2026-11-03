// Controller binding tests with fake adapters; no network, device or desktop input.
#include "app/dictation_controller.h"
#include "common/executor.h"

#include <windows.h>

#include <cassert>
#include <string>
#include <vector>

using namespace lemonvox;
using namespace lemonvox::win;

namespace {

void pump_until(const bool &done) {
    const ULONGLONG deadline = GetTickCount64() + 5000;
    MSG message;
    while (!done && GetTickCount64() < deadline) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        if (!done) MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
    }
}

struct FakeCapture final : CaptureAdapter {
    Executor &executor;
    std::vector<std::uint8_t> wav;
    AdapterError start_error = AdapterError::none;
    explicit FakeCapture(Executor &e) : executor(e) {}
    void start(OperationId op, VoidCompletion done) override {
        executor.post([op, done, error = start_error] { done(op, {error}); });
    }
    void stop(OperationId op, BytesCompletion done) override {
        executor.post([op, done, bytes = wav] { done(op, {AdapterError::none, bytes}); });
    }
    void cancel(OperationId) override {}
    void shutdown() override {}
};

// Serves the model inventory and fixed transcription/cleanup responses.
struct FakeTransport final : ByteTransport {
    Executor &executor;
    std::vector<std::string> paths;
    bool network_down = false;
    explicit FakeTransport(Executor &e) : executor(e) {}
    void send(OperationId op, TransportRequest request, TransportCompletion done) override {
        paths.push_back(request.url);
        TransportResponse response;
        if (network_down) {
            response.error = AdapterError::network;
        } else {
            response.status = 200;
            std::string body;
            if (request.url.find("/models") != std::string::npos) {
                body = R"({"object":"list","data":[)"
                    R"({"id":"asr","object":"model","owned_by":"lemonade","recipe":"whispercpp","downloaded":true,"labels":["transcription"]},)"
                    R"({"id":"llm","object":"model","owned_by":"lemonade","recipe":"llamacpp","downloaded":true,"labels":["chat"]}]})";
            } else if (request.url.find("/audio/transcriptions") != std::string::npos) {
                body = R"({"text":"um hello world"})";
            } else {
                body = R"({"choices":[{"message":{"role":"assistant","content":"Hello world."},"finish_reason":"stop"}]})";
            }
            response.bytes.assign(body.begin(), body.end());
        }
        executor.post([op, done, response] { done(op, response); });
    }
    void cancel(OperationId) override {}
    void shutdown() override {}
};

struct FakeInsertion final : InsertionAdapter {
    Executor &executor;
    bool matches = true;
    bool secure_at_start = false;
    std::string inserted;
    explicit FakeInsertion(Executor &e) : executor(e) {}
    void capture_original(OperationId op, TargetCompletion done) override {
        TargetResult result;
        result.target = {{7}, true, secure_at_start, true, true};
        executor.post([op, done, result] { done(op, result); });
    }
    void inspect(OperationId op, TargetToken, TargetCompletion done) override {
        TargetResult result;
        result.target = {{7}, true, false, matches, matches};
        executor.post([op, done, result] { done(op, result); });
    }
    void insert(OperationId op, TargetToken, std::string text, VoidCompletion done) override {
        inserted = text;
        executor.post([op, done] { done(op, {}); });
    }
    void cancel(OperationId) override {}
    void shutdown() override {}
};

WinPreferences configured() {
    WinPreferences preferences;
    preferences.asr_model = "asr";
    preferences.cleanup_model = "llm";
    return preferences;
}

struct Harness {
    UiThreadExecutor executor;
    FakeCapture capture{executor};
    FakeTransport transport{executor};
    FakeInsertion insertion{executor};
    SnapshotPreferences preferences{configured()};
    std::vector<WorkflowPhase> phases;
    UserMessage failure;
    bool failed = false;
    bool inserted = false;
    std::wstring fallback;
    CaptureProblem problem = CaptureProblem::none;
    DictationController controller{capture, transport, insertion, preferences, events(),
        [this] { return problem; }};

    Harness() { capture.wav = std::vector<std::uint8_t>(64, 1); }

    ControllerEvents events() {
        ControllerEvents result;
        result.phase = [this](WorkflowPhase phase) { phases.push_back(phase); };
        result.failed = [this](const UserMessage &message) { failure = message; failed = true; };
        result.inserted = [this] { inserted = true; };
        result.copy_fallback = [this](const std::wstring &reason) { fallback = reason; };
        return result;
    }

    void wait_for_phase(WorkflowPhase phase) {
        bool reached = false;
        const ULONGLONG deadline = GetTickCount64() + 5000;
        MSG message;
        while (!reached && GetTickCount64() < deadline) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
            reached = controller.phase() == phase;
            if (!reached) Sleep(2);
        }
        assert(reached);
    }
};

void full_dictation_inserts_once() {
    Harness h;
    h.controller.toggle();
    h.wait_for_phase(WorkflowPhase::recording);
    h.controller.toggle();
    pump_until(h.inserted);
    assert(h.inserted && !h.failed);
    assert(h.insertion.inserted == "Hello world.");
    assert(h.phases.front() == WorkflowPhase::preparing && h.phases.back() == WorkflowPhase::idle);
    assert(!h.controller.has_pending_text());
}

void changed_target_offers_copy() {
    Harness h;
    h.insertion.matches = false;
    h.controller.toggle();
    h.wait_for_phase(WorkflowPhase::recording);
    h.controller.toggle();
    const ULONGLONG deadline = GetTickCount64() + 5000;
    MSG message;
    while (h.fallback.empty() && GetTickCount64() < deadline) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Sleep(2);
    }
    assert(!h.fallback.empty() && h.insertion.inserted.empty());
    assert(h.controller.has_pending_text());
    std::string copied;
    assert(h.controller.copy_pending([&](const std::string &text) { copied = text; return true; }));
    assert(copied == "Hello world." && !h.controller.has_pending_text());
}

void invalid_configuration_fails_synchronously_with_settings_action() {
    Harness h;
    WinPreferences broken = configured();
    broken.endpoint = "http://example.com/v1";
    h.preferences.replace(broken);
    h.controller.toggle();
    assert(h.failed && h.failure.action == FailureAction::open_settings);
    assert(h.controller.phase() == WorkflowPhase::idle && !h.controller.busy());
    assert(h.transport.paths.empty());
}

void unreachable_server_reports_lemonade_unavailable() {
    Harness h;
    h.transport.network_down = true;
    h.controller.toggle();
    pump_until(h.failed);
    assert(h.failed && h.failure.text.find(L"Lemonade unavailable") == 0);
}

void microphone_denial_maps_to_privacy_action() {
    Harness h;
    h.capture.start_error = AdapterError::unavailable;
    h.problem = CaptureProblem::permission_denied;
    h.controller.toggle();
    pump_until(h.failed);
    assert(h.failed && h.failure.action == FailureAction::open_microphone_privacy);
}

void secure_field_refuses_dictation() {
    Harness h;
    h.insertion.secure_at_start = true;
    h.controller.toggle();
    pump_until(h.failed);
    assert(h.failed && h.failure.text.find(L"Password field") == 0);
    assert(h.transport.paths.empty());
}

void cancel_discards_late_results() {
    Harness h;
    h.controller.toggle();
    h.wait_for_phase(WorkflowPhase::recording);
    h.controller.toggle();
    h.controller.cancel();
    const ULONGLONG deadline = GetTickCount64() + 300;
    MSG message;
    while (GetTickCount64() < deadline) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Sleep(2);
    }
    assert(!h.inserted && h.insertion.inserted.empty() && !h.failed);
    assert(h.controller.phase() == WorkflowPhase::idle);
    // A new operation can start after cancellation.
    h.controller.toggle();
    h.wait_for_phase(WorkflowPhase::recording);
    h.controller.cancel();
}

void diagnostics_stay_content_free() {
    Harness h;
    h.controller.toggle();
    h.wait_for_phase(WorkflowPhase::recording);
    h.controller.toggle();
    pump_until(h.inserted);
    const std::wstring log = h.controller.diagnostics().format();
    assert(log.find(L"insertion_succeeded") != std::wstring::npos);
    assert(log.find(L"Hello") == std::wstring::npos && log.find(L"127.0.0.1") == std::wstring::npos);
}

void boundary_is_unique_and_prefixed() {
    const std::string a = make_multipart_boundary();
    const std::string b = make_multipart_boundary();
    assert(a.rfind("LemonVox-", 0) == 0 && a.size() == 45 && a != b);
}

} // namespace

int main() {
    full_dictation_inserts_once();
    changed_target_offers_copy();
    invalid_configuration_fails_synchronously_with_settings_action();
    unreachable_server_reports_lemonade_unavailable();
    microphone_denial_maps_to_privacy_action();
    secure_field_refuses_dictation();
    cancel_discards_late_results();
    diagnostics_stay_content_free();
    boundary_is_unique_and_prefixed();
    return 0;
}
