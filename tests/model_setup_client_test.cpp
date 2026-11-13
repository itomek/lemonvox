#include "model_setup_client.h"
#include <cassert>
#include <deque>
#include <iostream>

using namespace lemonvox;
namespace {
struct FakeTransport final : ByteTransport {
    TransportRequest last;
    TransportCompletion completion;
    OperationId operation = 0;
    std::vector<std::string> urls;
    void send(OperationId id, TransportRequest request, TransportCompletion done) override {
        operation = id; last = std::move(request); completion = std::move(done); urls.push_back(last.url);
    }
    void cancel(OperationId) override { completion = {}; }
    void shutdown() override { completion = {}; }
    void reply(const std::string &body, int status = 200) {
        auto done = std::move(completion); assert(done);
        done(operation, {AdapterError::none, status, {body.begin(), body.end()}});
    }
};
const char *catalog_missing = R"({"data":[{"id":"Whisper-Base","downloaded":false,"labels":["transcription"],"recipe":"whispercpp","size":0.5}]})";
const char *catalog_installed = R"({"data":[{"id":"Whisper-Base","downloaded":true,"labels":["transcription"],"recipe":"whispercpp","size":0.5}]})";
ProtocolConfiguration config() {
    ProtocolConfiguration c;
    c.endpoint = {"http://localhost:13305/api/v1", true, true, true, true, true};
    return c;
}
struct Harness {
    FakeTransport transport;
    std::deque<std::function<void()>> timers;
    ModelSetupClient::Snapshot view;
    int notifications = 0;
    ModelSetupClient client{transport, [this](unsigned, auto done) { timers.push_back(std::move(done)); }, [this](auto v) { view = std::move(v); ++notifications; }};
    void ready() {
        client.refresh(config()); transport.reply(catalog_missing);
        assert(transport.last.url == "http://localhost:13305/api/v1/health");
        transport.reply(R"({"version":"2026.46.2","all_models_loaded":[]})"); transport.reply("[]");
        assert(view.management_supported && view.available && !view.active);
        for (const auto &url : transport.urls) assert(url.find("/pull") == std::string::npos);
    }
    void tick() { assert(!timers.empty()); auto f = std::move(timers.front()); timers.pop_front(); f(); }
};
void consent_and_verify() {
    Harness h; h.ready();
    const auto count = h.transport.urls.size();
    h.client.install("Whisper-Base", LV_MODEL_ASR, false);
    assert(h.transport.urls.size() == count);
    h.client.install("../injected", LV_MODEL_ASR, true);
    assert(h.transport.urls.size() == count);
    h.client.install("Whisper-Base", LV_MODEL_ASR, true);
    assert(h.view.active && h.transport.last.method == HttpMethod::post);
    h.transport.reply(R"({"id":"model:Whisper-Base","status":"downloading","running":true,"percent":5})");
    assert(h.view.percent == 5 && h.view.active);
    h.tick(); h.transport.reply(R"([{"id":"model:Whisper-Base","status":"completed","running":false,"percent":100}])");
    assert(h.view.installed_id.empty());
    h.transport.reply(catalog_installed);
    assert(!h.view.active && h.view.installed_id == "Whisper-Base");
}
void cancel_waits_for_worker() {
    Harness h; h.ready();
    h.client.install("Whisper-Base", LV_MODEL_ASR, true);
    h.transport.reply(R"({"id":"model:Whisper-Base","status":"downloading","running":true})");
    h.client.cancel();
    assert(h.transport.last.url.find("/downloads/control") != std::string::npos);
    h.transport.reply(R"({"id":"model:Whisper-Base","status":"cancelled","running":true})");
    assert(h.view.active);
    h.tick(); // Stale pre-cancel timer has no effect.
    h.tick(); h.transport.reply(R"([{"id":"model:Whisper-Base","status":"cancelled","running":false}])");
    assert(!h.view.active && h.view.installed_id.empty());
}
void ambiguous_start_requires_refresh() {
    Harness h; h.ready();
    h.client.install("Whisper-Base", LV_MODEL_ASR, true);
    auto done = std::move(h.transport.completion);
    done(h.transport.operation, {AdapterError::network, 0, {}});
    assert(!h.view.active && h.view.installed_id.empty());
    auto count = h.transport.urls.size();
    auto notices = h.notifications;
    h.client.install("Whisper-Base", LV_MODEL_ASR, true);
    assert(h.transport.urls.size() == count && h.notifications > notices && !h.view.active);
    h.client.refresh(config()); h.transport.reply(catalog_missing);
    h.transport.reply(R"({"version":"2026.46.2"})");
    h.transport.reply(R"([{"id":"model:Whisper-Base","status":"downloading","running":true}])");
    assert(h.view.active); // Reconnect tracks the uncertain existing job, never starts another.
    assert(h.transport.urls.back().find("/downloads") != std::string::npos);
}
void stale_callbacks_and_failed_confirmation() {
    Harness h; h.ready();
    h.client.install("Whisper-Base", LV_MODEL_ASR, true);
    auto stale = h.transport.completion;
    const auto old_id = h.transport.operation;
    h.transport.reply(R"({"id":"model:Whisper-Base","status":"downloading","running":true})");
    h.client.cancel();
    stale(old_id, {AdapterError::none, 200, {}});
    assert(h.view.active);
    h.transport.reply(R"({"id":"model:Whisper-Base","status":"completed","running":false})");
    h.transport.reply(catalog_missing);
    assert(h.view.installed_id.empty() && !h.view.active);
    h.client.install("Whisper-Base", LV_MODEL_ASR, true);
    h.transport.reply(R"({"id":"model:Whisper-Base","status":"error","running":false})");
    assert(!h.view.active && h.view.installed_id.empty());
}
void absent_job_and_late_callbacks() {
    Harness h; h.ready();
    h.client.install("Whisper-Base", LV_MODEL_ASR, true);
    h.transport.reply(R"({"id":"model:Whisper-Base","status":"downloading","running":true})");
    for (int i = 0; i < 5 && h.view.active; ++i) { h.tick(); h.transport.reply("[]"); }
    assert(!h.view.active && h.view.installed_id.empty());
    const auto count = h.transport.urls.size();
    h.client.install("Whisper-Base", LV_MODEL_ASR, true);
    assert(h.transport.urls.size() == count); // Must refresh uncertain server state first.
}
}
int main() {
    consent_and_verify(); cancel_waits_for_worker(); absent_job_and_late_callbacks();
    ambiguous_start_requires_refresh(); stale_callbacks_and_failed_confirmation();
    std::cout << "PASS: reusable setup consent, progress, cancel acknowledgement and catalog verification\n";
}
