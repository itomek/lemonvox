#include "net/server_health.h"

#include <cassert>
#include <cstdio>
#include <utility>
#include <vector>

using namespace lemonvox;
using namespace lemonvox::win;

struct FakeHealthTransport final : ByteTransport {
    struct Pending { OperationId token; TransportRequest request; TransportCompletion complete; };
    std::vector<Pending> requests;
    std::vector<OperationId> canceled;
    void send(OperationId token, TransportRequest request, TransportCompletion complete) override {
        requests.push_back({token, std::move(request), std::move(complete)});
    }
    void cancel(OperationId token) override { canceled.push_back(token); }
    void shutdown() override {}
    void respond(std::size_t index, int http = 200, const std::string &body = "{\"data\":[]}") {
        TransportResponse response;
        response.status = http;
        response.bytes.assign(body.begin(), body.end());
        requests.at(index).complete(requests.at(index).token, std::move(response));
    }
};

int main() {
    double now = 0;
    FakeHealthTransport transport;
    std::vector<LVServerHealthStatus> statuses;
    {
        ServerHealthMonitor monitor(transport, [&] { return now; }, [&](LVServerHealthStatus status) {
            statuses.push_back(status);
        });
        assert(monitor.status() == LV_SERVER_HEALTH_CHECKING);
        monitor.set_endpoint("https://server.example/v1");
        assert(transport.requests.size() == 1);
        assert(transport.requests[0].request.url == "https://server.example/v1/models");
        assert(transport.requests[0].request.method == HttpMethod::get);
        assert(transport.requests[0].request.timeout_ms == 3000);
        monitor.tick();
        assert(transport.requests.size() == 1); // one in flight
        transport.respond(0);
        assert(monitor.status() == LV_SERVER_HEALTH_CONNECTED);
        now = 9;
        monitor.tick();
        assert(transport.requests.size() == 1);
        now = 10;
        monitor.tick();
        assert(transport.requests.size() == 2);
        assert(monitor.status() == LV_SERVER_HEALTH_CONNECTED); // no checking flicker
        monitor.set_endpoint("http://127.0.0.1:13305/v1");
        assert(transport.canceled.size() == 1);
        assert(transport.requests.size() == 3);
        transport.respond(1, 500); // stale old endpoint completion cannot repaint
        assert(monitor.status() == LV_SERVER_HEALTH_CHECKING);
        transport.respond(2, 200, "not JSON");
        assert(monitor.status() == LV_SERVER_HEALTH_DISCONNECTED);
        now = 20;
        monitor.tick();
        assert(transport.requests.size() == 4);
        now = 23;
        monitor.tick();
        assert(monitor.status() == LV_SERVER_HEALTH_DISCONNECTED);
        assert(transport.canceled.size() == 2); // core deadline cancels native request
        transport.respond(3); // expired callback ignored
        assert(monitor.status() == LV_SERVER_HEALTH_DISCONNECTED);
        now = 30;
        monitor.tick();
        assert(transport.requests.size() == 5);
        transport.respond(4);
        assert(monitor.status() == LV_SERVER_HEALTH_CONNECTED);
        monitor.set_endpoint("http://127.0.0.1:13305/v1");
        assert(transport.requests.size() == 5); // unchanged saved preference doesn't reset polling
        now = 40;
        monitor.tick();
        assert(transport.requests.size() == 6);
    }
    const auto delivered = statuses.size();
    transport.respond(5); // response queued before destruction must not invoke UI callback
    assert(statuses.size() == delivered && transport.canceled.size() == 3);
    std::puts("PASS: app-owned server health uses configured endpoint, 10s polling, 3s deadlines, stale suppression and safe teardown");
}
