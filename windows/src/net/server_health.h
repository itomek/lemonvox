#pragma once

#include "adapters.h"
#include "protocol.h"

#include <functional>
#include <memory>
#include <string>

namespace lemonvox::win {

// Application-owned health polling; no Settings lifetime or model selection is
// involved. Call on the serialized executor, including transport completions.
class ServerHealthMonitor {
public:
    using Clock = std::function<double()>;
    using Changed = std::function<void(LVServerHealthStatus)>;
    ServerHealthMonitor(lemonvox::ByteTransport &transport, Clock clock, Changed changed);
    ~ServerHealthMonitor();
    ServerHealthMonitor(const ServerHealthMonitor &) = delete;
    ServerHealthMonitor &operator=(const ServerHealthMonitor &) = delete;
    void set_endpoint(std::string endpoint);
    void tick();
    void shutdown();
    LVServerHealthStatus status() const;
private:
    struct State;
    lemonvox::ByteTransport &transport_;
    std::shared_ptr<State> state_;
};

} // namespace lemonvox::win
