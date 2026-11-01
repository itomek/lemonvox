#include "workflow.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace lemonvox;

namespace {

const char *kValidInventory = R"({"data":[{"id":"asr","downloaded":true,"recipe":"whispercpp","labels":["transcription"]},{"id":"cleanup","downloaded":true,"recipe":"llamacpp","labels":["chat"]}]})";

struct FakeCapture final : CaptureAdapter {
    struct PendingStart {
        OperationId operation;
        VoidCompletion completion;
    };

    struct PendingStop {
        OperationId operation;
        BytesCompletion completion;
    };

    void start(OperationId operation, VoidCompletion completion) override {
        starts.push_back(operation);
        pending_starts.push_back({operation, std::move(completion)});
        if (auto_start) complete_start(pending_starts.size() - 1, {AdapterError::none});
    }

    void stop(OperationId operation, BytesCompletion completion) override {
        stops.push_back(operation);
        pending_stops.push_back({operation, std::move(completion)});
        if (auto_stop) complete_stop(pending_stops.size() - 1, auto_stop_result);
    }

    void cancel(OperationId operation) override { canceled.push_back(operation); }
    void shutdown() override { shut_down = true; }

    void complete_start(std::size_t index, Completion result) {
        const auto &pending = pending_starts.at(index);
        pending.completion(pending.operation, std::move(result));
    }

    void complete_stop(std::size_t index, BytesResult result) {
        const auto &pending = pending_stops.at(index);
        pending.completion(pending.operation, std::move(result));
    }

    std::vector<OperationId> starts;
    std::vector<OperationId> stops;
    std::vector<OperationId> canceled;
    std::vector<PendingStart> pending_starts;
    std::vector<PendingStop> pending_stops;
    bool auto_start = false;
    bool auto_stop = false;
    BytesResult auto_stop_result{AdapterError::none, {'R', 'I', 'F', 'F'}};
    bool shut_down = false;
};

struct FakeTransport final : ByteTransport {
    struct Pending {
        OperationId operation;
        TransportRequest request;
        TransportCompletion completion;
    };

    void send(OperationId operation, TransportRequest request,
        TransportCompletion completion) override {
        pending.push_back({operation, std::move(request), std::move(completion)});
        if (auto_complete) {
            assert(automatic_index < automatic_responses.size());
            complete(pending.size() - 1, automatic_responses[automatic_index++]);
        }
    }

    void cancel(OperationId operation) override { canceled.push_back(operation); }
    void shutdown() override { shut_down = true; }

    void complete(std::size_t index, TransportResponse response) {
        const auto &next = pending.at(index);
        next.completion(next.operation, std::move(response));
    }

    void complete_next(TransportResponse response) {
        complete(next_completion++, std::move(response));
    }

    std::vector<Pending> pending;
    std::vector<OperationId> canceled;
    std::size_t next_completion = 0;
    bool auto_complete = false;
    std::vector<TransportResponse> automatic_responses;
    std::size_t automatic_index = 0;
    bool shut_down = false;
};

struct FakeInsertion final : InsertionAdapter {
    struct PendingTarget {
        OperationId operation;
        TargetCompletion completion;
    };

    struct PendingInsert {
        OperationId operation;
        VoidCompletion completion;
    };

    void capture_original(OperationId operation, TargetCompletion completion) override {
        captured.push_back(operation);
        pending_original.push_back({operation, std::move(completion)});
        if (auto_original) complete_original(pending_original.size() - 1, original_result);
    }

    void inspect(OperationId operation, TargetToken original, TargetCompletion completion) override {
        inspected.push_back({operation, original});
        pending_inspect.push_back({operation, std::move(completion)});
        if (auto_inspect) complete_inspect(pending_inspect.size() - 1, inspect_result);
    }

    void insert(OperationId operation, TargetToken target, std::string text,
        VoidCompletion completion) override {
        inserted.push_back({operation, target, std::move(text)});
        pending_insert.push_back({operation, std::move(completion)});
        if (auto_insert) complete_insert(pending_insert.size() - 1, {AdapterError::none});
    }

    void cancel(OperationId operation) override { canceled.push_back(operation); }
    void shutdown() override { shut_down = true; }

    void complete_original(std::size_t index, TargetResult result) {
        const auto &pending = pending_original.at(index);
        pending.completion(pending.operation, std::move(result));
    }

    void complete_inspect(std::size_t index, TargetResult result) {
        const auto &pending = pending_inspect.at(index);
        pending.completion(pending.operation, std::move(result));
    }

    void complete_insert(std::size_t index, Completion result) {
        const auto &pending = pending_insert.at(index);
        pending.completion(pending.operation, std::move(result));
    }

    struct InspectCall { OperationId operation; TargetToken original; };
    struct InsertCall { OperationId operation; TargetToken target; std::string text; };
    std::vector<OperationId> captured;
    std::vector<InspectCall> inspected;
    std::vector<InsertCall> inserted;
    std::vector<OperationId> canceled;
    std::vector<PendingTarget> pending_original;
    std::vector<PendingTarget> pending_inspect;
    std::vector<PendingInsert> pending_insert;
    TargetResult original_result{AdapterError::none, {{41}, true, false, true, true}};
    TargetResult inspect_result{AdapterError::none, {{42}, true, false, true, true}};
    bool auto_original = true;
    bool auto_inspect = true;
    bool auto_insert = true;
    bool shut_down = false;
};

struct FakeDiagnostics final : DiagnosticsSink {
    void record(DiagnosticEntry entry) override { entries.push_back(entry); }
    std::vector<DiagnosticEntry> entries;
};

WorkflowConfig valid_config() {
    WorkflowConfig config;
    config.preferences = {"http://127.0.0.1:13305/v1", "asr", "cleanup", "prompt", false};
    config.endpoint = {"http://127.0.0.1:13305/v1", true, true, true, true, true};
    return config;
}

TransportResponse successful_inventory() {
    TransportResponse response;
    response.status = 200;
    response.bytes.assign(kValidInventory, kValidInventory + std::char_traits<char>::length(kValidInventory));
    return response;
}

TransportResponse successful_response(const char *json) {
    TransportResponse response;
    response.status = 200;
    response.bytes.assign(json, json + std::char_traits<char>::length(json));
    return response;
}

bool is_terminal_event(DiagnosticEvent event) {
    return event == DiagnosticEvent::insertion_succeeded || event == DiagnosticEvent::insertion_fallback ||
        event == DiagnosticEvent::canceled || event == DiagnosticEvent::dictation_failed;
}

std::size_t terminal_event_count(const std::vector<DiagnosticEntry> &entries) {
    std::size_t count = 0;
    for (const auto &entry : entries) count += is_terminal_event(entry.event) ? 1 : 0;
    return count;
}

BytesResult wav_bytes() {
    BytesResult result;
    result.bytes = {'R', 'I', 'F', 'F'};
    return result;
}

OperationId start_recording(WorkflowCore &workflow, FakeCapture &capture, FakeTransport &transport,
    WorkflowConfig config = valid_config()) {
    const auto operation = workflow.start(std::move(config));
    transport.complete_next(successful_inventory());
    capture.complete_start(capture.pending_starts.size() - 1, {AdapterError::none});
    assert(workflow.phase() == WorkflowPhase::recording);
    return operation;
}

void finish_with_cleanup(WorkflowCore &workflow, FakeCapture &capture, FakeTransport &transport,
    const std::string &boundary = "LemonVox-fixture") {
    assert(workflow.stop(boundary));
    capture.complete_stop(capture.pending_stops.size() - 1, wav_bytes());
    transport.complete_next(successful_inventory());
    transport.complete_next(successful_response(R"({"text":" Send Tomasz 14 invoices. "})"));
    transport.complete_next(successful_response(R"({"choices":[{"message":{"content":"Send Tomasz 14 invoices."},"finish_reason":"stop"}]})"));
}

enum class DelayedBoundary : std::size_t {
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

OperationId reach_delayed_boundary(DelayedBoundary boundary, WorkflowCore &workflow,
    FakeCapture &capture, FakeTransport &transport, FakeInsertion &insertion) {
    const auto operation = workflow.start(valid_config());
    if (boundary == DelayedBoundary::original_target) return operation;
    insertion.complete_original(0, insertion.original_result);
    if (boundary == DelayedBoundary::first_models) return operation;
    transport.complete(0, successful_inventory());
    if (boundary == DelayedBoundary::capture_start) return operation;
    capture.complete_start(0, {AdapterError::none});
    assert(workflow.stop("LemonVox-fixture"));
    if (boundary == DelayedBoundary::capture_stop) return operation;
    capture.complete_stop(0, wav_bytes());
    if (boundary == DelayedBoundary::second_models) return operation;
    transport.complete(1, successful_inventory());
    if (boundary == DelayedBoundary::transcription) return operation;
    transport.complete(2, successful_response(R"({"text":"Send Tomasz 14 invoices."})"));
    if (boundary == DelayedBoundary::cleanup) return operation;
    transport.complete(3, successful_response(R"({"choices":[{"message":{"content":"Send Tomasz 14 invoices."},"finish_reason":"stop"}]})"));
    if (boundary == DelayedBoundary::inspect_target) return operation;
    insertion.complete_inspect(0, insertion.inspect_result);
    assert(boundary == DelayedBoundary::insert);
    return operation;
}

void deliver_old_completion(DelayedBoundary boundary, FakeCapture &capture, FakeTransport &transport,
    FakeInsertion &insertion) {
    switch (boundary) {
    case DelayedBoundary::original_target:
        insertion.complete_original(0, insertion.original_result);
        return;
    case DelayedBoundary::first_models:
        transport.complete(0, successful_inventory());
        return;
    case DelayedBoundary::capture_start:
        capture.complete_start(0, {AdapterError::none});
        return;
    case DelayedBoundary::capture_stop:
        capture.complete_stop(0, wav_bytes());
        return;
    case DelayedBoundary::second_models:
        transport.complete(1, successful_inventory());
        return;
    case DelayedBoundary::transcription:
        transport.complete(2, successful_response(R"({"text":"old"})"));
        return;
    case DelayedBoundary::cleanup:
        transport.complete(3, successful_response(R"({"choices":[{"message":{"content":"old"},"finish_reason":"stop"}]})"));
        return;
    case DelayedBoundary::inspect_target:
        insertion.complete_inspect(0, insertion.inspect_result);
        return;
    case DelayedBoundary::insert:
        insertion.complete_insert(0, {AdapterError::none});
        return;
    }
}

std::size_t dispatched_effect_count(const FakeCapture &capture, const FakeTransport &transport,
    const FakeInsertion &insertion) {
    return capture.starts.size() + capture.stops.size() + transport.pending.size() +
        insertion.captured.size() + insertion.inspected.size() + insertion.inserted.size();
}

void start_validates_models_before_starting_capture() {
    FakeCapture capture;
    capture.auto_start = true;
    FakeTransport transport;
    FakeInsertion insertion;
    FakeDiagnostics diagnostics;
    WorkflowCore workflow(capture, transport, insertion, diagnostics, {});

    const auto operation = workflow.start(valid_config());

    assert(operation != 0);
    assert(workflow.phase() == WorkflowPhase::preparing);
    assert(capture.starts.empty());
    assert(transport.pending.size() == 1);
    assert(transport.pending.front().request.method == HttpMethod::get);
    assert(transport.pending.front().request.url == "http://127.0.0.1:13305/v1/models");

    transport.complete_next(successful_inventory());

    assert(capture.starts == std::vector<OperationId>({operation}));
    assert(workflow.phase() == WorkflowPhase::recording);
    assert(diagnostics.entries.size() == 1);
    assert(diagnostics.entries.front().event == DiagnosticEvent::recording_started);
}

void invalid_configuration_completes_before_return_and_rejects_reentrant_restart() {
    FakeCapture capture;
    FakeTransport transport;
    FakeInsertion insertion;
    FakeDiagnostics diagnostics;
    bool returned = false;
    OperationId callback_operation = 0;
    OperationId phase_restarted_operation = 0;
    OperationId restarted_operation = 0;
    WorkflowCore *core = nullptr;
    WorkflowCoreCallbacks callbacks;
    callbacks.phase_changed = [&returned, &phase_restarted_operation, &core](OperationId, WorkflowPhase phase) {
        if (phase != WorkflowPhase::idle) return;
        assert(!returned);
        phase_restarted_operation = core->start(valid_config());
    };
    callbacks.failed = [&returned, &callback_operation, &restarted_operation, &core](OperationId operation, WorkflowFailure failure) {
        assert(!returned);
        assert(failure.source == WorkflowFailureSource::protocol);
        assert(failure.protocol_status == LV_PROTOCOL_CONFIGURATION);
        callback_operation = operation;
        restarted_operation = core->start(valid_config());
    };
    WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));
    core = &workflow;

    auto invalid = valid_config();
    invalid.preferences.cleanup_prompt = " \t";
    const auto failed_operation = workflow.start(std::move(invalid));
    returned = true;

    assert(failed_operation != 0);
    assert(callback_operation == failed_operation);
    assert(phase_restarted_operation == 0);
    assert(restarted_operation == 0);
    assert(workflow.phase() == WorkflowPhase::idle);
    assert(transport.pending.empty());
    assert(terminal_event_count(diagnostics.entries) == 1);
    assert(diagnostics.entries.back().event == DiagnosticEvent::dictation_failed);

    const auto restarted_after_return = workflow.start(valid_config());
    assert(restarted_after_return > failed_operation);
    assert(workflow.phase() == WorkflowPhase::preparing);
    assert(transport.pending.size() == 1);
}

void stop_runs_second_model_gate_then_transcribes_cleans_and_inserts() {
    FakeCapture capture;
    FakeTransport transport;
    FakeInsertion insertion;
    FakeDiagnostics diagnostics;
    std::vector<WorkflowPhase> phases;
    std::vector<std::string> ready_text;
    std::vector<WorkflowOutcome> outcomes;
    WorkflowCoreCallbacks callbacks;
    callbacks.phase_changed = [&phases](OperationId, WorkflowPhase phase) { phases.push_back(phase); };
    callbacks.text_ready = [&ready_text](OperationId, std::string text) { ready_text.push_back(std::move(text)); };
    callbacks.completed = [&outcomes](OperationId, WorkflowOutcome outcome) { outcomes.push_back(outcome); };
    WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));

    const auto operation = workflow.start(valid_config());
    transport.complete_next(successful_inventory());
    capture.complete_start(0, {AdapterError::none});

    assert(workflow.phase() == WorkflowPhase::recording);
    assert(workflow.stop("LemonVox-fixture"));
    assert(workflow.phase() == WorkflowPhase::transcribing);
    assert(capture.stops == std::vector<OperationId>({operation}));

    BytesResult wav;
    wav.bytes = {'R', 'I', 'F', 'F'};
    capture.complete_stop(0, std::move(wav));
    assert(transport.pending.size() == 2);
    assert(transport.pending[1].request.url == "http://127.0.0.1:13305/v1/models");

    transport.complete_next(successful_inventory());
    assert(transport.pending.size() == 3);
    const auto &transcription = transport.pending[2].request;
    assert(transcription.method == HttpMethod::post);
    assert(transcription.url == "http://127.0.0.1:13305/v1/audio/transcriptions");
    const auto transcription_body = std::string(transcription.body.begin(), transcription.body.end());
    assert(transcription_body.find("LemonVox-fixture") != std::string::npos);
    assert(transcription_body.find("RIFF") != std::string::npos);

    transport.complete_next(successful_response(R"({"text":" Send Tomasz 14 invoices. "})"));
    assert(transport.pending.size() == 4);
    const auto &cleanup = transport.pending[3].request;
    assert(cleanup.method == HttpMethod::post);
    assert(cleanup.url == "http://127.0.0.1:13305/v1/chat/completions");
    const auto cleanup_body = std::string(cleanup.body.begin(), cleanup.body.end());
    assert(cleanup_body.find("\"model\":\"cleanup\"") != std::string::npos);
    assert(cleanup_body.find("\"prompt\"") != std::string::npos);

    transport.complete_next(successful_response(R"({"choices":[{"message":{"content":"Send Tomasz 14 invoices."},"finish_reason":"stop"}]})"));

    assert(workflow.phase() == WorkflowPhase::idle);
    assert(insertion.inspected.size() == 1);
    assert(insertion.inspected.front().original.value == 41);
    assert(insertion.inserted.size() == 1);
    assert(insertion.inserted.front().target.value == 42);
    assert(insertion.inserted.front().text == "Send Tomasz 14 invoices.");
    assert(ready_text.empty());
    assert(outcomes.size() == 1);
    assert(outcomes.front().kind == WorkflowOutcomeKind::inserted);
    assert(outcomes.front().fallback_reason == WorkflowFallbackReason::none);
    assert((phases == std::vector<WorkflowPhase>{WorkflowPhase::preparing, WorkflowPhase::recording,
        WorkflowPhase::transcribing, WorkflowPhase::cleaning, WorkflowPhase::inserting, WorkflowPhase::idle}));
    assert(diagnostics.entries.size() == 4);
    assert(diagnostics.entries[0].event == DiagnosticEvent::recording_started);
    assert(diagnostics.entries[1].event == DiagnosticEvent::transcription_started);
    assert(diagnostics.entries[2].event == DiagnosticEvent::cleanup_started);
    assert(diagnostics.entries[3].event == DiagnosticEvent::insertion_succeeded);
}

void upstream_failure_preserves_protocol_safe_utf8_detail() {
    FakeCapture capture;
    FakeTransport transport;
    FakeInsertion insertion;
    FakeDiagnostics diagnostics;
    WorkflowFailure failure;
    bool failed = false;
    WorkflowCoreCallbacks callbacks;
    callbacks.failed = [&failure, &failed](OperationId, WorkflowFailure value) {
        failure = std::move(value);
        failed = true;
    };
    WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));

    std::string detail;
    for (int index = 0; index < 999; ++index) detail += "x ";
    detail += "x\xC3\xA9";
    const auto error = std::string("{\"error\":{\"message\":\"") + detail + "\"}}";
    const auto operation = workflow.start(valid_config());
    auto upstream = successful_response(error.c_str());
    upstream.status = 500;
    transport.complete_next(std::move(upstream));

    assert(operation != 0);
    assert(failed);
    assert(failure.source == WorkflowFailureSource::protocol);
    assert(failure.protocol_status == LV_PROTOCOL_UPSTREAM);
    assert(failure.http_status == 500);
    assert(failure.safe_detail == detail);
    assert(diagnostics.entries.size() == 1);
    assert(diagnostics.entries.front().event == DiagnosticEvent::dictation_failed);
    assert(diagnostics.entries.front().http_status == 500);
    assert(workflow.phase() == WorkflowPhase::idle);
}

void terminal_failure_rejects_restart_until_its_callbacks_finish() {
    FakeCapture capture;
    FakeTransport transport;
    FakeInsertion insertion;
    FakeDiagnostics diagnostics;
    WorkflowCore *core = nullptr;
    OperationId restart = 99;
    std::vector<std::string> events;
    WorkflowCoreCallbacks callbacks;
    callbacks.phase_changed = [&core, &restart, &events](OperationId, WorkflowPhase phase) {
        if (phase != WorkflowPhase::idle) return;
        events.push_back("idle");
        restart = core->start(valid_config());
    };
    callbacks.failed = [&events](OperationId, WorkflowFailure) { events.push_back("failed"); };
    WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));
    core = &workflow;

    const auto operation = workflow.start(valid_config());
    transport.complete_next(successful_response(R"({"data":[]})"));

    assert(operation != 0);
    assert(restart == 0);
    assert((events == std::vector<std::string>{"idle", "failed"}));
    assert(workflow.phase() == WorkflowPhase::idle);
    assert(workflow.start(valid_config()) > operation);
}

void terminal_success_rejects_restart_until_its_callbacks_finish() {
    FakeCapture capture;
    FakeTransport transport;
    FakeInsertion insertion;
    FakeDiagnostics diagnostics;
    WorkflowCore *core = nullptr;
    OperationId restart = 99;
    std::vector<std::string> events;
    WorkflowCoreCallbacks callbacks;
    callbacks.phase_changed = [&core, &restart, &events](OperationId, WorkflowPhase phase) {
        if (phase != WorkflowPhase::idle) return;
        events.push_back("idle");
        restart = core->start(valid_config());
    };
    callbacks.completed = [&events](OperationId, WorkflowOutcome outcome) {
        assert(outcome.kind == WorkflowOutcomeKind::inserted);
        events.push_back("inserted");
    };
    WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));
    core = &workflow;

    const auto operation = start_recording(workflow, capture, transport);
    finish_with_cleanup(workflow, capture, transport);

    assert(restart == 0);
    assert((events == std::vector<std::string>{"idle", "inserted"}));
    const auto next = workflow.start(valid_config());
    assert(next > operation);
    const auto requests_before_old_completion = transport.pending.size();
    transport.complete(0, successful_inventory());
    assert(transport.pending.size() == requests_before_old_completion);
}

void terminal_fallback_delivers_text_and_reason_before_restart_is_allowed() {
    FakeCapture capture;
    FakeTransport transport;
    FakeInsertion insertion;
    insertion.original_result = {AdapterError::none, {{0}, false, false, true, false}};
    FakeDiagnostics diagnostics;
    WorkflowCore *core = nullptr;
    OperationId restart = 99;
    std::vector<std::string> events;
    std::string copied_text;
    WorkflowCoreCallbacks callbacks;
    callbacks.phase_changed = [&core, &restart, &events](OperationId, WorkflowPhase phase) {
        if (phase != WorkflowPhase::idle) return;
        events.push_back("idle");
        restart = core->start(valid_config());
    };
    callbacks.text_ready = [&events, &copied_text](OperationId, std::string text) {
        events.push_back("text");
        copied_text = std::move(text);
    };
    callbacks.completed = [&events](OperationId, WorkflowOutcome outcome) {
        assert(outcome.kind == WorkflowOutcomeKind::copy_fallback);
        assert(outcome.fallback_reason == WorkflowFallbackReason::target_unavailable);
        events.push_back("fallback");
    };
    WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));
    core = &workflow;

    const auto operation = start_recording(workflow, capture, transport);
    finish_with_cleanup(workflow, capture, transport);

    assert(restart == 0);
    assert(copied_text == "Send Tomasz 14 invoices.");
    assert((events == std::vector<std::string>{"idle", "text", "fallback"}));
    const auto next = workflow.start(valid_config());
    assert(next > operation);
    const auto requests_before_old_completion = transport.pending.size();
    transport.complete(3, successful_response(R"({"choices":[{"message":{"content":"old"},"finish_reason":"stop"}]})"));
    assert(transport.pending.size() == requests_before_old_completion);
}

void phase_callbacks_can_cancel_or_shutdown_before_following_native_work() {
    {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        FakeDiagnostics diagnostics;
        WorkflowCore *core = nullptr;
        WorkflowCoreCallbacks callbacks;
        callbacks.phase_changed = [&core](OperationId, WorkflowPhase phase) {
            if (phase == WorkflowPhase::preparing) core->cancel();
        };
        WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));
        core = &workflow;

        const auto operation = workflow.start(valid_config());

        assert(operation != 0);
        assert(workflow.phase() == WorkflowPhase::idle);
        assert(insertion.captured.empty());
        assert(transport.pending.empty());
        assert(terminal_event_count(diagnostics.entries) == 1);
        assert(diagnostics.entries.back().event == DiagnosticEvent::canceled);
    }

    {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        FakeDiagnostics diagnostics;
        WorkflowCore *core = nullptr;
        WorkflowCoreCallbacks callbacks;
        callbacks.phase_changed = [&core](OperationId, WorkflowPhase phase) {
            if (phase == WorkflowPhase::recording) core->shutdown();
        };
        WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));
        core = &workflow;

        const auto operation = workflow.start(valid_config());
        transport.complete_next(successful_inventory());
        capture.complete_start(0, {AdapterError::none});

        assert(operation != 0);
        assert(workflow.is_shutdown());
        assert(workflow.phase() == WorkflowPhase::idle);
        assert(capture.stops.empty());
        assert(capture.shut_down && transport.shut_down && insertion.shut_down);
        assert(terminal_event_count(diagnostics.entries) == 1);
        assert(diagnostics.entries.back().event == DiagnosticEvent::canceled);
    }
}

void empty_audio_and_model_failures_stop_before_the_next_effect() {
    {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        FakeDiagnostics diagnostics;
        std::vector<WorkflowFailure> failures;
        WorkflowCoreCallbacks callbacks;
        callbacks.failed = [&failures](OperationId, WorkflowFailure failure) { failures.push_back(std::move(failure)); };
        WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));

        start_recording(workflow, capture, transport);
        assert(workflow.stop("LemonVox-fixture"));
        capture.complete_stop(0, {AdapterError::none, {}});

        assert(failures.size() == 1);
        assert(failures.front().protocol_status == LV_PROTOCOL_RECORDING);
        assert(transport.pending.size() == 1);
    }

    {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        FakeDiagnostics diagnostics;
        std::vector<WorkflowFailure> failures;
        WorkflowCoreCallbacks callbacks;
        callbacks.failed = [&failures](OperationId, WorkflowFailure failure) { failures.push_back(std::move(failure)); };
        WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));

        workflow.start(valid_config());
        transport.complete_next(successful_response(R"({"data":[]})"));

        assert(failures.size() == 1);
        assert(failures.front().protocol_status == LV_PROTOCOL_MODEL_UNAVAILABLE);
        assert(failures.front().http_status == 0);
        assert(capture.starts.empty());
        assert(diagnostics.entries.size() == 1);
        assert(diagnostics.entries.front().http_status == 0);
    }

    {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        FakeDiagnostics diagnostics;
        std::vector<WorkflowFailure> failures;
        WorkflowCoreCallbacks callbacks;
        callbacks.failed = [&failures](OperationId, WorkflowFailure failure) { failures.push_back(std::move(failure)); };
        WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));

        start_recording(workflow, capture, transport);
        assert(workflow.stop("LemonVox-fixture"));
        capture.complete_stop(0, wav_bytes());
        transport.complete_next(successful_response(R"({"data":[]})"));

        assert(failures.size() == 1);
        assert(failures.front().protocol_status == LV_PROTOCOL_MODEL_UNAVAILABLE);
        assert(failures.front().http_status == 0);
        assert(transport.pending.size() == 2);
        assert(diagnostics.entries.back().http_status == 0);
    }
}

void transport_and_malformed_responses_are_terminal_content_free_failures() {
    {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        FakeDiagnostics diagnostics;
        std::vector<WorkflowFailure> failures;
        WorkflowCoreCallbacks callbacks;
        callbacks.failed = [&failures](OperationId, WorkflowFailure failure) { failures.push_back(std::move(failure)); };
        WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));

        workflow.start(valid_config());
        TransportResponse network;
        network.error = AdapterError::network;
        transport.complete_next(std::move(network));

        assert(failures.size() == 1);
        assert(failures.front().source == WorkflowFailureSource::adapter);
        assert(failures.front().adapter_error == AdapterError::network);
        assert(failures.front().safe_detail.empty());
        assert(diagnostics.entries.size() == 1);
        assert(diagnostics.entries.front().event == DiagnosticEvent::dictation_failed);
        assert(diagnostics.entries.front().error == AdapterError::network);
    }

    {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        FakeDiagnostics diagnostics;
        std::vector<WorkflowFailure> failures;
        WorkflowCoreCallbacks callbacks;
        callbacks.failed = [&failures](OperationId, WorkflowFailure failure) { failures.push_back(std::move(failure)); };
        WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));

        workflow.start(valid_config());
        transport.complete_next(successful_response("{"));

        assert(failures.size() == 1);
        assert(failures.front().source == WorkflowFailureSource::protocol);
        assert(failures.front().protocol_status == LV_PROTOCOL_RESPONSE);
        assert(failures.front().http_status == 0);
        assert(failures.front().safe_detail.empty());
        assert(capture.starts.empty());
        assert(diagnostics.entries.size() == 1);
        assert(diagnostics.entries.front().event == DiagnosticEvent::dictation_failed);
        assert(diagnostics.entries.front().http_status == 0);
    }
}

void terminal_diagnostics_have_one_content_free_event_per_outcome() {
    {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        FakeDiagnostics diagnostics;
        WorkflowCore workflow(capture, transport, insertion, diagnostics, {});

        start_recording(workflow, capture, transport);
        finish_with_cleanup(workflow, capture, transport);

        assert(terminal_event_count(diagnostics.entries) == 1);
        assert(diagnostics.entries.back().event == DiagnosticEvent::insertion_succeeded);
    }

    {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        insertion.original_result = {AdapterError::none, {{0}, false, false, true, false}};
        FakeDiagnostics diagnostics;
        WorkflowCore workflow(capture, transport, insertion, diagnostics, {});

        start_recording(workflow, capture, transport);
        finish_with_cleanup(workflow, capture, transport);

        assert(terminal_event_count(diagnostics.entries) == 1);
        assert(diagnostics.entries.back().event == DiagnosticEvent::insertion_fallback);
    }

    {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        FakeDiagnostics diagnostics;
        WorkflowCore workflow(capture, transport, insertion, diagnostics, {});

        workflow.start(valid_config());
        workflow.cancel();

        assert(terminal_event_count(diagnostics.entries) == 1);
        assert(diagnostics.entries.back().event == DiagnosticEvent::canceled);
    }

    {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        FakeDiagnostics diagnostics;
        WorkflowCore workflow(capture, transport, insertion, diagnostics, {});

        workflow.start(valid_config());
        TransportResponse network;
        network.error = AdapterError::network;
        transport.complete_next(std::move(network));

        assert(terminal_event_count(diagnostics.entries) == 1);
        assert(diagnostics.entries.back().event == DiagnosticEvent::dictation_failed);
        for (const auto &entry : diagnostics.entries) assert(entry.event != DiagnosticEvent::transport_failed);
    }
}

void terminal_payload_callbacks_cannot_revive_or_corrupt_an_operation() {
    {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        FakeDiagnostics diagnostics;
        WorkflowCore *core = nullptr;
        OperationId restart = 99;
        WorkflowCoreCallbacks callbacks;
        callbacks.failed = [&core, &restart](OperationId, WorkflowFailure) {
            core->cancel();
            restart = core->start(valid_config());
        };
        WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));
        core = &workflow;

        workflow.start(valid_config());
        transport.complete_next(successful_response(R"({"data":[]})"));

        assert(restart == 0);
        assert(workflow.phase() == WorkflowPhase::idle);
        assert(transport.pending.size() == 1);
    }

    {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        insertion.original_result = {AdapterError::none, {{0}, false, false, true, false}};
        FakeDiagnostics diagnostics;
        WorkflowCore *core = nullptr;
        std::size_t outcomes = 0;
        OperationId restart = 99;
        WorkflowCoreCallbacks callbacks;
        callbacks.text_ready = [&core, &restart](OperationId, std::string) {
            core->cancel();
            restart = core->start(valid_config());
            core->shutdown();
        };
        callbacks.completed = [&outcomes](OperationId, WorkflowOutcome) { ++outcomes; };
        WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));
        core = &workflow;

        start_recording(workflow, capture, transport);
        finish_with_cleanup(workflow, capture, transport);

        assert(workflow.is_shutdown());
        assert(restart == 0);
        assert(outcomes == 0);
        assert(capture.shut_down && transport.shut_down && insertion.shut_down);
    }

    {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        FakeDiagnostics diagnostics;
        WorkflowCore *core = nullptr;
        OperationId restart = 99;
        WorkflowCoreCallbacks callbacks;
        callbacks.completed = [&core, &restart](OperationId, WorkflowOutcome) {
            core->cancel();
            restart = core->start(valid_config());
        };
        WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));
        core = &workflow;

        start_recording(workflow, capture, transport);
        finish_with_cleanup(workflow, capture, transport);

        assert(restart == 0);
        assert(workflow.phase() == WorkflowPhase::idle);
    }
}

void synchronous_adapters_complete_without_reentrant_state_loss() {
    FakeCapture capture;
    capture.auto_start = true;
    capture.auto_stop = true;
    FakeTransport transport;
    transport.auto_complete = true;
    transport.automatic_responses = {
        successful_inventory(),
        successful_inventory(),
        successful_response(R"({"text":"Send Tomasz 14 invoices."})"),
        successful_response(R"({"choices":[{"message":{"content":"Send Tomasz 14 invoices."},"finish_reason":"stop"}]})"),
    };
    FakeInsertion insertion;
    FakeDiagnostics diagnostics;
    std::vector<WorkflowOutcome> outcomes;
    WorkflowCoreCallbacks callbacks;
    callbacks.completed = [&outcomes](OperationId, WorkflowOutcome outcome) { outcomes.push_back(outcome); };
    WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));

    const auto operation = workflow.start(valid_config());
    assert(operation != 0);
    assert(workflow.phase() == WorkflowPhase::recording);
    assert(workflow.stop("LemonVox-fixture"));

    assert(workflow.phase() == WorkflowPhase::idle);
    assert(transport.automatic_index == 4);
    assert(transport.pending.size() == 4);
    assert(insertion.inserted.size() == 1);
    assert(outcomes.size() == 1);
    assert(outcomes.front().kind == WorkflowOutcomeKind::inserted);
}

void duplicate_and_out_of_order_same_operation_completions_are_ignored() {
    FakeCapture capture;
    FakeTransport transport;
    FakeInsertion insertion;
    insertion.auto_original = false;
    insertion.auto_inspect = false;
    insertion.auto_insert = false;
    FakeDiagnostics diagnostics;
    std::vector<WorkflowOutcome> outcomes;
    WorkflowCoreCallbacks callbacks;
    callbacks.completed = [&outcomes](OperationId, WorkflowOutcome outcome) { outcomes.push_back(outcome); };
    WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));

    const auto operation = workflow.start(valid_config());
    insertion.complete_original(0, insertion.original_result);
    insertion.complete_original(0, insertion.original_result);
    assert(transport.pending.size() == 1);

    transport.complete(0, successful_inventory());
    transport.complete(0, successful_inventory());
    assert(capture.starts == std::vector<OperationId>({operation}));

    capture.complete_start(0, {AdapterError::none});
    capture.complete_start(0, {AdapterError::none});
    assert(workflow.phase() == WorkflowPhase::recording);
    assert(diagnostics.entries.size() == 1);

    assert(workflow.stop("LemonVox-fixture"));
    capture.complete_stop(0, wav_bytes());
    capture.complete_stop(0, wav_bytes());
    assert(transport.pending.size() == 2);

    transport.complete(1, successful_inventory());
    transport.complete(1, successful_inventory());
    assert(transport.pending.size() == 3);

    transport.complete(2, successful_response(R"({"text":"Send Tomasz 14 invoices."})"));
    transport.complete(2, successful_response(R"({"text":"old transcript"})"));
    assert(transport.pending.size() == 4);

    transport.complete(3, successful_response(R"({"choices":[{"message":{"content":"Send Tomasz 14 invoices."},"finish_reason":"stop"}]})"));
    transport.complete(3, successful_response(R"({"choices":[{"message":{"content":"old cleanup"},"finish_reason":"stop"}]})"));
    assert(insertion.inspected.size() == 1);

    insertion.complete_inspect(0, insertion.inspect_result);
    insertion.complete_inspect(0, insertion.inspect_result);
    assert(insertion.inserted.size() == 1);

    insertion.complete_insert(0, {AdapterError::none});
    insertion.complete_insert(0, {AdapterError::none});
    assert(workflow.phase() == WorkflowPhase::idle);
    assert(outcomes.size() == 1);
    assert(outcomes.front().kind == WorkflowOutcomeKind::inserted);
}

void cancel_rejects_late_results_at_every_delayed_boundary() {
    const DelayedBoundary boundaries[] = {
        DelayedBoundary::original_target,
        DelayedBoundary::first_models,
        DelayedBoundary::capture_start,
        DelayedBoundary::capture_stop,
        DelayedBoundary::second_models,
        DelayedBoundary::transcription,
        DelayedBoundary::cleanup,
        DelayedBoundary::inspect_target,
        DelayedBoundary::insert,
    };

    for (const auto boundary : boundaries) {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        insertion.auto_original = false;
        insertion.auto_inspect = false;
        insertion.auto_insert = false;
        FakeDiagnostics diagnostics;
        WorkflowCore workflow(capture, transport, insertion, diagnostics, {});

        const auto operation = reach_delayed_boundary(boundary, workflow, capture, transport, insertion);
        workflow.cancel();
        assert(workflow.phase() == WorkflowPhase::idle);
        assert(capture.canceled.back() == operation);
        assert(transport.canceled.back() == operation);
        assert(insertion.canceled.back() == operation);
        assert(terminal_event_count(diagnostics.entries) == 1);
        assert(diagnostics.entries.back().event == DiagnosticEvent::canceled);

        const auto after_cancel = dispatched_effect_count(capture, transport, insertion);
        deliver_old_completion(boundary, capture, transport, insertion);
        assert(dispatched_effect_count(capture, transport, insertion) == after_cancel);

        const auto operation_b = workflow.start(valid_config());
        assert(operation_b > operation);
        const auto after_b = dispatched_effect_count(capture, transport, insertion);
        deliver_old_completion(boundary, capture, transport, insertion);
        assert(dispatched_effect_count(capture, transport, insertion) == after_b);
    }
}

void shutdown_is_a_no_callback_barrier_and_destructor_shuts_adapters() {
    {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        insertion.auto_original = false;
        FakeDiagnostics diagnostics;
        std::vector<std::string> callbacks;
        WorkflowCoreCallbacks workflow_callbacks;
        workflow_callbacks.phase_changed = [&callbacks](OperationId, WorkflowPhase phase) {
            callbacks.push_back(phase == WorkflowPhase::preparing ? "preparing" : "other");
        };
        workflow_callbacks.failed = [&callbacks](OperationId, WorkflowFailure) { callbacks.push_back("failed"); };
        workflow_callbacks.text_ready = [&callbacks](OperationId, std::string) { callbacks.push_back("text"); };
        workflow_callbacks.completed = [&callbacks](OperationId, WorkflowOutcome) { callbacks.push_back("completed"); };
        WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(workflow_callbacks));

        const auto operation = workflow.start(valid_config());
        assert(operation != 0);
        assert(callbacks == std::vector<std::string>({"preparing"}));
        const auto effects_before_shutdown = dispatched_effect_count(capture, transport, insertion);

        workflow.shutdown();

        assert(workflow.is_shutdown());
        assert(workflow.phase() == WorkflowPhase::idle);
        assert(capture.canceled == std::vector<OperationId>({operation}));
        assert(transport.canceled == std::vector<OperationId>({operation}));
        assert(insertion.canceled == std::vector<OperationId>({operation}));
        assert(capture.shut_down && transport.shut_down && insertion.shut_down);
        assert(terminal_event_count(diagnostics.entries) == 1);
        assert(diagnostics.entries.back().event == DiagnosticEvent::canceled);
        assert(callbacks == std::vector<std::string>({"preparing"}));

        insertion.complete_original(0, insertion.original_result);
        assert(dispatched_effect_count(capture, transport, insertion) == effects_before_shutdown);
        assert(callbacks == std::vector<std::string>({"preparing"}));
        assert(workflow.start(valid_config()) == 0);
        assert(!workflow.stop("LemonVox-fixture"));
        workflow.cancel();
        assert(terminal_event_count(diagnostics.entries) == 1);
    }

    {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        FakeDiagnostics diagnostics;
        OperationId destroyed_operation = 0;
        {
            WorkflowCore workflow(capture, transport, insertion, diagnostics, {});
            destroyed_operation = workflow.start(valid_config());
            assert(destroyed_operation != 0);
        }

        assert(capture.canceled == std::vector<OperationId>({destroyed_operation}));
        assert(transport.canceled == std::vector<OperationId>({destroyed_operation}));
        assert(insertion.canceled == std::vector<OperationId>({destroyed_operation}));
        assert(capture.shut_down && transport.shut_down && insertion.shut_down);
        // After destruction the adapter contract, rather than the core, owns
        // the no-callback barrier because old lambdas no longer have a target.
    }
}

void target_tokens_and_facts_choose_insert_or_a_copy_fallback_reason() {
    {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        insertion.original_result = {AdapterError::none, {{0}, false, false, true, false}};
        FakeDiagnostics diagnostics;
        std::vector<std::string> text;
        std::vector<WorkflowOutcome> outcomes;
        WorkflowCoreCallbacks callbacks;
        callbacks.text_ready = [&text](OperationId, std::string value) { text.push_back(std::move(value)); };
        callbacks.completed = [&outcomes](OperationId, WorkflowOutcome outcome) { outcomes.push_back(outcome); };
        WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));

        start_recording(workflow, capture, transport);
        finish_with_cleanup(workflow, capture, transport);

        assert(insertion.inspected.empty());
        assert(text == std::vector<std::string>({"Send Tomasz 14 invoices."}));
        assert(outcomes.size() == 1);
        assert(outcomes.front().fallback_reason == WorkflowFallbackReason::target_unavailable);
    }

    {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        insertion.original_result = {AdapterError::none, {{0}, false, false, true, false}};
        insertion.inspect_result = {AdapterError::none, {{42}, true, false, true, false}};
        FakeDiagnostics diagnostics;
        std::vector<WorkflowOutcome> outcomes;
        WorkflowCoreCallbacks callbacks;
        callbacks.completed = [&outcomes](OperationId, WorkflowOutcome outcome) { outcomes.push_back(outcome); };
        WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));

        auto current = valid_config();
        current.preferences.insert_current_target = true;
        start_recording(workflow, capture, transport, std::move(current));
        finish_with_cleanup(workflow, capture, transport);

        assert(insertion.inspected.size() == 1);
        assert(insertion.inspected.front().original.value == 0);
        assert(insertion.inserted.size() == 1);
        assert(outcomes.size() == 1 && outcomes.front().kind == WorkflowOutcomeKind::inserted);
    }

    const struct {
        TargetResult inspect;
        WorkflowFallbackReason reason;
    } inspect_cases[] = {
        {{AdapterError::none, {{42}, true, false, true, false}}, WorkflowFallbackReason::target_changed},
        {{AdapterError::none, {{42}, true, true, true, true}}, WorkflowFallbackReason::secure_field},
        {{AdapterError::none, {{42}, false, false, true, true}}, WorkflowFallbackReason::unsupported_field},
        {{AdapterError::none, {{42}, true, false, false, true}}, WorkflowFallbackReason::target_changed},
        {{AdapterError::target_changed, {}}, WorkflowFallbackReason::target_changed},
        {{AdapterError::insertion_unavailable, {}}, WorkflowFallbackReason::accessibility_unavailable},
    };

    for (const auto &test_case : inspect_cases) {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        insertion.inspect_result = test_case.inspect;
        FakeDiagnostics diagnostics;
        std::vector<std::string> text;
        std::vector<WorkflowOutcome> outcomes;
        std::vector<WorkflowFailure> failures;
        WorkflowCoreCallbacks callbacks;
        callbacks.text_ready = [&text](OperationId, std::string value) { text.push_back(std::move(value)); };
        callbacks.completed = [&outcomes](OperationId, WorkflowOutcome outcome) { outcomes.push_back(outcome); };
        callbacks.failed = [&failures](OperationId, WorkflowFailure failure) { failures.push_back(std::move(failure)); };
        WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));

        start_recording(workflow, capture, transport);
        finish_with_cleanup(workflow, capture, transport);

        assert(failures.empty());
        assert(text == std::vector<std::string>({"Send Tomasz 14 invoices."}));
        assert(outcomes.size() == 1);
        assert(outcomes.front().kind == WorkflowOutcomeKind::copy_fallback);
        assert(outcomes.front().fallback_reason == test_case.reason);
    }

    {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        insertion.auto_insert = false;
        FakeDiagnostics diagnostics;
        std::vector<WorkflowOutcome> outcomes;
        WorkflowCoreCallbacks callbacks;
        callbacks.completed = [&outcomes](OperationId, WorkflowOutcome outcome) { outcomes.push_back(outcome); };
        WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));

        start_recording(workflow, capture, transport);
        finish_with_cleanup(workflow, capture, transport);
        insertion.complete_insert(0, {AdapterError::insertion_unavailable});

        assert(outcomes.size() == 1);
        assert(outcomes.front().kind == WorkflowOutcomeKind::copy_fallback);
        assert(outcomes.front().fallback_reason == WorkflowFallbackReason::paste_unavailable);
    }

    {
        FakeCapture capture;
        FakeTransport transport;
        FakeInsertion insertion;
        insertion.original_result = {AdapterError::insertion_unavailable, {}};
        FakeDiagnostics diagnostics;
        std::vector<WorkflowOutcome> outcomes;
        WorkflowCoreCallbacks callbacks;
        callbacks.completed = [&outcomes](OperationId, WorkflowOutcome outcome) { outcomes.push_back(outcome); };
        WorkflowCore workflow(capture, transport, insertion, diagnostics, std::move(callbacks));

        start_recording(workflow, capture, transport);
        finish_with_cleanup(workflow, capture, transport);

        assert(outcomes.size() == 1);
        assert(outcomes.front().kind == WorkflowOutcomeKind::copy_fallback);
        assert(outcomes.front().fallback_reason == WorkflowFallbackReason::accessibility_unavailable);
    }
}

void operation_uses_a_copied_configuration_snapshot_after_settings_mutate() {
    FakeCapture capture;
    FakeTransport transport;
    FakeInsertion insertion;
    FakeDiagnostics diagnostics;
    WorkflowCore workflow(capture, transport, insertion, diagnostics, {});
    auto config = valid_config();

    const auto operation = workflow.start(config);
    config.preferences.endpoint = "http://127.0.0.1:9999/v1";
    config.preferences.asr_model = "other-asr";
    config.preferences.cleanup_model = "other-cleanup";
    config.preferences.cleanup_prompt = "other prompt";
    config.endpoint.canonical_base_url = "http://127.0.0.1:9999/v1";

    transport.complete_next(successful_inventory());
    capture.complete_start(0, {AdapterError::none});
    assert(workflow.stop("LemonVox-fixture"));
    capture.complete_stop(0, wav_bytes());
    transport.complete_next(successful_inventory());

    assert(operation != 0);
    assert(transport.pending[2].request.url == "http://127.0.0.1:13305/v1/audio/transcriptions");
    const auto transcription = std::string(transport.pending[2].request.body.begin(), transport.pending[2].request.body.end());
    assert(transcription.find("\r\n\r\nasr\r\n") != std::string::npos);
    assert(transcription.find("other-asr") == std::string::npos);

    transport.complete_next(successful_response(R"({"text":"Send Tomasz 14 invoices."})"));
    assert(transport.pending[3].request.url == "http://127.0.0.1:13305/v1/chat/completions");
    const auto cleanup = std::string(transport.pending[3].request.body.begin(), transport.pending[3].request.body.end());
    assert(cleanup.find("cleanup") != std::string::npos);
    assert(cleanup.find("prompt") != std::string::npos);
    assert(cleanup.find("other-cleanup") == std::string::npos);
    assert(cleanup.find("other prompt") == std::string::npos);
}

} // namespace

int main() {
    start_validates_models_before_starting_capture();
    invalid_configuration_completes_before_return_and_rejects_reentrant_restart();
    stop_runs_second_model_gate_then_transcribes_cleans_and_inserts();
    upstream_failure_preserves_protocol_safe_utf8_detail();
    terminal_failure_rejects_restart_until_its_callbacks_finish();
    terminal_success_rejects_restart_until_its_callbacks_finish();
    terminal_fallback_delivers_text_and_reason_before_restart_is_allowed();
    phase_callbacks_can_cancel_or_shutdown_before_following_native_work();
    empty_audio_and_model_failures_stop_before_the_next_effect();
    transport_and_malformed_responses_are_terminal_content_free_failures();
    terminal_diagnostics_have_one_content_free_event_per_outcome();
    terminal_payload_callbacks_cannot_revive_or_corrupt_an_operation();
    synchronous_adapters_complete_without_reentrant_state_loss();
    duplicate_and_out_of_order_same_operation_completions_are_ignored();
    cancel_rejects_late_results_at_every_delayed_boundary();
    shutdown_is_a_no_callback_barrier_and_destructor_shuts_adapters();
    target_tokens_and_facts_choose_insert_or_a_copy_fallback_reason();
    operation_uses_a_copied_configuration_snapshot_after_settings_mutate();
    std::puts("PASS: workflow validates models before recording");
}
