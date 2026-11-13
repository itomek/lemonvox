import Foundation

private actor HealthHTTP: HTTPTransport {
    private(set) var requests: [URLRequest] = []
    private var pending: [CheckedContinuation<(Data, Int), Error>] = []
    func send(_ request: URLRequest) async throws -> (Data, Int) {
        requests.append(request)
        return try await withCheckedThrowingContinuation { pending.append($0) }
    }
    func respond(_ code: Int, _ body: String = "{\"data\":[]}") {
        precondition(!pending.isEmpty)
        pending.removeFirst().resume(returning:(Data(body.utf8),code))
    }
}

@main struct ServerHealthTests {
    @MainActor static func wait(_ predicate: () async -> Bool) async {
        let deadline = ProcessInfo.processInfo.systemUptime + 5
        while !(await predicate()) {
            precondition(ProcessInfo.processInfo.systemUptime < deadline, "Health test condition timed out")
            await Task.yield()
        }
    }
    @MainActor static func main() async {
        let http = HealthHTTP()
        var now = 0.0
        let monitor = ServerHealthMonitor(transport:http, clock:{ now })
        var updates: [LVServerHealthStatus] = []
        monitor.onChange = { updates.append($0) }
        monitor.start(endpoint:"http://127.0.0.1:13305/v1", polling:false)
        await wait { await http.requests.count == 1 }
        let request = await http.requests[0]
        precondition(request.url?.path == "/v1/models" && request.httpMethod == "GET")
        precondition(request.timeoutInterval == 3 && request.cachePolicy == .reloadIgnoringLocalCacheData)
        await http.respond(200)
        await wait { monitor.status == LV_SERVER_HEALTH_CONNECTED }
        now = 9; monitor.tick()
        let firstCount = await http.requests.count
        precondition(firstCount == 1)
        now = 10; monitor.tick(); monitor.tick()
        await wait { await http.requests.count == 2 }
        await http.respond(503)
        await wait { monitor.status == LV_SERVER_HEALTH_DISCONNECTED }
        now = 20; monitor.tick()
        await wait { await http.requests.count == 3 }
        await http.respond(200)
        await wait { monitor.status == LV_SERVER_HEALTH_CONNECTED }
        now = 30; monitor.tick()
        await wait { await http.requests.count == 4 }
        monitor.setEndpoint("https://server.example/v1")
        await wait { await http.requests.count == 5 }
        precondition(monitor.status == LV_SERVER_HEALTH_CHECKING)
        await http.respond(200) // Old endpoint cannot change new endpoint's status.
        await Task.yield()
        precondition(monitor.status == LV_SERVER_HEALTH_CHECKING)
        await http.respond(500)
        await wait { monitor.status == LV_SERVER_HEALTH_DISCONNECTED }
        now = 40; monitor.tick()
        await wait { await http.requests.count == 6 }
        now = 43; monitor.tick()
        precondition(monitor.status == LV_SERVER_HEALTH_DISCONNECTED)
        await http.respond(200) // Timed-out request cannot turn green late.
        await Task.yield()
        precondition(monitor.status == LV_SERVER_HEALTH_DISCONNECTED)
        now = 50; monitor.tick()
        await wait { await http.requests.count == 7 }
        monitor.stop()
        await http.respond(200)
        await Task.yield()
        precondition(monitor.status == LV_SERVER_HEALTH_CHECKING)
        precondition(updates.contains(LV_SERVER_HEALTH_CONNECTED) && updates.contains(LV_SERVER_HEALTH_DISCONNECTED))
        print("PASS: background health cadence, timeout, endpoint reset, recovery and shutdown ignore stale results")
    }
}
