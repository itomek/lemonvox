import Foundation

struct DiscoveredModels {
    let transcription: [LocalModel]
    let cleanup: [LocalModel]
    let reason: String?
    let version: String?
}
struct ModelDiscovery {
    let transport: HTTPTransport
    fileprivate func modelsRequest(endpoint: String) throws -> URLRequest {
        try CoreProtocolBridge.endpointFacts(endpoint) { _, endpointFacts in
            var config = LVProtocolConfig()
            config.endpoint = endpointFacts
            var raw = lv_protocol_make_models_request(&config)
            return try CoreProtocolBridge.request(&raw, cachePolicy: .useProtocolCachePolicy)
        }
    }
    func load(endpoint: String) async throws -> DiscoveredModels {
        let (data, status) = try await transport.send(modelsRequest(endpoint: endpoint))
        try Task.checkCancellation()
        guard (200..<300).contains(status) else { throw DictationError.fromHTTP(status, data: data) }
        var inventory = CoreProtocolBridge.withSlice(data) { lv_protocol_parse_inventory($0) }
        defer { lv_protocol_inventory_free(&inventory) }
        guard inventory.status == LV_PROTOCOL_OK else { throw CoreProtocolBridge.error(status: inventory.status, httpStatus: 0, detail: nil) }
        let reason = inventory.has_reason != 0 ? CoreProtocolBridge.text(inventory.reason).map(SafeDisplay.message) : nil
        var transcription: [LocalModel] = []
        var cleanup: [LocalModel] = []
        if inventory.count != 0 {
            guard let models = inventory.models else { throw DictationError.response }
            for index in 0..<inventory.count {
                let raw = models[index]
                guard let id = CoreProtocolBridge.text(raw.id) else { throw DictationError.response }
                let model = LocalModel(id: id, downloaded: nil, recipe: nil, labels: nil,
                    cloud_provider: raw.remote != 0 ? "remote" : nil, coreASR: raw.asr != 0,
                    coreCleanup: raw.cleanup != 0)
                if raw.asr != 0 { transcription.append(model) }
                if raw.cleanup != 0 { cleanup.append(model) }
            }
        }
        return DiscoveredModels(transcription: transcription.sorted { $0.id < $1.id },
            cleanup: cleanup.sorted { $0.id < $1.id }, reason: reason, version: nil)
    }
    func version(endpoint: String) async -> String? {
        guard var url = try? DictationConfig.endpointURL(endpoint) else { return nil }
        url.deleteLastPathComponent()
        if url.path != "/api" { url.appendPathComponent("api") }
        url.appendPathComponent("v1/health")
        var request = URLRequest(url: url); request.timeoutInterval = 5
        guard let (data, code) = try? await transport.send(request), (200..<300).contains(code) else { return nil }
        var result = CoreProtocolBridge.withSlice(data) { lv_protocol_parse_version($0) }
        defer { lv_protocol_result_free(&result) }
        guard result.status == LV_PROTOCOL_OK, let version = CoreProtocolBridge.text(result.value) else { return nil }
        return SafeDisplay.message(version)
    }

}

// Application-owned reachability monitor. Native networking supplies bytes;
// shared core owns cadence, deadlines, response validation and stale-result policy.
@MainActor final class ServerHealthMonitor {
    private let transport: HTTPTransport
    private let clock: () -> Double
    private var health = LVServerHealth()
    private var endpoint = ""
    private var running = false
    private var timer: Timer?
    private var requestTask: Task<Void, Never>?
    private var nativeRequest: UInt64 = 0
    private var publishedStatus: LVServerHealthStatus?
    var onChange: (LVServerHealthStatus) -> Void = { _ in }
    var status: LVServerHealthStatus { health.status }

    init(transport: HTTPTransport, clock: @escaping () -> Double = { ProcessInfo.processInfo.systemUptime }) {
        self.transport = transport
        self.clock = clock
        lv_server_health_init(&health)
    }
    func start(endpoint: String, polling: Bool = true) {
        running = true
        setEndpoint(endpoint)
        if polling && timer == nil {
            timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
                MainActor.assumeIsolated { self?.tick() }
            }
            if let timer { RunLoop.main.add(timer, forMode: .common) }
        }
    }
    func setEndpoint(_ endpoint: String) {
        let endpoint = endpoint.trimmingCharacters(in: .whitespacesAndNewlines)
        if endpoint != self.endpoint {
            requestTask?.cancel(); requestTask = nil; nativeRequest = 0
            self.endpoint = endpoint
            lv_server_health_reset(&health)
        }
        if running { tick() }
    }
    func stop() {
        running = false
        timer?.invalidate(); timer = nil
        requestTask?.cancel(); requestTask = nil; nativeRequest = 0
        lv_server_health_reset(&health)
    }
    func tick() {
        guard running else { return }
        let token = lv_server_health_begin(&health, clock())
        if requestTask != nil && health.active_request != nativeRequest {
            requestTask?.cancel(); requestTask = nil; nativeRequest = 0
        }
        publish()
        guard token != 0 else { return }
        nativeRequest = token
        let endpoint = self.endpoint
        requestTask = Task { @MainActor [weak self] in
            guard let self else { return }
            var response = Data(), code = 0
            var available: UInt8 = 0
            do {
                var request = try ModelDiscovery(transport: self.transport).modelsRequest(endpoint: endpoint)
                request.timeoutInterval = 3
                request.cachePolicy = .reloadIgnoringLocalCacheData
                (response, code) = try await self.transport.send(request)
                available = 1
            } catch { }
            guard self.running, !Task.isCancelled else { return }
            _ = CoreProtocolBridge.withSlice(response) {
                lv_server_health_complete(&self.health, token, self.clock(), available, Int32(code), $0)
            }
            if self.nativeRequest == token { self.requestTask = nil; self.nativeRequest = 0 }
            self.publish()
        }
    }
    private func publish() {
        guard publishedStatus != health.status else { return }
        publishedStatus = health.status
        onChange(health.status)
    }
}
