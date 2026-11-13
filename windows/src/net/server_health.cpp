#include "net/server_health.h"
#include "net/endpoint_facts.h"

#include <utility>

namespace lemonvox::win {
struct ServerHealthMonitor::State {
    LVServerHealth policy{};
    Clock clock;
    Changed changed;
    std::string endpoint;
    bool configured = false;
    bool alive = true;
    void publish(LVServerHealthStatus previous) {
        if (alive && policy.status != previous && changed) changed(policy.status);
    }
};

ServerHealthMonitor::ServerHealthMonitor(lemonvox::ByteTransport &transport, Clock clock, Changed changed)
    : transport_(transport), state_(std::make_shared<State>()) {
    lv_server_health_init(&state_->policy);
    state_->clock = std::move(clock);
    state_->changed = std::move(changed);
}
ServerHealthMonitor::~ServerHealthMonitor() { shutdown(); }
LVServerHealthStatus ServerHealthMonitor::status() const { return state_->policy.status; }

void ServerHealthMonitor::shutdown() {
    if (!state_->alive) return;
    state_->alive = false;
    state_->changed = {};
    const auto pending = state_->policy.active_request;
    lv_server_health_reset(&state_->policy);
    if (pending) transport_.cancel(pending);
}

void ServerHealthMonitor::set_endpoint(std::string endpoint) {
    if (!state_->alive || (state_->configured && endpoint == state_->endpoint)) return;
    const auto previous = state_->policy.status;
    const auto pending = state_->policy.active_request;
    state_->endpoint = std::move(endpoint);
    state_->configured = true;
    lv_server_health_reset(&state_->policy);
    if (pending) transport_.cancel(pending);
    state_->publish(previous);
    tick();
}

void ServerHealthMonitor::tick() {
    auto state = state_;
    if (!state->alive || !state->configured) return;
    const auto previous = state->policy.status;
    const auto pending = state->policy.active_request;
    const auto token = lv_server_health_begin(&state->policy, state->clock());
    if (pending && pending != state->policy.active_request) transport_.cancel(pending);
    state->publish(previous);
    if (!token) return;

    auto complete = [state, token](lemonvox::TransportResponse response) {
        if (!state->alive) return;
        const auto before = state->policy.status;
        const LVProtocolSlice bytes{response.bytes.data(), response.bytes.size()};
        lv_server_health_complete(&state->policy, token, state->clock(),
            response.error == lemonvox::AdapterError::none ? 1 : 0, response.status, bytes);
        state->publish(before);
    };
    lemonvox::ProtocolConfiguration config;
    lemonvox::TransportRequest request;
    if (!endpoint_is_valid(state->endpoint, &config.endpoint) ||
        lemonvox::Protocol::make_models_request(config, request) != LV_PROTOCOL_OK) {
        lemonvox::TransportResponse invalid;
        invalid.error = lemonvox::AdapterError::invalid_input;
        complete(std::move(invalid));
        return;
    }
    request.timeout_ms = 3000;
    transport_.send(token, std::move(request),
        [complete = std::move(complete)](lemonvox::OperationId, lemonvox::TransportResponse response) mutable {
            complete(std::move(response));
        });
}
} // namespace lemonvox::win
