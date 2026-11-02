#include "net/winhttp_transport.h"

#include "common/utf.h"

#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <climits>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace lemonvox::win {

using lemonvox::AdapterError;
using lemonvox::OperationId;
using lemonvox::TransportCompletion;
using lemonvox::TransportRequest;
using lemonvox::TransportResponse;

namespace {
constexpr DWORD kChunkBytes = 64 * 1024;
constexpr DWORD kCallbackFlags = WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES;
}

struct WinHttpTransport::Request {
    std::shared_ptr<Shared> shared;
    std::uint64_t key = 0;
    OperationId operation = 0;
    TransportCompletion completion;
    HINTERNET connect = nullptr;
    HINTERNET handle = nullptr;
    std::vector<std::uint8_t> body;
    std::wstring headers;
    std::vector<std::uint8_t> chunk;
    TransportResponse response;
    ULONGLONG deadline = 0;
    bool finished = false;
    bool canceled = false;
    // Owns the Request while WinHTTP may still call back with it as context.
    std::shared_ptr<Request> self;
};

struct WinHttpTransport::Shared {
    explicit Shared(Executor &target, Options settings) : executor(target), options(settings) {}

    Executor &executor;
    Options options;
    // Recursive: WinHTTP may call back synchronously from inside an API call made under the lock.
    std::recursive_mutex mutex;
    std::condition_variable_any changed;
    HINTERNET session = nullptr;
    bool shut_down = false;
    std::uint64_t next_key = 1;
    // Requests that are in flight or whose posted completion has not yet run.
    std::map<std::uint64_t, std::shared_ptr<Request>> requests;
    std::size_t open_handles = 0;
    std::thread watchdog;
};

namespace {

using Shared = WinHttpTransport::Shared;
using Request = WinHttpTransport::Request;

AdapterError map_winhttp_error(DWORD error) {
    switch (error) {
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
    case ERROR_WINHTTP_HEADER_NOT_FOUND:
    case ERROR_WINHTTP_INVALID_HEADER:
        return AdapterError::response;
    default:
        return AdapterError::network;
    }
}

bool valid_header_name(const std::string &name) {
    if (name.empty()) return false;
    for (char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte >= 0x7F || c == ':' || c == '(' || c == ')' || c == '<' || c == '>' || c == '@' ||
            c == ',' || c == ';' || c == '\\' || c == '"' || c == '/' || c == '[' || c == ']' || c == '?' ||
            c == '=' || c == '{' || c == '}') {
            return false;
        }
    }
    return true;
}

bool valid_header_value(const std::string &value) {
    for (char c : value) {
        if (c == '\r' || c == '\n' || c == '\0') return false;
    }
    return true;
}

// Lock held. Marks the request finished, releases its handles and, unless
// suppressed, posts the completion. WinHTTP delivers HANDLE_CLOSING later.
void finish(Request &request, TransportResponse response) {
    if (request.finished) return;
    request.finished = true;
    Shared &shared = *request.shared;
    HINTERNET handle = std::exchange(request.handle, nullptr);
    HINTERNET connect = std::exchange(request.connect, nullptr);
    request.body.clear();
    request.chunk.clear();
    request.chunk.shrink_to_fit();

    const auto entry = shared.requests.find(request.key);
    const bool deliver = !request.canceled && !shared.shut_down && request.completion &&
        entry != shared.requests.end();
    if (deliver) {
        auto keep = entry->second;
        auto owner = request.shared;
        auto completion = std::move(request.completion);
        const auto operation = request.operation;
        const bool posted = shared.executor.post(
            [owner, keep, completion = std::move(completion), operation,
                response = std::move(response)]() mutable {
                {
                    std::lock_guard<std::recursive_mutex> lock(owner->mutex);
                    owner->requests.erase(keep->key);
                    if (owner->shut_down || keep->canceled) return;
                }
                completion(operation, std::move(response));
            });
        if (!posted) shared.requests.erase(request.key);
    } else {
        request.completion = nullptr;
        shared.requests.erase(request.key);
    }
    shared.changed.notify_all();
    if (handle) WinHttpCloseHandle(handle);
    if (connect) WinHttpCloseHandle(connect);
}

void fail(Request &request, AdapterError error) {
    TransportResponse response;
    response.error = error;
    finish(request, std::move(response));
}

// Lock held.
void read_next(Request &request) {
    if (request.finished || !request.handle) return;
    if (!WinHttpReadData(request.handle, request.chunk.data(), kChunkBytes, nullptr)) {
        fail(request, map_winhttp_error(GetLastError()));
    }
}

void CALLBACK status_callback(HINTERNET, DWORD_PTR context, DWORD status, LPVOID info, DWORD length) {
    auto *request = reinterpret_cast<Request *>(context);
    if (!request) return;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        std::shared_ptr<Request> release;
        std::shared_ptr<Shared> shared = request->shared;
        {
            std::lock_guard<std::recursive_mutex> lock(shared->mutex);
            release = std::move(request->self);
            --shared->open_handles;
            shared->changed.notify_all();
        }
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(request->shared->mutex);
    if (request->finished || !request->handle) return;
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        if (!WinHttpReceiveResponse(request->handle, nullptr)) fail(*request, map_winhttp_error(GetLastError()));
        break;
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE: {
        DWORD code = 0;
        DWORD size = sizeof(code);
        if (!WinHttpQueryHeaders(request->handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX) ||
            code < 100 || code > 999) {
            fail(*request, AdapterError::response);
            break;
        }
        request->response.status = static_cast<int>(code);
        read_next(*request);
        break;
    }
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        if (length == 0) {
            finish(*request, std::move(request->response));
            break;
        }
        if (request->response.bytes.size() + length > request->shared->options.max_response_bytes) {
            fail(*request, AdapterError::response);
            break;
        }
        request->response.bytes.insert(request->response.bytes.end(), request->chunk.data(),
            request->chunk.data() + length);
        read_next(*request);
        break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
        const auto *result = static_cast<const WINHTTP_ASYNC_RESULT *>(info);
        fail(*request, map_winhttp_error(result ? result->dwError : ERROR_WINHTTP_INTERNAL_ERROR));
        break;
    }
    default:
        break;
    }
}

// Lock held.
void abort_request(Request &request, AdapterError error) {
    if (request.finished) return;
    fail(request, error);
}

void watchdog_loop(std::shared_ptr<Shared> shared) {
    std::unique_lock<std::recursive_mutex> lock(shared->mutex);
    while (!shared->shut_down) {
        const ULONGLONG now = GetTickCount64();
        ULONGLONG next = 0;
        std::vector<std::shared_ptr<Request>> expired;
        for (auto &entry : shared->requests) {
            const auto &request = entry.second;
            if (request->finished) continue;
            if (request->deadline <= now) expired.push_back(request);
            else if (next == 0 || request->deadline < next) next = request->deadline;
        }
        for (auto &request : expired) abort_request(*request, AdapterError::network);
        if (!expired.empty()) continue;
        if (next == 0) shared->changed.wait(lock);
        else shared->changed.wait_for(lock, std::chrono::milliseconds(next - now));
    }
}

TransportResponse error_response(AdapterError error) {
    TransportResponse response;
    response.error = error;
    return response;
}

} // namespace

WinHttpTransport::WinHttpTransport(Executor &executor) : WinHttpTransport(executor, Options()) {}

WinHttpTransport::WinHttpTransport(Executor &executor, Options options)
    : shared_(std::make_shared<Shared>(executor, options)) {
    shared_->session = WinHttpOpen(L"LemonVox", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (shared_->session) {
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(shared_->session, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    }
    shared_->watchdog = std::thread(watchdog_loop, shared_);
}

WinHttpTransport::~WinHttpTransport() {
    shutdown();
}

void WinHttpTransport::send(OperationId operation, TransportRequest request, TransportCompletion completion) {
    std::lock_guard<std::recursive_mutex> lock(shared_->mutex);
    if (shared_->shut_down) return;

    auto native = std::make_shared<Request>();
    native->shared = shared_;
    native->key = shared_->next_key++;
    native->operation = operation;
    native->completion = std::move(completion);
    shared_->requests[native->key] = native;

    auto reject = [&](AdapterError error) { finish(*native, error_response(error)); };

    bool ok = false;
    const std::wstring url = utf8_to_wide(request.url, &ok);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    parts.dwUserNameLength = static_cast<DWORD>(-1);
    parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!ok || url.empty() || url.find(L'\0') != std::wstring::npos ||
        !WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) ||
        parts.dwHostNameLength == 0 || parts.dwUserNameLength != 0 || parts.dwPasswordLength != 0) {
        reject(AdapterError::response);
        return;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    std::string headers;
    for (const auto &header : request.headers) {
        if (!valid_header_name(header.name) || !valid_header_value(header.value)) {
            reject(AdapterError::invalid_input);
            return;
        }
        headers += header.name + ": " + header.value + "\r\n";
    }
    native->headers = utf8_to_wide(headers, &ok);
    if (!ok || request.body.size() > MAXDWORD) {
        reject(AdapterError::invalid_input);
        return;
    }
    native->body = std::move(request.body);
    native->chunk.resize(kChunkBytes);

    if (!shared_->session) {
        reject(AdapterError::network);
        return;
    }
    native->connect = WinHttpConnect(shared_->session, host.c_str(), parts.nPort, 0);
    if (!native->connect) {
        reject(map_winhttp_error(GetLastError()));
        return;
    }
    const DWORD flags = WINHTTP_FLAG_REFRESH | (parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    native->handle = WinHttpOpenRequest(native->connect, request.method == lemonvox::HttpMethod::post ? L"POST" : L"GET",
        path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!native->handle) {
        reject(map_winhttp_error(GetLastError()));
        return;
    }

    const auto &options = shared_->options;
    const DWORD timeout = request.timeout_ms != 0 ? request.timeout_ms : options.default_request_timeout_ms;
    const int clamped = timeout > static_cast<DWORD>(INT_MAX) ? INT_MAX : static_cast<int>(timeout);
    DWORD disable = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_AUTHENTICATION;
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(native.get());
    if (!WinHttpSetOption(native->handle, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable)) ||
        !WinHttpSetOption(native->handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)) ||
        !WinHttpSetOption(native->handle, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon)) ||
        !WinHttpSetTimeouts(native->handle, clamped, clamped, clamped, clamped) ||
        !WinHttpSetOption(native->handle, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) ||
        WinHttpSetStatusCallback(native->handle, status_callback, kCallbackFlags, 0) ==
            WINHTTP_INVALID_STATUS_CALLBACK) {
        reject(AdapterError::network);
        return;
    }
    // From here WinHTTP reports HANDLE_CLOSING for the request handle, which releases `self`.
    native->self = native;
    ++shared_->open_handles;
    native->deadline = GetTickCount64() + options.resource_timeout_ms;
    shared_->changed.notify_all();

    const DWORD body_size = static_cast<DWORD>(native->body.size());
    if (!WinHttpSendRequest(native->handle,
            native->headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : native->headers.c_str(),
            native->headers.empty() ? 0 : static_cast<DWORD>(-1L),
            body_size ? native->body.data() : WINHTTP_NO_REQUEST_DATA, body_size, body_size, context)) {
        fail(*native, map_winhttp_error(GetLastError()));
    }
}

void WinHttpTransport::cancel(OperationId operation) {
    std::lock_guard<std::recursive_mutex> lock(shared_->mutex);
    std::vector<std::shared_ptr<Request>> matching;
    for (auto &entry : shared_->requests) {
        if (entry.second->operation == operation) matching.push_back(entry.second);
    }
    for (auto &request : matching) {
        request->canceled = true;
        abort_request(*request, AdapterError::canceled);
        shared_->requests.erase(request->key);
    }
}

void WinHttpTransport::shutdown() {
    {
        std::unique_lock<std::recursive_mutex> lock(shared_->mutex);
        if (!shared_->shut_down) {
            shared_->shut_down = true;
            std::vector<std::shared_ptr<Request>> all;
            for (auto &entry : shared_->requests) all.push_back(entry.second);
            for (auto &request : all) abort_request(*request, AdapterError::canceled);
            shared_->requests.clear();
            shared_->changed.notify_all();
        }
        // Barrier: wait until WinHTTP has released every request handle.
        shared_->changed.wait(lock, [&] { return shared_->open_handles == 0; });
    }
    if (shared_->watchdog.joinable() && shared_->watchdog.get_id() != std::this_thread::get_id()) {
        shared_->watchdog.join();
    }
    std::lock_guard<std::recursive_mutex> lock(shared_->mutex);
    if (shared_->session) {
        WinHttpCloseHandle(shared_->session);
        shared_->session = nullptr;
    }
}

} // namespace lemonvox::win
