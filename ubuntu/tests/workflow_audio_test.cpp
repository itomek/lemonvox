// Opt-in synthetic speech integration driver. Never selects the default input.
#include "../adapters.h"
#include "workflow.h"
#include <gst/gst.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <cctype>
using namespace lemonvox;
struct QuietDiagnostics : DiagnosticsSink { void record(DiagnosticEntry) override {} };
bool expected_phrase(const std::string &text) {
    bool name = false, number = false, day = false;
    std::string token;
    auto finish = [&] {
        name = name || token == "alex";
        number = number || token == "14" || token == "fourteen";
        day = day || token == "friday";
        token.clear();
    };
    for (unsigned char c : text) {
        if (std::isalnum(c)) token.push_back(static_cast<char>(std::tolower(c)));
        else finish();
    }
    finish();
    return name && number && day;
}
// Observe the actual ASR response independently: cleanup must not conceal an
// incorrect transcription. Only booleans survive; dictated text is not logged.
struct CheckedTransport : ByteTransport {
    explicit CheckedTransport(ubuntu::GlibExecutor &executor) : native(executor) {}
    ubuntu::SoupByteTransport native;
    bool asr_checked = false, asr_matches = false;
    void send(OperationId id, TransportRequest request, TransportCompletion completion) override {
        const bool asr = request.url.find("/audio/transcriptions") != std::string::npos;
        native.send(id, std::move(request), [this, asr, completion = std::move(completion)](OperationId operation, TransportResponse response) mutable {
            if (asr && response.error == AdapterError::none && response.status == 200) {
                const auto transcript = Protocol::parse_transcription(response.bytes);
                asr_checked = transcript.status == LV_PROTOCOL_OK;
                asr_matches = asr_checked && expected_phrase(transcript.value);
            }
            completion(operation, std::move(response));
        });
    }
    void cancel(OperationId id) override { native.cancel(id); }
    void shutdown() override { native.shutdown(); }
};
int main(int argc, char **argv) {
    if (argc != 5 || !*argv[1]) { std::cerr << "Usage: audio-test EXACT_VIRTUAL_MONITOR ENDPOINT ASR_MODEL CLEANUP_MODEL\n"; return 2; }
    if (!expected_phrase("Alex, 14 invoices on Friday.") || expected_phrase("Alexandra, 114 invoices on Friday.")) return 2;
    gst_init(nullptr, nullptr);
    ubuntu::GlibExecutor executor;
    ubuntu::PipeWireCaptureOptions options; options.source_device = argv[1];
    ubuntu::PipeWireCaptureAdapter capture(executor, options);
    CheckedTransport transport(executor);
    ubuntu::ManualCopyInsertionAdapter insertion(executor);
    QuietDiagnostics diagnostics;
    bool done = false, failed = false, recording = false;
    std::string result;
    WorkflowCoreCallbacks callbacks;
    callbacks.phase_changed = [&](OperationId, WorkflowPhase phase) {
        if (phase == WorkflowPhase::recording) { recording = true; std::cout << "RECORDING" << std::endl; }
    };
    callbacks.text_ready = [&](OperationId, std::string text) { result = std::move(text); };
    callbacks.completed = [&](OperationId, WorkflowOutcome outcome) { failed = outcome.kind != WorkflowOutcomeKind::copy_fallback; done = true; };
    callbacks.failed = [&](OperationId, WorkflowFailure failure) {
        std::cerr << "Synthetic workflow failed: adapter=" << static_cast<int>(failure.adapter_error) << " protocol=" << failure.protocol_status << " http=" << failure.http_status << '\n';
        failed = true; done = true;
    };
    WorkflowCore core(capture, transport, insertion, diagnostics, std::move(callbacks));
    WorkflowConfig config;
    config.preferences = {argv[2], argv[3], argv[4], "Add punctuation. Preserve names and numbers, spelling out no numbers. Return only the cleaned text. Do not add explanations.", false};
    if (!ubuntu::endpoint_facts(argv[2], config.endpoint)) return 2;
    core.start(config);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
    auto recording_at = std::chrono::steady_clock::time_point{};
    bool stopped = false;
    while (!done && std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
        if (recording && recording_at == std::chrono::steady_clock::time_point{}) recording_at = std::chrono::steady_clock::now();
        if (recording && !stopped && std::chrono::steady_clock::now() - recording_at > std::chrono::seconds(8)) {
            stopped = true; core.stop("synthetic-audio-integration-boundary");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    core.shutdown();
    if (!done || failed) return 1;
    if (!transport.asr_checked || !transport.asr_matches || !expected_phrase(result)) {
        std::cerr << "Synthetic ASR/cleanup did not independently preserve the expected name, number and day.\n"; return 1;
    }
    std::cout << "PASS: played synthetic speech, production capture, independently checked real ASR/cleanup, expected name/number/day and Copy fallback\n";
}
