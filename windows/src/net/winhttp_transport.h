#pragma once

#include "adapters.h"
#include "common/executor.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lemonvox::win {

// lemonvox::ByteTransport over asynchronous WinHTTP.
//
// Policy (parity with the Mac URLSession configuration): no proxy, redirects
// are never followed (a 3xx is returned as its status), no cookies, no
// automatic authentication or credentials, no caching. Per-request timeout is
// TransportRequest::timeout_ms when nonzero, otherwise 60 s; it bounds name
// resolution, connect, send and each receive (an idle timeout, like
// URLSession's timeoutIntervalForRequest). Each request is also capped at
// 120 s total (timeoutIntervalForResource). Nothing about a request (URL,
// headers, body) is ever logged.
//
// Results: any HTTP response yields error=none plus status and body (the core
// classifies status). Connection refused, unreachable, DNS, TLS and timeout
// failures yield AdapterError::network. A malformed URL or unsupported scheme
// yields AdapterError::response (Mac parity); headers containing CR/LF/NUL or
// a non-token name yield AdapterError::invalid_input. A body larger than
// Options::max_response_bytes or a malformed HTTP response yields
// AdapterError::response.
//
// Threading: send/cancel/shutdown are thread-safe but are intended to be
// called on the executor thread, as WorkflowCore does. Each completion is
// posted through Executor::post exactly once, unless suppressed by cancel()
// or shutdown(). WinHTTP callbacks never touch the core.
//
// cancel(op) aborts every in-flight request carrying `op` (closing its WinHTTP
// handles) and SUPPRESSES its completion: no callback is ever invoked for a
// canceled request, including one whose result was already posted but has not
// yet run on the executor (when cancel() runs on the executor thread). This is
// safe for WorkflowCore, which only cancels operations it has already
// invalidated and rejects late completions by ID anyway.
//
// shutdown() is a no-callback barrier: it aborts all in-flight requests, waits
// until WinHTTP has released every request handle, and guarantees that no
// completion starts after it returns. Later send() calls are ignored without a
// callback. The destructor calls shutdown().
class WinHttpTransport final : public lemonvox::ByteTransport {
public:
    struct Options {
        std::uint32_t default_request_timeout_ms = 60000;
        std::uint32_t resource_timeout_ms = 120000;
        std::size_t max_response_bytes = 64u * 1024u * 1024u;
    };

    explicit WinHttpTransport(Executor &executor);
    WinHttpTransport(Executor &executor, Options options);
    ~WinHttpTransport() override;
    WinHttpTransport(const WinHttpTransport &) = delete;
    WinHttpTransport &operator=(const WinHttpTransport &) = delete;

    void send(lemonvox::OperationId operation, lemonvox::TransportRequest request,
        lemonvox::TransportCompletion completion) override;
    void cancel(lemonvox::OperationId operation) override;
    void shutdown() override;

    struct Shared;
    struct Request;

private:
    std::shared_ptr<Shared> shared_;
};

} // namespace lemonvox::win
