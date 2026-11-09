#include "../adapters.h"
#include "workflow.h"
#include <libsoup/soup.h>
#include <gst/gst.h>
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <chrono>
#include <thread>
using namespace lemonvox;
namespace {
constexpr const char *inventory = R"({"data":[{"id":"asr","downloaded":true,"recipe":"whispercpp","labels":["transcription"]},{"id":"cleanup","downloaded":true,"recipe":"llamacpp","labels":["chat"]}]})";
struct Scenario {
    int transcriptions = 0, cleanups = 0;
    bool wav_seen = false;
    bool cancel_processing = false;
    bool empty_transcript = false;
    bool error = false;
    bool canceled = false;
    std::unique_ptr<WorkflowCore> core;
};
void reply(SoupServerMessage *msg, unsigned status, const char *body) {
    soup_server_message_set_status(msg, status, nullptr);
    soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY, body, std::strlen(body));
}
void handler(SoupServer *, SoupServerMessage *msg, const char *path, GHashTable *, gpointer data) {
    auto &s = *static_cast<Scenario *>(data);
    if (std::strcmp(path, "/v1/models") == 0) { reply(msg, 200, inventory); return; }
    if (std::strcmp(path, "/v1/audio/transcriptions") == 0) {
        ++s.transcriptions;
        auto *body = soup_server_message_get_request_body(msg);
        std::string bytes(body->data, static_cast<std::size_t>(body->length));
        s.wav_seen = bytes.find("RIFF") != std::string::npos && bytes.find("WAVE") != std::string::npos && body->length > 1000;
        if (s.cancel_processing) { s.core->cancel(); s.canceled = true; }
        reply(msg, s.error ? 503 : 200, s.empty_transcript ? "{\"text\":\"\"}" : "{\"text\":\"send Alex the 14 invoices on Friday\"}"); return;
    }
    if (std::strcmp(path, "/v1/chat/completions") == 0) {
        ++s.cleanups;
        reply(msg, 200, "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"Send Alex the 14 invoices on Friday.\"}}]}"); return;
    }
    reply(msg, 404, "{}");
}
struct Log : DiagnosticsSink { std::vector<DiagnosticEntry> entries; void record(DiagnosticEntry e) override { entries.push_back(e); } };
void run(bool cancel_recording, bool cancel_processing, bool empty_transcript, bool server_error) {
    Scenario s; s.cancel_processing = cancel_processing; s.empty_transcript = empty_transcript; s.error = server_error;
    auto *server = soup_server_new(nullptr, nullptr);
    soup_server_add_handler(server, nullptr, handler, &s, nullptr);
    GError *error = nullptr;
    assert(soup_server_listen_local(server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
    auto *uris = soup_server_get_uris(server); auto *uri = static_cast<GUri *>(uris->data);
    const std::string endpoint = "http://127.0.0.1:" + std::to_string(g_uri_get_port(uri)) + "/v1";
    g_slist_free_full(uris, reinterpret_cast<GDestroyNotify>(g_uri_unref));
    ubuntu::GlibExecutor executor;
    ubuntu::PipeWireCaptureOptions options; options.source_factory = "audiotestsrc";
    ubuntu::PipeWireCaptureAdapter capture(executor, options);
    ubuntu::SoupByteTransport transport(executor);
    ubuntu::ManualCopyInsertionAdapter insertion(executor);
    Log log;
    std::string text;
    bool failed = false, completed = false, recording = false;
    WorkflowCoreCallbacks cb;
    cb.phase_changed = [&](OperationId, WorkflowPhase phase) { if (phase == WorkflowPhase::recording) recording = true; };
    cb.text_ready = [&](OperationId, std::string result) { text = std::move(result); };
    cb.failed = [&](OperationId, WorkflowFailure) { failed = true; };
    cb.completed = [&](OperationId, WorkflowOutcome outcome) { assert(outcome.kind == WorkflowOutcomeKind::copy_fallback); completed = true; };
    s.core = std::make_unique<WorkflowCore>(capture, transport, insertion, log, std::move(cb));
    WorkflowConfig config;
    config.preferences = {endpoint, "asr", "cleanup", "Preserve names and numbers.", false};
    assert(ubuntu::endpoint_facts(endpoint, config.endpoint));
    s.core->start(config);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto recording_at = std::chrono::steady_clock::time_point{};
    bool stopped = false;
    while (std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
        if (recording && recording_at == std::chrono::steady_clock::time_point{}) recording_at = std::chrono::steady_clock::now();
        if (recording && !stopped && std::chrono::steady_clock::now() - recording_at > std::chrono::milliseconds(250)) {
            stopped = true;
            if (cancel_recording) { s.core->cancel(); s.canceled = true; }
            else { assert(s.core->stop("native-test-boundary")); assert(!s.core->stop("duplicate")); }
        }
        if (failed || completed || s.canceled) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Let canceled native HTTP completions arrive; they must not produce a result.
    if (s.canceled) {
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        while (std::chrono::steady_clock::now() < until) { while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        assert(!failed && !completed && text.empty() && s.cleanups == 0);
    } else if (empty_transcript || server_error) assert(failed && !completed && text.empty() && s.cleanups == 0);
    else { assert(completed && !failed && text == "Send Alex the 14 invoices on Friday." && s.transcriptions == 1 && s.cleanups == 1); }
    assert(s.core->phase() == WorkflowPhase::idle);
    if (!cancel_recording) assert(s.wav_seen);
    assert(!log.entries.empty());
    s.core->shutdown(); s.core.reset();
    soup_server_disconnect(server); g_object_unref(server);
}
}
int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    run(false, false, false, false);
    run(true, false, false, false);
    run(false, true, false, false);
    run(false, false, true, false);
    run(false, false, false, true);
    std::cout << "PASS: native capture + HTTP + shared workflow, one result, cancel and error recovery\n";
}
