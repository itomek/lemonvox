import Foundation

struct DiscoveredModels {
    let transcription: [LocalModel]
    let cleanup: [LocalModel]
    let reason: String?
    let version: String?
}
struct ModelDiscovery {
    let transport: HTTPTransport
    private func modelsRequest(endpoint: String) throws -> URLRequest {
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
