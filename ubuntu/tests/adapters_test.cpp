#include "adapters.h"

#include <libsoup/soup.h>

#include <cassert>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using lemonvox::AdapterError;
using lemonvox::Completion;
using lemonvox::OperationId;
using lemonvox::ProtocolEndpointFacts;
using lemonvox::TargetResult;
using lemonvox::TransportRequest;
using lemonvox::TransportResponse;
using lemonvox::ubuntu::ManualCopyInsertionAdapter;
using lemonvox::ubuntu::PipeWireCaptureAdapter;
using lemonvox::ubuntu::PipeWireCaptureOptions;
using lemonvox::ubuntu::SoupByteTransport;

class Context final {
public:
    Context() : context_(g_main_context_new()) {
        assert(context_);
        g_main_context_push_thread_default(context_);
    }

    ~Context() {
        g_main_context_pop_thread_default(context_);
        g_main_context_unref(context_);
    }

    GMainContext *get() const { return context_; }

    bool until(const std::function<bool()> &done, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            while (g_main_context_pending(context_)) g_main_context_iteration(context_, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        while (g_main_context_pending(context_)) g_main_context_iteration(context_, false);
        return done();
    }

private:
    GMainContext *context_ = nullptr;
};

class LocalServer final {
public:
    explicit LocalServer(std::string response = "{}", bool hold_open = false, const char *bind_address = nullptr)
        : response_(std::move(response)), hold_open_(hold_open) {
        server_ = soup_server_new(nullptr, nullptr);
        assert(server_);
        soup_server_add_handler(server_, nullptr, &LocalServer::handle, this, nullptr);
        GError *error = nullptr;
        if (bind_address) {
            GInetAddress *address = g_inet_address_new_from_string(bind_address);
            GSocketAddress *socket = g_inet_socket_address_new(address, 0);
            assert(soup_server_listen(server_, socket, static_cast<SoupServerListenOptions>(0), &error));
            g_object_unref(socket);
            g_object_unref(address);
        } else {
            assert(soup_server_listen_local(server_, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
        }
        assert(error == nullptr);
        GSList *uris = soup_server_get_uris(server_);
        assert(uris && uris->data);
        endpoint_ = g_uri_to_string(static_cast<GUri *>(uris->data));
        g_slist_free_full(uris, reinterpret_cast<GDestroyNotify>(g_uri_unref));
    }

    ~LocalServer() {
        soup_server_disconnect(server_);
        g_object_unref(server_);
    }

    std::string endpoint() const { return endpoint_; }
    bool saw_request() const { return saw_request_; }
    const std::string &query() const { return query_; }

private:
    static void handle(SoupServer *, SoupServerMessage *message, const char *, GHashTable *, void *data) {
        auto *self = static_cast<LocalServer *>(data);
        self->saw_request_ = true;
        const char *query = g_uri_get_query(soup_server_message_get_uri(message));
        self->query_ = query ? query : "";
        if (self->hold_open_) {
            soup_server_message_pause(message);
            return;
        }
        soup_server_message_set_status(message, SOUP_STATUS_OK, nullptr);
        soup_server_message_set_response(message, "application/json", SOUP_MEMORY_COPY,
            self->response_.data(), static_cast<gssize>(self->response_.size()));
    }

    SoupServer *server_ = nullptr;
    std::string endpoint_;
    std::string response_;
    bool hold_open_ = false;
    bool saw_request_ = false;
    std::string query_;
};

void endpoint_facts_accept_local_and_remote() {
    ProtocolEndpointFacts facts;
    assert(lemonvox::ubuntu::endpoint_facts("HTTP://LOCALHOST:13305/v1///", facts));
    assert(facts.canonical_base_url == "http://localhost:13305/v1");
    assert(facts.scheme_allowed && facts.host_loopback && facts.no_userinfo && facts.no_query_fragment && facts.path_ends_v1);
    assert(lemonvox::ubuntu::endpoint_is_valid("http://127.0.0.1:13305/v1"));
    assert(lemonvox::ubuntu::endpoint_is_valid("https://example.com/v1"));
    assert(lemonvox::ubuntu::endpoint_is_valid("http://192.168.1.231:13305/api/v1"));
    assert(!lemonvox::ubuntu::endpoint_is_valid("http://[:::]/v1"));
    assert(!lemonvox::ubuntu::endpoint_is_valid("http://[1:2:3]/v1"));
    assert(!lemonvox::ubuntu::endpoint_is_valid("http://user@127.0.0.1/v1"));
    assert(!lemonvox::ubuntu::endpoint_is_valid("http://127.0.0.1/v1?token=private"));
}

void manual_insertion_never_claims_a_safe_target() {
    Context context;
    ManualCopyInsertionAdapter insertion(context.get());
    bool completed = false;
    insertion.capture_original(9, [&completed](OperationId operation, TargetResult result) {
        assert(operation == 9);
        assert(result.error == AdapterError::insertion_unavailable);
        assert(result.target.token.value == 0);
        completed = true;
    });
    assert(context.until([&] { return completed; }));

    bool canceled_completion = false;
    insertion.capture_original(10, [&canceled_completion](OperationId, TargetResult) { canceled_completion = true; });
    insertion.cancel(10);
    assert(context.until([] { return false; }, std::chrono::milliseconds(25)) == false);
    assert(!canceled_completion);
}

void loopback_transport_returns_owned_response() {
    Context context;
    LocalServer server;
    SoupByteTransport transport(context.get());
    TransportRequest request;
    request.method = lemonvox::HttpMethod::post;
    request.url = server.endpoint() + "v1/models";
    request.headers = {{"Content-Type", "application/json"}};
    request.body = {'{', '}'};
    request.timeout_ms = 1000;
    bool completed = false;
    transport.send(17, std::move(request), [&completed](OperationId operation, TransportResponse response) {
        assert(operation == 17);
        assert(response.error == AdapterError::none);
        assert(response.status == 200);
        assert(response.bytes == std::vector<std::uint8_t>({'{', '}'}));
        completed = true;
    });
    assert(context.until([&] { return completed; }));
    assert(server.saw_request());
}

// Rejecting every query URL prevents the production setup catalog from reaching
// the server. Exercise the shared request builder through the native adapter.
void transport_sends_the_model_setup_catalog_query() {
    Context context;
    LocalServer server(R"({"data":[]})");
    SoupByteTransport transport(context.get());
    lemonvox::ProtocolConfiguration config;
    assert(lemonvox::ubuntu::endpoint_facts(server.endpoint() + "api/v1", config.endpoint));
    TransportRequest request;
    assert(lemonvox::Protocol::make_catalog_request(config, request) == LV_PROTOCOL_OK);
    bool completed = false;
    transport.send(40, std::move(request), [&](OperationId, TransportResponse response) {
        assert(response.error == AdapterError::none && response.status == 200);
        completed = true;
    });
    assert(context.until([&] { return completed; }));
    assert(server.saw_request() && server.query() == "show_all=true");
}

void transport_still_rejects_other_query_and_fragment_urls() {
    Context context;
    LocalServer server;
    SoupByteTransport transport(context.get());
    for (const auto *suffix : {"v1/models?token=private", "v1/models?show_all=true&token=private",
            "v1/models?show_all=true#fragment", "v1/models#fragment",
            "v1/downloads?show_all=true", "models?show_all=true"}) {
        TransportRequest request;
        request.url = server.endpoint() + suffix;
        bool completed = false;
        transport.send(41, std::move(request), [&](OperationId, TransportResponse response) {
            assert(response.error == AdapterError::invalid_input);
            completed = true;
        });
        assert(context.until([&] { return completed; }));
        assert(!server.saw_request());
    }
}

void transport_accepts_a_host_outside_the_old_loopback_allowlist() {
    Context context;
    LocalServer server("{}", false, "127.0.0.2");
    ProtocolEndpointFacts facts;
    assert(lemonvox::ubuntu::endpoint_facts(server.endpoint() + "v1", facts));
    assert(!facts.host_loopback);
    SoupByteTransport transport(context.get());
    TransportRequest request;
    request.url = server.endpoint() + "v1/models";
    bool completed = false;
    transport.send(18, std::move(request), [&](OperationId, TransportResponse response) {
        assert(response.error == AdapterError::none && response.status == 200);
        assert(response.bytes == std::vector<std::uint8_t>({'{', '}'}));
        completed = true;
    });
    assert(context.until([&] { return completed; }));
    assert(server.saw_request());
}

void synthetic_capture_returns_a_bounded_wav() {
    Context context;
    PipeWireCaptureOptions options;
    options.source_factory = "audiotestsrc";
    options.max_capture_seconds = 1;
    PipeWireCaptureAdapter capture(context.get(), options);
    bool started = false;
    capture.start(31, [&started](OperationId operation, Completion result) {
        assert(operation == 31);
        assert(result.error == AdapterError::none);
        started = true;
    });
    assert(context.until([&] { return started; }));

    bool stopped = false;
    capture.stop(31, [&stopped](OperationId operation, lemonvox::BytesResult result) {
        assert(operation == 31);
        assert(result.error == AdapterError::none);
        assert(result.bytes.size() > 44);
        assert(std::memcmp(result.bytes.data(), "RIFF", 4) == 0);
        assert(std::memcmp(result.bytes.data() + 8, "WAVE", 4) == 0);
        stopped = true;
    });
    assert(context.until([&] { return stopped; }));
}

void cancel_suppresses_a_queued_capture_completion() {
    Context context;
    PipeWireCaptureOptions options;
    options.source_factory = "lemonvox-missing-source";
    PipeWireCaptureAdapter capture(context.get(), options);
    bool callback = false;
    capture.start(34, [&callback](OperationId, Completion) { callback = true; });
    capture.cancel(34);
    assert(!context.until([&] { return callback; }, std::chrono::milliseconds(50)));
}

void shutdown_releases_queued_capture_wav_without_context_drain() {
    Context context;
    PipeWireCaptureOptions options;
    options.source_factory = "audiotestsrc";
    options.max_capture_seconds = 1;
    std::weak_ptr<PipeWireCaptureAdapter::State> weak_state;
    std::weak_ptr<int> weak_completion_capture;
    {
        PipeWireCaptureAdapter capture(context.get(), options);
        weak_state = capture.state_;
        bool started = false;
        capture.start(35, [&started](OperationId, Completion result) {
            assert(result.error == AdapterError::none);
            started = true;
        });
        assert(context.until([&] { return started; }));
        assert(!context.until([] { return false; }, std::chrono::milliseconds(25)));

        auto completion_capture = std::make_shared<int>(1);
        weak_completion_capture = completion_capture;
        capture.stop(35, [completion_capture](OperationId, lemonvox::BytesResult) {
            assert(completion_capture);
            assert(false && "shutdown must suppress queued capture completion");
        });
        completion_capture.reset();
        capture.shutdown();
    }
    // No GLib iteration occurs between shutdown/destruction and these checks.
    // The queued stop callback previously retained both State and its WAV.
    assert(weak_state.expired());
    assert(weak_completion_capture.expired());
    assert(!context.until([] { return false; }, std::chrono::milliseconds(50)));
}

void cancel_releases_queued_capture_completion_without_dispatch() {
    Context context;
    PipeWireCaptureOptions options;
    options.source_factory = "audiotestsrc";
    options.max_capture_seconds = 1;
    PipeWireCaptureAdapter capture(context.get(), options);
    bool started = false;
    capture.start(36, [&started](OperationId, Completion result) {
        assert(result.error == AdapterError::none);
        started = true;
    });
    assert(context.until([&] { return started; }));
    assert(!context.until([] { return false; }, std::chrono::milliseconds(25)));

    auto completion_capture = std::make_shared<int>(1);
    std::weak_ptr<int> weak_completion_capture = completion_capture;
    capture.stop(36, [completion_capture](OperationId, lemonvox::BytesResult) {
        assert(completion_capture);
        assert(false && "cancel must suppress queued capture completion");
    });
    completion_capture.reset();
    capture.cancel(36);
    assert(weak_completion_capture.expired());
    assert(!context.until([] { return false; }, std::chrono::milliseconds(50)));
}

void transport_rejects_a_response_above_its_bound() {
    Context context;
    LocalServer server("too large");
    lemonvox::ubuntu::SoupTransportOptions options;
    options.max_response_bytes = 2;
    SoupByteTransport transport(context.get(), options);
    TransportRequest request;
    request.url = server.endpoint() + "v1/models";
    bool completed = false;
    transport.send(32, std::move(request), [&completed](OperationId operation, TransportResponse response) {
        assert(operation == 32);
        assert(response.error == AdapterError::response);
        assert(response.bytes.empty());
        completed = true;
    });
    assert(context.until([&] { return completed; }));
}

void transport_deadline_is_a_timeout_not_user_cancellation() {
    Context context;
    LocalServer server("{}", true);
    SoupByteTransport transport(context.get());
    TransportRequest request;
    request.url = server.endpoint() + "v1/models";
    request.timeout_ms = 25;
    bool completed = false;
    transport.send(38, std::move(request), [&](OperationId, TransportResponse response) {
        assert(response.error == AdapterError::timeout);
        assert(response.status == 0 && response.bytes.empty());
        completed = true;
    });
    assert(context.until([&] { return completed; }));
    transport.shutdown();
}

void canceled_or_shutdown_transport_never_calls_back() {
    Context context;
    SoupByteTransport transport(context.get());
    TransportRequest request;
    request.url = "http://example.com/v1/models";
    bool callback = false;
    transport.send(23, request, [&callback](OperationId, TransportResponse) { callback = true; });
    transport.cancel(23);
    assert(!context.until([&] { return callback; }, std::chrono::milliseconds(50)));

    transport.send(24, std::move(request), [&callback](OperationId, TransportResponse) { callback = true; });
    transport.shutdown();
    assert(!context.until([&] { return callback; }, std::chrono::milliseconds(50)));
}

void shutdown_releases_live_transport_state_without_context_drain() {
    Context context;
    LocalServer server("{}", true);
    // This target alone is compiled with -fno-access-control so this white-box
    // regression can prove the State/request cycle is gone without changing
    // the production adapter API.
    std::weak_ptr<SoupByteTransport::State> weak_state;
    std::weak_ptr<int> weak_completion_capture;
    {
        SoupByteTransport transport(context.get());
        weak_state = transport.state_;
        auto completion_capture = std::make_shared<int>(1);
        weak_completion_capture = completion_capture;
        TransportRequest request;
        request.url = server.endpoint() + "v1/models";
        transport.send(25, std::move(request), [completion_capture](OperationId, TransportResponse) {
            assert(completion_capture);
            assert(false && "shutdown must suppress this callback");
        });
        completion_capture.reset();
        assert(context.until([&] { return server.saw_request(); }));
        transport.shutdown();
    }
    // Do not pump context after shutdown: normal app shutdown can stop it
    // before libsoup dispatches its cancellation callbacks. The only remaining
    // owner used to be the request completion retained through the State cycle.
    assert(weak_state.expired());
    assert(weak_completion_capture.expired());
    // A later native cancellation callback must use its retained lifetime
    // capsule safely and remain callback-silent.
    assert(!context.until([] { return false; }, std::chrono::milliseconds(50)));
}

} // namespace

int main() {
    endpoint_facts_accept_local_and_remote();
    manual_insertion_never_claims_a_safe_target();
    loopback_transport_returns_owned_response();
    transport_sends_the_model_setup_catalog_query();
    transport_still_rejects_other_query_and_fragment_urls();
    transport_accepts_a_host_outside_the_old_loopback_allowlist();
    synthetic_capture_returns_a_bounded_wav();
    cancel_suppresses_a_queued_capture_completion();
    shutdown_releases_queued_capture_wav_without_context_drain();
    cancel_releases_queued_capture_completion_without_dispatch();
    transport_rejects_a_response_above_its_bound();
    transport_deadline_is_a_timeout_not_user_cancellation();
    canceled_or_shutdown_transport_never_calls_back();
    shutdown_releases_live_transport_state_without_context_drain();
    g_print("PASS: Ubuntu adapter endpoint, conservative insertion, loopback transport and teardown contracts\n");
}
