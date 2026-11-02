#include "net/model_catalog.h"

#include "net/endpoint_facts.h"

#include <algorithm>
#include <utility>

namespace lemonvox::win {

namespace {

constexpr lemonvox::OperationId kCatalogIdBit = 0x8000000000000000ull;

const char kConfigurationMessage[] = "Configure a loopback Lemonade endpoint ending in /v1";
const char kNetworkMessage[] = "Local Lemonade unavailable; check endpoint and server";
const char kResponseMessage[] = "Lemonade returned an unsupported response";

CatalogResult failure(CatalogStatus status, const char *message, int http_status = 0) {
    CatalogResult result;
    result.status = status;
    result.safe_message = message;
    result.http_status = http_status;
    return result;
}

CatalogResult classify(const lemonvox::TransportResponse &response) {
    if (response.error != lemonvox::AdapterError::none) {
        return response.error == lemonvox::AdapterError::response
            ? failure(CatalogStatus::response, kResponseMessage)
            : failure(CatalogStatus::network, kNetworkMessage);
    }
    const auto http = lemonvox::Protocol::classify_http(response.status, response.bytes);
    if (http.status != LV_PROTOCOL_OK) {
        CatalogResult result = failure(CatalogStatus::http, "", response.status);
        result.safe_message = http.status == LV_PROTOCOL_UPSTREAM
            ? "Lemonade HTTP " + std::to_string(response.status) + ": " + http.value
            : "Lemonade request failed (HTTP " + std::to_string(response.status) + ")";
        return result;
    }
    lemonvox::ProtocolInventory inventory;
    if (lemonvox::Protocol::parse_inventory(response.bytes, inventory) != LV_PROTOCOL_OK) {
        return failure(CatalogStatus::response, kResponseMessage, response.status);
    }
    CatalogResult result;
    result.status = CatalogStatus::ok;
    result.http_status = response.status;
    result.models = std::move(inventory.models);
    std::stable_sort(result.models.begin(), result.models.end(),
        [](const lemonvox::ProtocolModel &left, const lemonvox::ProtocolModel &right) { return left.id < right.id; });
    if (inventory.has_reason) result.safe_message = std::move(inventory.reason);
    return result;
}

} // namespace

struct ModelCatalog::State {
    std::uint64_t counter = 0;
    lemonvox::OperationId current = 0;
    bool alive = true;
};

ModelCatalog::ModelCatalog(lemonvox::ByteTransport &transport, Executor &executor)
    : transport_(transport), executor_(executor), state_(std::make_shared<State>()) {}

ModelCatalog::~ModelCatalog() {
    cancel();
    state_->alive = false;
}

void ModelCatalog::cancel() {
    const auto previous = std::exchange(state_->current, 0);
    if (previous != 0) transport_.cancel(previous);
}

void ModelCatalog::refresh(const std::string &endpoint, Callback callback) {
    cancel();
    const lemonvox::OperationId operation = kCatalogIdBit | ++state_->counter;
    state_->current = operation;
    auto state = state_;
    auto deliver = [state, operation, callback = std::move(callback)](CatalogResult result) {
        if (!state->alive || state->current != operation) return;
        state->current = 0;
        if (callback) callback(std::move(result));
    };

    lemonvox::ProtocolConfiguration config;
    lemonvox::TransportRequest request;
    if (!endpoint_is_valid(endpoint, &config.endpoint) ||
        lemonvox::Protocol::make_models_request(config, request) != LV_PROTOCOL_OK) {
        executor_.post([deliver = std::move(deliver)]() mutable {
            deliver(failure(CatalogStatus::configuration, kConfigurationMessage));
        });
        return;
    }
    request.timeout_ms = kTimeoutMs;
    transport_.send(operation, std::move(request),
        [deliver = std::move(deliver)](lemonvox::OperationId, lemonvox::TransportResponse response) mutable {
            deliver(classify(response));
        });
}

} // namespace lemonvox::win
