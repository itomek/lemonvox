#pragma once

#include "adapters.h"
#include "common/executor.h"
#include "protocol.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lemonvox::win {

enum class CatalogStatus : std::uint8_t {
    ok,
    configuration, // endpoint is not an http(s) URL ending in /v1
    network,       // server unreachable, timeout or transport failure
    http,          // non-2xx HTTP status; see http_status
    response,      // unsupported or malformed response body
};

struct CatalogResult {
    CatalogStatus status = CatalogStatus::response;
    // In inventory order sorted by id. Filter with model.asr / model.cleanup.
    std::vector<lemonvox::ProtocolModel> models;
    int http_status = 0;
    // Display-safe (core-redacted) text: the failure description, or for ok the
    // server's optional inventory reason (empty when none). Never contains the
    // endpoint URL.
    std::string safe_message;
};

// Settings helper: fetches Lemonade's model inventory (GET <endpoint>/models)
// through a ByteTransport and the shared protocol core.
//
// Threading: call refresh()/cancel() on the executor thread. The callback runs
// on that executor exactly once per refresh unless the refresh is superseded by
// a newer refresh(), canceled, or the catalog is destroyed; then it never runs.
// Operation IDs use the top bit (0x8000...) so a transport shared with
// WorkflowCore never collides with core IDs; a dedicated transport is still
// recommended. The transport must outlive the catalog; the transport owner
// calls transport.shutdown().
class ModelCatalog {
public:
    using Callback = std::function<void(CatalogResult)>;

    ModelCatalog(lemonvox::ByteTransport &transport, Executor &executor);
    ~ModelCatalog();
    ModelCatalog(const ModelCatalog &) = delete;
    ModelCatalog &operator=(const ModelCatalog &) = delete;

    // Starts a refresh for `endpoint` (UTF-8, as typed in Settings), superseding
    // any refresh in flight. Configuration errors are also delivered
    // asynchronously through the executor.
    void refresh(const std::string &endpoint, Callback callback);
    // Abandons the refresh in flight, if any.
    void cancel();

    static constexpr std::uint32_t kTimeoutMs = 10000;

private:
    struct State;
    lemonvox::ByteTransport &transport_;
    Executor &executor_;
    std::shared_ptr<State> state_;
};

} // namespace lemonvox::win
