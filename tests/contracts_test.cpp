#include "adapters.h"

#include <cassert>
#include <cstdio>
#include <utility>

using namespace lemonvox;

namespace {

class FakeCapture final : public CaptureAdapter {
public:
    void start(OperationId operation, VoidCompletion completion) override {
        completion(operation, {AdapterError::none});
    }

    void stop(OperationId operation, BytesCompletion completion) override {
        BytesResult result;
        result.bytes = {0x52, 0x49, 0x46, 0x46};
        completion(operation, std::move(result));
    }

    void cancel(OperationId operation) override { canceled = operation; }
    void shutdown() override { shut_down = true; }

    OperationId canceled = 0;
    bool shut_down = false;
};

class FakeTransport final : public ByteTransport {
public:
    void send(OperationId operation, TransportRequest request,
        TransportCompletion completion) override {
        assert(request.method == HttpMethod::post);
        assert(request.url == "http://127.0.0.1:13305/v1/models");
        assert(request.body == std::vector<std::uint8_t>({1, 2, 3}));
        TransportResponse response;
        response.status = 200;
        response.bytes = {'{', '}'};
        completion(operation, std::move(response));
    }

    void cancel(OperationId operation) override { canceled = operation; }
    void shutdown() override { shut_down = true; }

    OperationId canceled = 0;
    bool shut_down = false;
};

class FakeInsertion final : public InsertionAdapter {
public:
    void capture_original(OperationId operation, TargetCompletion completion) override {
        TargetResult result;
        result.target = {{42}, true, false, true, true};
        completion(operation, std::move(result));
    }

    void inspect(OperationId operation, TargetToken original, TargetCompletion completion) override {
        assert(original.value == 42);
        TargetResult result;
        result.target = {{43}, true, false, true, true};
        completion(operation, std::move(result));
    }

    void insert(OperationId operation, TargetToken target, std::string text,
        VoidCompletion completion) override {
        assert(target.value == 43);
        assert(text == "cleaned text");
        completion(operation, {AdapterError::none});
    }

    void cancel(OperationId operation) override { canceled = operation; invalidated = true; }
    void shutdown() override { shut_down = true; invalidated = true; }

    OperationId canceled = 0;
    bool shut_down = false;
    bool invalidated = false;
};

class FakePreferences final : public PreferencesAdapter {
public:
    PreferencesSnapshot snapshot() const override {
        return {"http://127.0.0.1:13305/v1", "asr", "cleanup", "prompt", false};
    }
};

class FakeDiagnostics final : public DiagnosticsSink {
public:
    void record(DiagnosticEntry entry) override { last = entry; }
    DiagnosticEntry last{};
};

} // namespace

int main() {
    constexpr OperationId operation = 7;
    FakeCapture capture;
    bool started = false;
    capture.start(operation, [&started](OperationId completed, Completion result) {
        assert(completed == operation && result.error == AdapterError::none);
        started = true;
    });
    assert(started);

    bool captured = false;
    capture.stop(operation, [&captured](OperationId completed, BytesResult result) {
        assert(completed == operation && result.error == AdapterError::none);
        assert(result.bytes == std::vector<std::uint8_t>({0x52, 0x49, 0x46, 0x46}));
        captured = true;
    });
    assert(captured);

    FakeTransport transport;
    TransportRequest request;
    request.method = HttpMethod::post;
    request.url = "http://127.0.0.1:13305/v1/models";
    request.body = {1, 2, 3};
    bool received = false;
    transport.send(operation, std::move(request), [&received](OperationId completed, TransportResponse response) {
        assert(completed == operation && response.error == AdapterError::none);
        assert(response.status == 200 && response.bytes == std::vector<std::uint8_t>({'{', '}'}));
        received = true;
    });
    assert(received);

    FakeInsertion insertion;
    bool inspected = false;
    TargetToken original;
    insertion.capture_original(operation, [&original](OperationId completed, TargetResult result) {
        assert(completed == operation && result.error == AdapterError::none);
        original = result.target.token;
    });
    insertion.inspect(operation, original, [&insertion, &inspected](OperationId completed, TargetResult result) {
        assert(completed == operation && result.error == AdapterError::none);
        assert(result.target.editable && !result.target.secure && result.target.frontmost && result.target.matches_original);
        insertion.insert(completed, result.target.token, "cleaned text", [&inspected](OperationId inserted, Completion completion) {
            assert(inserted == operation && completion.error == AdapterError::none);
            inspected = true;
        });
    });
    assert(inspected);

    FakePreferences preferences;
    const auto snapshot = preferences.snapshot();
    assert(snapshot.endpoint == "http://127.0.0.1:13305/v1" && snapshot.asr_model == "asr");

    FakeDiagnostics diagnostics;
    diagnostics.record({DiagnosticEvent::transport_failed, AdapterError::network, 503});
    assert(diagnostics.last.event == DiagnosticEvent::transport_failed && diagnostics.last.http_status == 503);

    WorkflowCallbacks callbacks;
    bool phase_notified = false;
    callbacks.phase_changed = [&phase_notified](OperationId changed, WorkflowPhase phase) {
        assert(changed == operation && phase == WorkflowPhase::transcribing);
        phase_notified = true;
    };
    callbacks.phase_changed(operation, WorkflowPhase::transcribing);
    assert(phase_notified);

    capture.cancel(operation);
    transport.cancel(operation);
    insertion.cancel(operation);
    capture.shutdown();
    transport.shutdown();
    insertion.shutdown();
    assert(capture.canceled == operation && transport.canceled == operation && insertion.canceled == operation);
    assert(capture.shut_down && transport.shut_down && insertion.shut_down && insertion.invalidated);
    std::puts("PASS: portable adapter contracts own values, identify operations and isolate native behavior");
}
