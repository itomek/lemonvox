#include "net/endpoint_facts.h"
#include "net/model_catalog.h"
#include "net/winhttp_transport.h"

#include "common/executor.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace lemonvox::win;
using lemonvox::AdapterError;
using lemonvox::OperationId;
using lemonvox::TransportRequest;
using lemonvox::TransportResponse;

namespace {

void pump_until(const std::function<bool()> &done, DWORD timeout_ms = 15000) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    MSG message;
    while (!done() && GetTickCount64() < deadline) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!done()) MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
}

void pump_for(DWORD milliseconds) {
    pump_until([] { return false; }, milliseconds);
}

void wait_until(const std::function<bool()> &done, DWORD timeout_ms = 10000) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (!done() && GetTickCount64() < deadline) Sleep(5);
}

// ---------------------------------------------------------------------------
// In-process loopback HTTP/1.1 server.

struct ServerRequest {
    std::string method;
    std::string path;
    std::string headers; // lowercased raw header block
    std::string body;
};

class TestServer {
public:
    TestServer() {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(listener_ != INVALID_SOCKET);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        assert(bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
        int size = sizeof(address);
        assert(getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &size) == 0);
        port_ = ntohs(address.sin_port);
        assert(listen(listener_, SOMAXCONN) == 0);
        acceptor_ = std::thread([this] { accept_loop(); });
    }

    ~TestServer() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            for (SOCKET client : clients_) shutdown(client, SD_BOTH);
        }
        released_.notify_all();
        closesocket(listener_);
        acceptor_.join();
        for (auto &worker : workers_) worker.join();
    }

    std::string url(const std::string &path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

    int hits(const std::string &path) {
        std::lock_guard<std::mutex> lock(mutex_);
        return hits_[path];
    }

    // Number of connections that closed while their /slow request was still held.
    int aborted_slow() {
        std::lock_guard<std::mutex> lock(mutex_);
        return aborted_slow_;
    }

    std::vector<ServerRequest> requests() {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    void accept_loop() {
        for (;;) {
            SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                closesocket(client);
                return;
            }
            clients_.push_back(client);
            workers_.emplace_back([this, client] { serve(client); });
        }
    }

    static bool read_request(SOCKET client, std::string &buffer, ServerRequest &request) {
        std::size_t header_end;
        while ((header_end = buffer.find("\r\n\r\n")) == std::string::npos) {
            char chunk[4096];
            const int received = recv(client, chunk, sizeof(chunk), 0);
            if (received <= 0) return false;
            buffer.append(chunk, static_cast<std::size_t>(received));
        }
        const std::string head = buffer.substr(0, header_end);
        const std::size_t line_end = head.find("\r\n");
        const std::string line = head.substr(0, line_end);
        const std::size_t first = line.find(' ');
        const std::size_t second = line.find(' ', first + 1);
        request.method = line.substr(0, first);
        request.path = line.substr(first + 1, second - first - 1);
        request.headers = line_end == std::string::npos ? std::string() : head.substr(line_end + 2);
        for (char &c : request.headers) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        std::size_t length = 0;
        const std::size_t content_length = request.headers.find("content-length:");
        if (content_length != std::string::npos) length = std::stoul(request.headers.substr(content_length + 15));
        buffer.erase(0, header_end + 4);
        while (buffer.size() < length) {
            char chunk[4096];
            const int received = recv(client, chunk, sizeof(chunk), 0);
            if (received <= 0) return false;
            buffer.append(chunk, static_cast<std::size_t>(received));
        }
        request.body = buffer.substr(0, length);
        buffer.erase(0, length);
        return true;
    }

    static void respond(SOCKET client, int status, const std::string &reason, const std::string &body,
        const std::string &extra = {}) {
        const std::string response = "HTTP/1.1 " + std::to_string(status) + " " + reason +
            "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nContent-Type: application/json\r\n" + extra +
            "\r\n" + body;
        send(client, response.data(), static_cast<int>(response.size()), 0);
    }

    void serve(SOCKET client) {
        std::string buffer;
        ServerRequest request;
        while (read_request(client, buffer, request)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++hits_[request.path];
                requests_.push_back(request);
            }
            const std::string &path = request.path;
            if (path == "/ok") {
                respond(client, 200, "OK", "hello body");
            } else if (path == "/404") {
                respond(client, 404, "Not Found", "missing");
            } else if (path == "/500") {
                respond(client, 500, "Internal Server Error", R"({"error":{"message":"boom"}})");
            } else if (path == "/redirect") {
                respond(client, 302, "Found", "", "Location: /ok\r\n");
            } else if (path == "/cookie") {
                respond(client, 200, "OK", "", "Set-Cookie: session=secret; Path=/\r\n");
            } else if (path == "/auth") {
                respond(client, 401, "Unauthorized", "", "WWW-Authenticate: Negotiate\r\nWWW-Authenticate: NTLM\r\n");
            } else if (path == "/echo") {
                respond(client, 200, "OK", request.body);
            } else if (path == "/big") {
                respond(client, 200, "OK", std::string(300 * 1024, 'x'));
            } else if (path == "/v1/models" || path == "/api/v1/models") {
                respond(client, 200, "OK",
                    R"({"data":[{"id":"chat","downloaded":true,"recipe":"llamacpp","labels":["chat"]},)"
                    R"({"id":"asr","downloaded":true,"recipe":"whispercpp","labels":["transcription"]}]})");
            } else if (path == "/broken/v1/models") {
                respond(client, 200, "OK", "not json");
            } else if (path == "/denied/v1/models") {
                respond(client, 503, "Service Unavailable", R"({"error":{"message":"loading"}})");
            } else if (path == "/slow" || path == "/slow/v1/models") {
                std::unique_lock<std::mutex> lock(mutex_);
                // Hold the request until the client disconnects or the server stops.
                for (;;) {
                    if (stopping_) break;
                    lock.unlock();
                    fd_set set;
                    FD_ZERO(&set);
                    FD_SET(client, &set);
                    timeval wait{0, 20000};
                    const int ready = select(0, &set, nullptr, nullptr, &wait);
                    char probe;
                    const bool closed = ready > 0 && recv(client, &probe, 1, MSG_PEEK) <= 0;
                    lock.lock();
                    if (closed) {
                        ++aborted_slow_;
                        break;
                    }
                }
                break;
            } else {
                respond(client, 404, "Not Found", "");
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        closesocket(client);
        for (auto &entry : clients_) {
            if (entry == client) entry = INVALID_SOCKET;
        }
    }

    SOCKET listener_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    std::thread acceptor_;
    std::vector<std::thread> workers_;
    std::vector<SOCKET> clients_;
    std::mutex mutex_;
    std::condition_variable released_;
    bool stopping_ = false;
    std::map<std::string, int> hits_;
    std::vector<ServerRequest> requests_;
    int aborted_slow_ = 0;
};

unsigned short closed_port() {
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(probe, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    int size = sizeof(address);
    assert(getsockname(probe, reinterpret_cast<sockaddr *>(&address), &size) == 0);
    closesocket(probe);
    return ntohs(address.sin_port);
}

// Records completions and asserts each arrives on the executor thread once.
struct Recorder {
    DWORD owner = GetCurrentThreadId();
    int calls = 0;
    OperationId operation = 0;
    TransportResponse response;

    lemonvox::TransportCompletion completion() {
        return [this](OperationId completed, TransportResponse result) {
            assert(GetCurrentThreadId() == owner);
            ++calls;
            operation = completed;
            response = std::move(result);
        };
    }
};

TransportRequest get(const std::string &url, std::uint32_t timeout_ms = 0) {
    TransportRequest request;
    request.url = url;
    request.timeout_ms = timeout_ms;
    return request;
}

// Executor whose tasks run only when the test says so.
class ManualExecutor final : public Executor {
public:
    bool post(std::function<void()> task) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return false;
        tasks_.push_back(std::move(task));
        return true;
    }
    std::size_t pending() {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }
    void run_all() {
        std::deque<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            batch.swap(tasks_);
        }
        for (auto &task : batch) task();
    }

private:
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
    bool closed_ = false;
};

// ---------------------------------------------------------------------------
// Endpoint facts.

void expect_valid(const char *endpoint, const char *canonical) {
    lemonvox::ProtocolEndpointFacts facts;
    if (!endpoint_is_valid(endpoint, &facts)) {
        std::fprintf(stderr, "expected valid endpoint: %s\n", endpoint);
        assert(false);
    }
    if (facts.canonical_base_url != canonical) {
        std::fprintf(stderr, "endpoint %s canonical %s\n", endpoint, facts.canonical_base_url.c_str());
        assert(false);
    }
    // The core appends paths to the canonical base.
    lemonvox::ProtocolConfiguration config;
    config.endpoint = facts;
    TransportRequest request;
    assert(lemonvox::Protocol::make_models_request(config, request) == LV_PROTOCOL_OK);
    assert(request.url == std::string(canonical) + "/models");
}

void expect_parse_invalid(const std::string &endpoint) {
    lemonvox::ProtocolEndpointFacts facts;
    facts.scheme_allowed = true;
    if (endpoint_facts(endpoint, facts)) {
        std::fprintf(stderr, "expected unparseable endpoint: %s\n", endpoint.c_str());
        assert(false);
    }
    assert(facts.canonical_base_url.empty() && !facts.scheme_allowed);
    assert(!endpoint_is_valid(endpoint));
}

void endpoint_facts_cover_mac_rules() {
    expect_valid("http://127.0.0.1:13305/v1", "http://127.0.0.1:13305/v1");
    expect_valid("http://127.0.0.1:13305/v1/", "http://127.0.0.1:13305/v1");
    expect_valid("http://127.0.0.1:13305/v1///", "http://127.0.0.1:13305/v1");
    expect_valid("http://127.0.0.1:13305/api/v1", "http://127.0.0.1:13305/api/v1");
    expect_valid("https://localhost/v1", "https://localhost/v1");
    expect_valid("HTTP://LocalHost:8000/v1", "http://localhost:8000/v1");
    expect_valid("http://[::1]:13305/v1", "http://[::1]:13305/v1");
    expect_valid("http://[::1]/v1", "http://[::1]/v1");
    expect_valid("http://127.0.0.1:1/v1", "http://127.0.0.1:1/v1");
    expect_valid("http://127.0.0.1:65535/v1", "http://127.0.0.1:65535/v1");
    expect_valid("http://127.0.0.1:13305/a%20b/v1", "http://127.0.0.1:13305/a%20b/v1");

    expect_valid("http://example.com/v1", "http://example.com/v1");
    expect_valid("http://192.168.1.2:13305/v1", "http://192.168.1.2:13305/v1");
    expect_valid("http://127.0.0.2/v1", "http://127.0.0.2/v1");
    expect_valid("http://localhost.example.com/v1", "http://localhost.example.com/v1");
    expect_valid("http://[::2]/v1", "http://[::2]/v1");
    expect_valid("http://192.168.1.231:13305/api/v1", "http://192.168.1.231:13305/api/v1");
    expect_valid("https://inference.example.com/v1", "https://inference.example.com/v1");
    struct Case {
        const char *endpoint;
        bool scheme, loopback, no_userinfo, no_query_fragment, ends_v1;
    };
    const Case parsed_but_rejected[] = {
        {"ftp://127.0.0.1/v1", false, true, true, true, true},
        {"http://user@127.0.0.1/v1", true, true, false, true, true},
        {"http://user:pass@127.0.0.1/v1", true, true, false, true, true},
        {"http://127.0.0.1/v1?x=1", true, true, true, false, true},
        {"http://127.0.0.1/v1?", true, true, true, false, true},
        {"http://127.0.0.1/v1#frag", true, true, true, false, true},
        {"http://127.0.0.1/v2", true, true, true, true, false},
        {"http://127.0.0.1/v1/models", true, true, true, true, false},
        {"http://127.0.0.1", true, true, true, true, false},
        {"http://127.0.0.1/", true, true, true, true, false},
        {"http://127.0.0.1/V1", true, true, true, true, false},
        {"http://127.0.0.1/%76%31", true, true, true, true, false},
    };
    for (const auto &item : parsed_but_rejected) {
        lemonvox::ProtocolEndpointFacts facts;
        if (!endpoint_facts(item.endpoint, facts)) {
            std::fprintf(stderr, "expected parseable endpoint: %s\n", item.endpoint);
            assert(false);
        }
        assert(facts.scheme_allowed == item.scheme && facts.host_loopback == item.loopback);
        assert(facts.no_userinfo == item.no_userinfo && facts.no_query_fragment == item.no_query_fragment);
        assert(facts.path_ends_v1 == item.ends_v1);
        assert(lemonvox::Protocol::validate_endpoint(facts) == LV_PROTOCOL_CONFIGURATION);
        assert(!endpoint_is_valid(item.endpoint));
    }

    const char *unparseable[] = {
        "", " http://127.0.0.1/v1", "http://127.0.0.1/v1 ", "http://127.0.0.1 /v1", "http://127.0.0.1/v1\t",
        "http://127.0.0.1/v\n1", "127.0.0.1:13305/v1", "localhost/v1", "http:/127.0.0.1/v1", "http:127.0.0.1/v1",
        "://127.0.0.1/v1", "1http://127.0.0.1/v1", "http://", "http:///v1", "http://:13305/v1",
        "http://127.0.0.1:/v1", "http://127.0.0.1:0/v1", "http://127.0.0.1:013305/v1", "http://127.0.0.1:65536/v1", "http://127.0.0.1:123456/v1",
        "http://127.0.0.1:-1/v1", "http://127.0.0.1:+80/v1", "http://127.0.0.1:80x/v1", "http://127.0.0.1:1:2/v1",
        "http://[::1/v1", "http://[]/v1", "http://[::1]x/v1", "http://[zz::1]/v1", "http://[127.0.0.1]/v1",
        "http://127..0.1/v1", "http://.localhost/v1", "http://localhost./v1", "http://local_host/v1",
        "http://127.0.0.1//v1", "http://127.0.0.1/a//v1", "http://127.0.0.1/./v1", "http://127.0.0.1/../v1",
        "http://127.0.0.1/a/../v1", "http://127.0.0.1/v1%", "http://127.0.0.1/v1%2", "http://127.0.0.1/%zz/v1",
        "http://127.0.0.1\\v1", "http://127.0.0.1/v1\\", "http://127.0.0.1/{v1}", "http://127.0.0.1/v1|",
        "http://127.0.0.1/\"v1\"", "http://127.0.0.1/<v1>", "http://127.0.0.1/v1?a b", "http://us er@127.0.0.1/v1",
        "http://127.0.0.1/v1?%", "http://127.0.0.1/\xC3\xA9/v1", "http://l\xC3\xB6" "calhost/v1", "http://127.0.0.1/v1\x7F",
    };
    for (const char *endpoint : unparseable) expect_parse_invalid(endpoint);
    expect_parse_invalid(std::string("http://127.0.0.1/v1\0", 20));
    expect_parse_invalid("http://127.0.0.1/" + std::string(3000, 'a') + "/v1");
}

// ---------------------------------------------------------------------------
// Transport.

void transport_returns_status_and_body(TestServer &server) {
    UiThreadExecutor executor;
    WinHttpTransport transport(executor);
    Recorder ok, missing, failed;
    transport.send(1, get(server.url("/ok")), ok.completion());
    transport.send(2, get(server.url("/404")), missing.completion());
    transport.send(3, get(server.url("/500")), failed.completion());
    assert(ok.calls == 0); // Never synchronous.
    pump_until([&] { return ok.calls && missing.calls && failed.calls; });
    assert(ok.calls == 1 && ok.operation == 1 && ok.response.error == AdapterError::none);
    assert(ok.response.status == 200);
    assert(std::string(ok.response.bytes.begin(), ok.response.bytes.end()) == "hello body");
    assert(missing.calls == 1 && missing.operation == 2 && missing.response.error == AdapterError::none);
    assert(missing.response.status == 404);
    assert(std::string(missing.response.bytes.begin(), missing.response.bytes.end()) == "missing");
    assert(failed.calls == 1 && failed.response.error == AdapterError::none && failed.response.status == 500);
    pump_for(200);
    assert(ok.calls == 1 && missing.calls == 1 && failed.calls == 1);
    transport.shutdown();
}

void transport_never_follows_redirects_or_handles_auth(TestServer &server) {
    UiThreadExecutor executor;
    WinHttpTransport transport(executor);
    const int before = server.hits("/ok");
    Recorder redirect, auth;
    transport.send(1, get(server.url("/redirect")), redirect.completion());
    transport.send(2, get(server.url("/auth")), auth.completion());
    pump_until([&] { return redirect.calls && auth.calls; });
    assert(redirect.calls == 1 && redirect.response.error == AdapterError::none && redirect.response.status == 302);
    assert(auth.calls == 1 && auth.response.error == AdapterError::none && auth.response.status == 401);
    assert(server.hits("/auth") == 1);
    pump_for(200);
    assert(server.hits("/ok") == before);
    transport.shutdown();
}

void transport_sends_headers_body_and_no_cookies(TestServer &server) {
    UiThreadExecutor executor;
    WinHttpTransport transport(executor);
    Recorder cookie;
    transport.send(1, get(server.url("/cookie")), cookie.completion());
    pump_until([&] { return cookie.calls != 0; });
    assert(cookie.response.status == 200);

    TransportRequest post;
    post.method = lemonvox::HttpMethod::post;
    post.url = server.url("/echo");
    post.headers.push_back({"Content-Type", "multipart/form-data; boundary=abc"});
    const std::string payload = std::string("binary\0payload", 14) + std::string(100000, 'z');
    post.body.assign(payload.begin(), payload.end());
    Recorder echo;
    transport.send(2, std::move(post), echo.completion());
    pump_until([&] { return echo.calls != 0; });
    assert(echo.response.error == AdapterError::none && echo.response.status == 200);
    assert(std::string(echo.response.bytes.begin(), echo.response.bytes.end()) == payload);

    const auto requests = server.requests();
    const ServerRequest *seen = nullptr;
    for (const auto &request : requests) {
        if (request.path == "/echo") seen = &request;
    }
    assert(seen && seen->method == "POST");
    assert(seen->headers.find("content-type: multipart/form-data; boundary=abc") != std::string::npos);
    assert(seen->headers.find("cookie") == std::string::npos);
    assert(seen->headers.find("authorization") == std::string::npos);
    assert(seen->headers.find("cache-control: no-cache") != std::string::npos ||
        seen->headers.find("pragma: no-cache") != std::string::npos);
    transport.shutdown();
}

void transport_rejects_invalid_requests(TestServer &server) {
    UiThreadExecutor executor;
    WinHttpTransport transport(executor);
    Recorder bad_url, bad_scheme, bad_header, bad_name;
    transport.send(1, get("not a url"), bad_url.completion());
    transport.send(2, get("ftp://127.0.0.1/v1"), bad_scheme.completion());
    TransportRequest injected = get(server.url("/ok"));
    injected.headers.push_back({"X-Test", "a\r\nInjected: yes"});
    transport.send(3, std::move(injected), bad_header.completion());
    TransportRequest named = get(server.url("/ok"));
    named.headers.push_back({"Bad Name", "x"});
    transport.send(4, std::move(named), bad_name.completion());
    pump_until([&] { return bad_url.calls && bad_scheme.calls && bad_header.calls && bad_name.calls; });
    assert(bad_url.calls == 1 && bad_url.response.error == AdapterError::response);
    assert(bad_scheme.calls == 1 && bad_scheme.response.error == AdapterError::response);
    assert(bad_header.calls == 1 && bad_header.response.error == AdapterError::invalid_input);
    assert(bad_name.calls == 1 && bad_name.response.error == AdapterError::invalid_input);
    transport.shutdown();
}

void transport_caps_response_size(TestServer &server) {
    UiThreadExecutor executor;
    WinHttpTransport::Options options;
    options.max_response_bytes = 200 * 1024;
    WinHttpTransport transport(executor, options);
    Recorder big;
    transport.send(1, get(server.url("/big")), big.completion());
    pump_until([&] { return big.calls != 0; });
    assert(big.calls == 1 && big.response.error == AdapterError::response && big.response.bytes.empty());
    transport.shutdown();
}

void transport_maps_timeouts_and_refusal_to_network(TestServer &server) {
    UiThreadExecutor executor;
    WinHttpTransport transport(executor);
    Recorder slow, refused;
    const ULONGLONG start = GetTickCount64();
    transport.send(1, get(server.url("/slow"), 500), slow.completion());
    transport.send(2, get("http://127.0.0.1:" + std::to_string(closed_port()) + "/v1/models", 5000),
        refused.completion());
    pump_until([&] { return slow.calls && refused.calls; });
    assert(slow.calls == 1 && slow.response.error == AdapterError::network && slow.response.status == 0);
    assert(refused.calls == 1 && refused.response.error == AdapterError::network);
    assert(GetTickCount64() - start < 10000);
    transport.shutdown();

    // The total resource cap applies even when the idle timeout has not elapsed.
    UiThreadExecutor capped_executor;
    WinHttpTransport::Options options;
    options.resource_timeout_ms = 600;
    WinHttpTransport capped(capped_executor, options);
    Recorder resource;
    const ULONGLONG capped_start = GetTickCount64();
    capped.send(1, get(server.url("/slow")), resource.completion());
    pump_until([&] { return resource.calls != 0; });
    assert(resource.calls == 1 && resource.response.error == AdapterError::network);
    assert(GetTickCount64() - capped_start < 5000);
    capped.shutdown();
}

void transport_cancel_suppresses_completion(TestServer &server) {
    UiThreadExecutor executor;
    WinHttpTransport transport(executor);
    const int aborted_before = server.aborted_slow();
    const int slow_before = server.hits("/slow");
    Recorder canceled, other;
    transport.send(7, get(server.url("/slow")), canceled.completion());
    transport.send(8, get(server.url("/ok")), other.completion());
    wait_until([&] { return server.hits("/slow") == slow_before + 1; });
    assert(server.hits("/slow") == slow_before + 1);
    transport.cancel(7);
    transport.cancel(99); // Unknown IDs are ignored.
    pump_until([&] { return other.calls != 0; });
    pump_for(300);
    assert(canceled.calls == 0);
    assert(other.calls == 1 && other.operation == 8 && other.response.status == 200);
    wait_until([&] { return server.aborted_slow() == aborted_before + 1; });
    assert(server.aborted_slow() == aborted_before + 1); // The native request was really aborted.
    transport.shutdown();
}

void transport_cancel_suppresses_already_posted_completion(TestServer &server) {
    ManualExecutor executor;
    WinHttpTransport transport(executor);
    Recorder recorder;
    transport.send(5, get(server.url("/ok")), recorder.completion());
    wait_until([&] { return executor.pending() == 1; });
    assert(executor.pending() == 1);
    transport.cancel(5);
    executor.run_all();
    assert(recorder.calls == 0);

    transport.send(6, get(server.url("/ok")), recorder.completion());
    wait_until([&] { return executor.pending() == 1; });
    transport.shutdown();
    executor.run_all();
    assert(recorder.calls == 0);
}

void transport_shutdown_is_a_barrier(TestServer &server) {
    UiThreadExecutor executor;
    const int aborted_before = server.aborted_slow();
    const int slow_before = server.hits("/slow");
    Recorder slow, late;
    {
        WinHttpTransport transport(executor);
        transport.send(1, get(server.url("/slow")), slow.completion());
        transport.send(2, get(server.url("/slow")), slow.completion());
        wait_until([&] { return server.hits("/slow") == slow_before + 2; });
        assert(server.hits("/slow") == slow_before + 2);
        const ULONGLONG start = GetTickCount64();
        transport.shutdown();
        assert(GetTickCount64() - start < 5000);
        transport.send(3, get(server.url("/ok")), late.completion());
        transport.shutdown(); // Idempotent.
    }
    pump_for(300);
    assert(slow.calls == 0 && late.calls == 0);
    wait_until([&] { return server.aborted_slow() == aborted_before + 2; });
    assert(server.aborted_slow() == aborted_before + 2);
}

void transport_handles_many_concurrent_requests(TestServer &server) {
    UiThreadExecutor executor;
    WinHttpTransport transport(executor);
    std::vector<int> calls(40, 0);
    int done = 0;
    for (int index = 0; index < 40; ++index) {
        transport.send(static_cast<OperationId>(index + 1), get(server.url(index % 2 ? "/ok" : "/404")),
            [&, index](OperationId operation, TransportResponse response) {
                assert(operation == static_cast<OperationId>(index + 1));
                assert(response.error == AdapterError::none);
                assert(response.status == (index % 2 ? 200 : 404));
                ++calls[static_cast<std::size_t>(index)];
                ++done;
            });
    }
    pump_until([&] { return done == 40; });
    pump_for(100);
    for (int value : calls) assert(value == 1);
    transport.shutdown();
}

// ---------------------------------------------------------------------------
// Model catalog.

void catalog_fetches_and_classifies(TestServer &server) {
    UiThreadExecutor executor;
    WinHttpTransport transport(executor);
    ModelCatalog catalog(transport, executor);

    std::vector<CatalogResult> results;
    auto record = [&](CatalogResult result) { results.push_back(std::move(result)); };

    catalog.refresh(server.url("/v1"), record);
    pump_until([&] { return results.size() == 1; });
    assert(results.size() == 1 && results[0].status == CatalogStatus::ok && results[0].http_status == 200);
    assert(results[0].models.size() == 2);
    assert(results[0].models[0].id == "asr" && results[0].models[0].asr && !results[0].models[0].cleanup);
    assert(results[0].models[1].id == "chat" && results[0].models[1].cleanup);

    catalog.refresh(server.url("/api/v1/"), record);
    pump_until([&] { return results.size() == 2; });
    assert(results[1].status == CatalogStatus::ok && results[1].models.size() == 2);

    catalog.refresh("http://example.com/v1", record);
    assert(results.size() == 2); // Configuration errors are asynchronous too.
    pump_until([&] { return results.size() == 3; });
    assert(results[2].status == CatalogStatus::configuration && !results[2].safe_message.empty());

    catalog.refresh(server.url("/broken/v1"), record);
    pump_until([&] { return results.size() == 4; });
    assert(results[3].status == CatalogStatus::response);

    catalog.refresh(server.url("/denied/v1"), record);
    pump_until([&] { return results.size() == 5; });
    assert(results[4].status == CatalogStatus::http && results[4].http_status == 503);
    assert(results[4].safe_message == "Lemonade HTTP 503: loading");
    assert(results[4].safe_message.find("127.0.0.1") == std::string::npos);

    catalog.refresh("http://127.0.0.1:" + std::to_string(closed_port()) + "/v1", record);
    pump_until([&] { return results.size() == 6; });
    assert(results[5].status == CatalogStatus::network);
    transport.shutdown();
}

void catalog_newer_refresh_supersedes_older(TestServer &server) {
    UiThreadExecutor executor;
    WinHttpTransport transport(executor);
    ModelCatalog catalog(transport, executor);
    const int aborted_before = server.aborted_slow();
    const int slow_before = server.hits("/slow/v1/models");
    int stale = 0;
    std::vector<CatalogResult> results;
    catalog.refresh(server.url("/slow/v1"), [&](CatalogResult) { ++stale; });
    wait_until([&] { return server.hits("/slow/v1/models") == slow_before + 1; });
    catalog.refresh(server.url("/v1"), [&](CatalogResult result) { results.push_back(std::move(result)); });
    pump_until([&] { return !results.empty(); });
    pump_for(200);
    assert(stale == 0 && results.size() == 1 && results[0].status == CatalogStatus::ok);
    wait_until([&] { return server.aborted_slow() == aborted_before + 1; });
    assert(server.aborted_slow() == aborted_before + 1);

    int canceled = 0;
    catalog.refresh("http://example.com/v1", [&](CatalogResult) { ++canceled; });
    catalog.cancel();
    pump_for(200);
    assert(canceled == 0);

    {
        ModelCatalog destroyed(transport, executor);
        destroyed.refresh("http://example.com/v1", [&](CatalogResult) { ++canceled; });
    }
    pump_for(200);
    assert(canceled == 0);
    transport.shutdown();
}

} // namespace

int main() {
    WSADATA data;
    assert(WSAStartup(MAKEWORD(2, 2), &data) == 0);
    endpoint_facts_cover_mac_rules();
    {
        TestServer server;
        transport_returns_status_and_body(server);
        transport_never_follows_redirects_or_handles_auth(server);
        transport_sends_headers_body_and_no_cookies(server);
        transport_rejects_invalid_requests(server);
        transport_caps_response_size(server);
        transport_maps_timeouts_and_refusal_to_network(server);
        transport_cancel_suppresses_completion(server);
        transport_cancel_suppresses_already_posted_completion(server);
        transport_shutdown_is_a_barrier(server);
        transport_handles_many_concurrent_requests(server);
        catalog_fetches_and_classifies(server);
        catalog_newer_refresh_supersedes_older(server);
    }
    WSACleanup();
    std::puts("net_test passed");
    return 0;
}
