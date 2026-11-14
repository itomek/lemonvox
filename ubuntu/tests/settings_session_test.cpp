#include "../adapters.h"
#include "settings.h"
#include <glib/gstdio.h>
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
constexpr const char *inventory = R"({"data":[{"id":"asr","downloaded":true,"recipe":"whispercpp","labels":["transcription"]},{"id":"cleanup","downloaded":true,"recipe":"llamacpp","labels":["chat"]},{"id":"asr-next","downloaded":true,"recipe":"whispercpp","labels":["transcription"]},{"id":"cleanup-next","downloaded":true,"recipe":"llamacpp","labels":["chat"]}]})";
struct Scenario {
    int transcriptions = 0, cleanups = 0;
    bool wav_seen = false;
    std::vector<std::string> asr_bodies, cleanup_bodies;
    std::vector<int> ports;
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
        s.ports.push_back(g_uri_get_port(soup_server_message_get_uri(msg)));
        auto *body = soup_server_message_get_request_body(msg);
        std::string bytes(body->data, static_cast<std::size_t>(body->length));
        s.asr_bodies.push_back(bytes);
        s.wav_seen = bytes.find("RIFF") != std::string::npos && bytes.find("WAVE") != std::string::npos && body->length > 1000;
        if (s.cancel_processing) { s.core->cancel(); s.canceled = true; }
        reply(msg, s.error ? 503 : 200, s.empty_transcript ? "{\"text\":\"\"}" : "{\"text\":\"send Alex the 14 invoices on Friday\"}"); return;
    }
    if (std::strcmp(path, "/v1/chat/completions") == 0) {
        ++s.cleanups;
        auto *body = soup_server_message_get_request_body(msg);
        s.cleanup_bodies.emplace_back(body->data, body->length);
        reply(msg, 200, "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"Send Alex the 14 invoices on Friday.\"}}]}"); return;
    }
    reply(msg, 404, "{}");
}
struct Log : DiagnosticsSink { std::vector<DiagnosticEntry> entries; void record(DiagnosticEntry e) override { entries.push_back(e); } };

GtkWidget *row(GtkWidget *widget, const char *title) {
    if (ADW_IS_PREFERENCES_ROW(widget) && std::string(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(widget))) == title) return widget;
    for (auto *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
        if (auto *found = row(child, title)) return found;
    return nullptr;
}
GtkWidget *type(GtkWidget *widget, GType wanted) {
    if (G_TYPE_CHECK_INSTANCE_TYPE(widget, wanted)) return widget;
    for (auto *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
        if (auto *found = type(child, wanted)) return found;
    return nullptr;
}
void spin(const std::function<bool()> &condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(condition());
}
}
int main(int argc, char **argv) {
    gst_init(&argc, &argv); adw_init();
    auto *app = gtk_application_new("com.itomek.LemonVox.SessionAcceptance", G_APPLICATION_NON_UNIQUE);
    assert(g_application_register(G_APPLICATION(app), nullptr, nullptr));
    auto *dir = g_dir_make_tmp("lemonvox-session-XXXXXX", nullptr); assert(dir);
    const std::string path = std::string(dir) + "/settings.ini";
    ubuntu::SettingsStore store(path);
    Scenario s;
    auto *server = soup_server_new(nullptr, nullptr);
    soup_server_add_handler(server, nullptr, handler, &s, nullptr);
    assert(soup_server_listen_local(server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, nullptr));
    auto *uris = soup_server_get_uris(server);
    const std::string endpoint = "http://127.0.0.1:" + std::to_string(g_uri_get_port(static_cast<GUri *>(uris->data))) + "/v1";
    g_slist_free_full(uris, reinterpret_cast<GDestroyNotify>(g_uri_unref));
    auto *next_server = soup_server_new(nullptr, nullptr);
    soup_server_add_handler(next_server, nullptr, handler, &s, nullptr);
    assert(soup_server_listen_local(next_server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, nullptr));
    uris = soup_server_get_uris(next_server);
    const int next_port = g_uri_get_port(static_cast<GUri *>(uris->data));
    const std::string next_endpoint = "http://127.0.0.1:" + std::to_string(next_port) + "/v1";
    g_slist_free_full(uris, reinterpret_cast<GDestroyNotify>(g_uri_unref));
    auto saved = store.load(); saved.endpoint = endpoint; saved.asr_model = "asr";
    saved.cleanup_model = "cleanup"; saved.cleanup_prompt = "Original prompt 14.";
    std::string error; assert(store.save(saved, error));
    auto binding = saved;
    int installs = 0, refreshes = 0;
    ubuntu::GlibExecutor executor;
    ubuntu::PipeWireCaptureOptions options; options.source_factory = "audiotestsrc";
    ubuntu::PipeWireCaptureAdapter capture(executor, options);
    ubuntu::SoupByteTransport transport(executor); ubuntu::ManualCopyInsertionAdapter insertion(executor); Log log;
    int completed = 0;
    WorkflowCoreCallbacks cb; cb.completed = [&](OperationId, WorkflowOutcome) { ++completed; };
    cb.failed = [&](OperationId, WorkflowFailure) { assert(false); };
    s.core = std::make_unique<WorkflowCore>(capture, transport, insertion, log, std::move(cb));
    auto start = [&] {
        WorkflowConfig config; config.preferences = {saved.endpoint, saved.asr_model, saved.cleanup_model, saved.cleanup_prompt, false};
        config.preferences.cleanup_backend = saved.cleanup_backend;
        config.preferences.cleanup_thinking = saved.cleanup_thinking;
        assert(ubuntu::endpoint_facts(saved.endpoint, config.endpoint)); s.core->start(config);
        spin([&] { return s.core->phase() == WorkflowPhase::recording; });
    };
    start();
    auto window = std::make_unique<ubuntu::SettingsWindow>(app, saved,
        [&](const auto &candidate, std::string &message) {
            if ((saved.toggle_shortcut != candidate.toggle_shortcut || saved.cancel_shortcut != candidate.cancel_shortcut) && s.core->phase() != WorkflowPhase::idle) { message = "Finish dictation before changing shortcuts. The existing shortcuts were kept."; return false; }
            if (!ubuntu::persist_settings_change(saved, candidate,
                [&](const auto &value, std::string &e) { return store.save(value, e); },
                [&](const auto &value, std::string &) { binding = value; return true; }, message)) return false;
            saved = candidate; return true;
        }, [&](const auto &) { ++refreshes; }, [] {}, [](const auto &) {},
        [&](const auto &, const auto &, bool) { ++installs; });
    ProtocolModel a; a.id = "asr"; a.asr = true; a.downloaded = true;
    ProtocolModel b = a; b.id = "asr-next";
    ProtocolModel c; c.id = "cleanup"; c.cleanup = true; c.downloaded = true;
    ProtocolModel d = c; d.id = "cleanup-next";
    window->models({a,b,c,d}, "Synthetic inventory"); window->present();
    auto *widget = GTK_WIDGET(gtk_application_get_windows(app)->data);
    auto edit = [&](const char *title, const std::string &text) { auto *w = row(widget, title); assert(w); gtk_editable_set_text(GTK_EDITABLE(w), text.c_str()); };
    auto select = [&](const char *title, guint index) { auto *w = row(widget, title); assert(w); adw_combo_row_set_selected(ADW_COMBO_ROW(w), index); };
    // Every Settings category is changed while the production core records.
    edit("API endpoint", "invalid draft");
    select("Theme", 2); assert(saved.appearance == "dark" && saved.endpoint == endpoint);
    edit("API endpoint", next_endpoint);
    select("Transcription model", 2); select("Cleanup model", 2);
    select("API and thinking behavior", 2);
    auto *debug = type(row(widget, "Debug traces"), GTK_TYPE_SWITCH); assert(debug);
    gtk_switch_set_active(GTK_SWITCH(debug), TRUE);
    edit("Dictation shortcut", "<Control><Alt>F9"); edit("Cancellation shortcut", "<Control><Super>F10");
    auto *prompt = GTK_TEXT_VIEW(type(widget, GTK_TYPE_TEXT_VIEW)); assert(prompt);
    for (int i = 0; i < 20; ++i) gtk_text_buffer_set_text(gtk_text_view_get_buffer(prompt), ("Next prompt " + std::to_string(i)).c_str(), -1);
    // Actual close flushes pending prompt and shortcut edits before debounce.
    gtk_window_close(GTK_WINDOW(widget));
    assert(saved.cleanup_prompt == "Next prompt 19");
    assert(saved.asr_model == "asr-next" && saved.cleanup_model == "cleanup-next");
    assert(saved.debug && binding.toggle_shortcut == ubuntu::Settings{}.toggle_shortcut && binding.cancel_shortcut == ubuntu::Settings{}.cancel_shortcut);
    assert(store.load().cleanup_prompt == saved.cleanup_prompt && store.load().endpoint == next_endpoint);
    assert(s.core->phase() == WorkflowPhase::recording && installs == 0 && refreshes == 0);
    auto pause = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    spin([&] { return std::chrono::steady_clock::now() >= pause; });
    assert(s.core->stop("settings-acceptance")); spin([&] { return completed == 1; });
    assert(s.ports.size() == 1 && s.ports[0] != next_port);
    assert(s.wav_seen && s.asr_bodies.size() == 1 && s.cleanup_bodies.size() == 1);
    assert(s.asr_bodies[0].find("asr-next") == std::string::npos);
    assert(s.cleanup_bodies[0].find("Original prompt 14.") != std::string::npos);
    assert(s.cleanup_bodies[0].find("Next prompt") == std::string::npos);
    assert(s.cleanup_bodies[0].find("cleanup-next") == std::string::npos);
    assert(s.cleanup_bodies[0].find("\"enable_thinking\":false") != std::string::npos);
    // Retry shortcut drafts only after the active session has completed.
    window->present();
    edit("Dictation shortcut", "<Control><Alt>F11"); edit("Cancellation shortcut", "<Control><Super>F12");
    gtk_window_close(GTK_WINDOW(widget));
    assert(binding.toggle_shortcut == "<Control><Alt>F11" && binding.cancel_shortcut == "<Control><Super>F12");
    start(); pause = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    spin([&] { return std::chrono::steady_clock::now() >= pause; });
    assert(s.core->stop("next-session")); spin([&] { return completed == 2; });
    assert(s.ports.size() == 2 && s.ports[1] == next_port);
    assert(s.asr_bodies[1].find("asr-next") != std::string::npos);
    assert(s.cleanup_bodies[1].find("Next prompt 19") != std::string::npos && s.cleanup_bodies[1].find("cleanup-next") != std::string::npos);
    assert(s.cleanup_bodies[1].find("enable_thinking") == std::string::npos);
    s.core->shutdown(); s.core.reset(); window.reset();
    soup_server_disconnect(next_server); g_object_unref(next_server);
    soup_server_disconnect(server); g_object_unref(server); g_object_unref(app);
    assert(g_remove(path.c_str()) == 0); assert(g_rmdir(dir) == 0); g_free(dir);
    std::cout << "PASS: production Settings UI/store + synthetic capture/HTTP; all categories while recording; close-flush; invalid draft independence; original/next request snapshots; no installs\n";
}
