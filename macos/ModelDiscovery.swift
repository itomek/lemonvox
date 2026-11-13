import Foundation

struct DiscoveredModels {
    let transcription: [LocalModel]
    let cleanup: [LocalModel]
    let reason: String?
    let version: String?
    var setupCatalog: ModelSetupCatalog? = nil
}

struct ModelSetupInfo {
    let state: LVModelSetupState
    let canInstall: Bool
    let message: String
}
struct ModelSetupCatalog {
    let json: Data
    let managementSupported: Bool
    var healthJSON = Data()
    func withInventory<Result>(_ body: (inout LVProtocolInventory) throws -> Result) rethrows -> Result {
        var inventory = CoreProtocolBridge.withSlice(json) { lv_protocol_parse_catalog($0) }
        defer { lv_protocol_inventory_free(&inventory) }
        if !healthJSON.isEmpty {
            _ = CoreProtocolBridge.withSlice(healthJSON) { lv_protocol_catalog_apply_health(&inventory, $0) }
        }
        return try body(&inventory)
    }
    func info(modelID: String, purpose: LVModelPurpose, serverAvailable: Bool = true) -> ModelSetupInfo {
        withInventory { inventory in
            var result = CoreProtocolBridge.withSlice(modelID) {
                lv_protocol_model_setup(&inventory, $0, purpose, serverAvailable ? 1 : 0, managementSupported ? 1 : 0)
            }
            defer { lv_protocol_model_setup_free(&result) }
            return ModelSetupInfo(state: result.state, canInstall: result.can_install != 0,
                message: SafeDisplay.message(CoreProtocolBridge.text(result.message) ?? "Model setup unavailable."))
        }
    }
    func sizeGB(modelID: String) -> Double? {
        withInventory { inventory in
            guard let models = inventory.models else { return nil }
            for index in 0..<inventory.count where CoreProtocolBridge.text(models[index].id) == modelID {
                return models[index].size_gb > 0 ? models[index].size_gb : nil
            }
            return nil
        }
    }
}
struct ModelInstallUpdate {
    let percent: Double
    let message: String
}
struct ModelSetupFailure: LocalizedError {
    let message: String
    var errorDescription: String? { message }
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
    private func managementRequest(endpoint: String, make: (UnsafePointer<LVProtocolConfig>) -> LVProtocolRequest) throws -> URLRequest {
        try CoreProtocolBridge.endpointFacts(endpoint) { _, facts in
            var config = LVProtocolConfig(); config.endpoint = facts
            var raw = make(&config)
            return try CoreProtocolBridge.request(&raw)
        }
    }
    private func healthRequest(endpoint: String) throws -> URLRequest {
        var url = try DictationConfig.endpointURL(endpoint)
        url.appendPathComponent("health")
        var request = URLRequest(url: url); request.timeoutInterval = 5
        return request
    }
    private func checked(_ request: URLRequest) async throws -> Data {
        let (data, status) = try await transport.send(request)
        try Task.checkCancellation()
        guard (200..<300).contains(status) else { throw DictationError.fromHTTP(status, data: data) }
        return data
    }
    func load(endpoint: String) async throws -> DiscoveredModels {
        let installedJSON = try await checked(modelsRequest(endpoint: endpoint))
        var catalogJSON = installedJSON
        var managementSupported = false
        var healthJSON = Data()
        // All discovery requests are read-only; a newer API is enabled only after
        // its version, catalog and job-status response have been checked.
        do {
            healthJSON = try await checked(healthRequest(endpoint: endpoint))
            if CoreProtocolBridge.withSlice(healthJSON, { lv_protocol_supports_model_management($0) }) != 0 {
                let candidate = try await checked(managementRequest(endpoint: endpoint, make: lv_protocol_make_catalog_request))
                let jobs = try await checked(managementRequest(endpoint: endpoint, make: lv_protocol_make_downloads_request))
                var progress = CoreProtocolBridge.withSlice(jobs) { bytes in
                    CoreProtocolBridge.withSlice("") { lv_protocol_parse_install_progress_for_model(bytes, $0) }
                }
                defer { lv_protocol_install_progress_free(&progress) }
                var catalog = CoreProtocolBridge.withSlice(candidate) { lv_protocol_parse_catalog($0) }
                defer { lv_protocol_inventory_free(&catalog) }
                if catalog.status == LV_PROTOCOL_OK && progress.status == LV_PROTOCOL_OK {
                    catalogJSON = candidate; managementSupported = true
                }
            }
        } catch is CancellationError { throw CancellationError() }
        catch { /* Generic servers retain ordinary installed inventory and help. */ }
        let setup = ModelSetupCatalog(json: catalogJSON, managementSupported: managementSupported, healthJSON: healthJSON)
        return try setup.withInventory { inventory in
            guard inventory.status == LV_PROTOCOL_OK else { throw DictationError.response }
            let reason = inventory.has_reason != 0 ? CoreProtocolBridge.text(inventory.reason).map(SafeDisplay.message) : nil
            var transcription: [LocalModel] = [], cleanup: [LocalModel] = []
            if let models = inventory.models {
                for index in 0..<inventory.count {
                    let raw = models[index]
                    guard let id = CoreProtocolBridge.text(raw.id) else { throw DictationError.response }
                    let model = LocalModel(id: id, downloaded: raw.downloaded != 0, recipe: nil, labels: nil,
                        cloud_provider: raw.remote != 0 ? "remote" : nil, coreASR: raw.asr != 0, coreCleanup: raw.cleanup != 0)
                    if raw.asr != 0 { transcription.append(model) }
                    if raw.cleanup != 0 { cleanup.append(model) }
                }
            }
            return DiscoveredModels(transcription: transcription.sorted { $0.id < $1.id },
                cleanup: cleanup.sorted { $0.id < $1.id }, reason: reason, version: nil, setupCatalog: setup)
        }
    }
    func version(endpoint: String) async -> String? {
        guard let request = try? healthRequest(endpoint: endpoint), let data = try? await checked(request) else { return nil }
        var result = CoreProtocolBridge.withSlice(data) { lv_protocol_parse_version($0) }
        defer { lv_protocol_result_free(&result) }
        guard result.status == LV_PROTOCOL_OK, let version = CoreProtocolBridge.text(result.value) else { return nil }
        return SafeDisplay.message(version)
    }
    func install(endpoint: String, modelID: String, purpose: LVModelPurpose, catalog: ModelSetupCatalog,
                 progress: @escaping (ModelInstallUpdate) async -> Void) async throws {
        let request = try catalog.withInventory { inventory in
            try CoreProtocolBridge.endpointFacts(endpoint) { _, facts in
                var config = LVProtocolConfig(); config.endpoint = facts
                return try CoreProtocolBridge.withSlice(modelID) {
                    var raw = lv_protocol_make_install_request(&config, &inventory, $0, purpose,
                        catalog.managementSupported ? 1 : 0, 1)
                    return try CoreProtocolBridge.request(&raw)
                }
            }
        }
        do {
            var data = try await checked(request)
            var missingPolls = 0
            while true {
                try Task.checkCancellation()
                var raw = CoreProtocolBridge.withSlice(data) { bytes in
                    CoreProtocolBridge.withSlice(modelID) { lv_protocol_parse_install_progress_for_model(bytes, $0) }
                }
                let update = ModelInstallUpdate(percent: raw.percent,
                    message: SafeDisplay.message(CoreProtocolBridge.text(raw.message) ?? "Downloading model through the inference server…"))
                let failed = raw.failed != 0, canceled = raw.canceled != 0, complete = raw.complete != 0
                let status = raw.status, running = raw.running != 0
                lv_protocol_install_progress_free(&raw)
                guard status == LV_PROTOCOL_OK else { throw DictationError.response }
                await progress(update)
                if canceled { throw CancellationError() }
                if failed { throw ModelSetupFailure(message: update.message) }
                if complete { return }
                missingPolls = running ? 0 : missingPolls + 1
                guard missingPolls <= 60 else {
                    throw ModelSetupFailure(message:"The server no longer reports this download. Refresh the catalog and retry when the server is available.")
                }
                try await Task.sleep(nanoseconds: 500_000_000)
                data = try await checked(managementRequest(endpoint:endpoint,make:lv_protocol_make_downloads_request))
            }
        } catch is CancellationError {
            // Cancellation of the local task also asks the server to stop its job.
            let cancellation = Task { try await cancelInstall(endpoint: endpoint, modelID: modelID, purpose: purpose, catalog: catalog) }
            do { try await cancellation.value }
            catch { throw ModelSetupFailure(message:"Local monitoring stopped, but server cancellation is unconfirmed. Check the server’s Downloads page before retrying.") }
            throw CancellationError()
        }
    }
    func cancelInstall(endpoint: String, modelID: String, purpose: LVModelPurpose, catalog: ModelSetupCatalog) async throws {
        let request = try catalog.withInventory { inventory in
            try CoreProtocolBridge.endpointFacts(endpoint) { _, facts in
                var config = LVProtocolConfig(); config.endpoint = facts
                return try CoreProtocolBridge.withSlice(modelID) {
                    var raw = lv_protocol_make_cancel_install_request(&config, &inventory, $0, purpose, catalog.managementSupported ? 1 : 0)
                    return try CoreProtocolBridge.request(&raw)
                }
            }
        }
        var data = try await checked(request)
        for _ in 0..<60 {
            var raw = CoreProtocolBridge.withSlice(data) { bytes in
                CoreProtocolBridge.withSlice(modelID) { lv_protocol_parse_install_progress_for_model(bytes,$0) }
            }
            let valid=raw.status == LV_PROTOCOL_OK
            let stopped=raw.running == 0 && (raw.canceled != 0 || raw.complete != 0 || raw.failed != 0)
            lv_protocol_install_progress_free(&raw)
            guard valid else { throw DictationError.response }
            if stopped { return }
            try await Task.sleep(nanoseconds:500_000_000)
            data = try await checked(managementRequest(endpoint:endpoint,make:lv_protocol_make_downloads_request))
        }
        throw ModelSetupFailure(message:"The server is still stopping the download. Check its Downloads page before retrying.")
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
