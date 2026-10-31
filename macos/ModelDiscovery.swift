import Foundation

struct ModelInventory: Decodable {
    let data: [LocalModel]
    let reason: String?
    let message: String?
}
struct DiscoveredModels {
    let transcription: [LocalModel]
    let cleanup: [LocalModel]
    let reason: String?
    let version: String?
}
struct ModelDiscovery {
    let transport: HTTPTransport
    func load(endpoint: String) async throws -> DiscoveredModels {
        let base = try DictationConfig.endpointURL(endpoint)
        let (data, status) = try await transport.send(URLRequest(url: base.appendingPathComponent("models")))
        try Task.checkCancellation()
        guard (200..<300).contains(status) else { throw DictationError.fromHTTP(status, data: data) }
        guard let inventory = try? JSONDecoder().decode(ModelInventory.self, from: data) else { throw DictationError.response }
        return DiscoveredModels(transcription: inventory.data.filter { $0.supports(true) }.sorted { $0.id < $1.id },
            cleanup: inventory.data.filter { $0.supports(false) }.sorted { $0.id < $1.id },
            reason: (inventory.reason ?? inventory.message).map(SafeDisplay.message), version: nil)
    }
    func version(endpoint: String) async -> String? {
        guard var url = try? DictationConfig.endpointURL(endpoint) else { return nil }
        url.deleteLastPathComponent()
        if url.path != "/api" { url.appendPathComponent("api") }
        url.appendPathComponent("v1/health")
        var request = URLRequest(url: url); request.timeoutInterval = 5
        guard let (data, code) = try? await transport.send(request), (200..<300).contains(code),
              let object = (try? JSONSerialization.jsonObject(with: data)) as? [String:Any] else { return nil }
        return (object["version"] as? String).map(SafeDisplay.message)
    }

}
