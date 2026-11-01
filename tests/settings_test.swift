import Foundation
actor DiscoveryHTTP: HTTPTransport {
    var count = 0
    var requests: [URLRequest] = []
    let inventory: String
    let delay: UInt64
    init(_ inventory: String, delay: UInt64 = 0) { self.inventory = inventory; self.delay = delay }
    func send(_ request: URLRequest) async throws -> (Data, Int) {
        count += 1; requests.append(request)
        if delay > 0 { try await Task.sleep(nanoseconds: delay) }
        let body = request.url!.path.hasSuffix("models") ? inventory : #"{"version":"2026.44.1","cloud":{"providers":[]}}"#
        return (Data(body.utf8), 200)
    }
}
@main struct SettingsTests {
    static func main() async throws {
        let localAsr = LocalModel(id: "asr", downloaded: true, recipe: "whispercpp", labels: ["transcription"], cloud_provider: nil)
        let localCleanup = LocalModel(id: "cleanup", downloaded: true, recipe: "llamacpp", labels: ["chat"], cloud_provider: nil)
        let remoteCleanup = LocalModel(id: "remote", downloaded: nil, recipe: "cloud", labels: ["chat"], cloud_provider: "provider")
        let missing = LocalModel(id: "missing", downloaded: nil, recipe: nil, labels: nil, cloud_provider: nil)
        precondition(localAsr.supports(true) && !localAsr.supports(false))
        precondition(localCleanup.supports(false) && !localCleanup.supports(true))
        precondition(remoteCleanup.supports(false) && !remoteCleanup.supports(true))
        precondition(!missing.supports(true) && !missing.supports(false))
        let poisoned = try JSONDecoder().decode(LocalModel.self, from: Data(#"{"id":"poisoned","downloaded":false,"coreASR":true,"coreCleanup":true}"#.utf8))
        precondition(!poisoned.supports(true) && !poisoned.supports(false))
        var remoteFacts = LVModelFacts()
        remoteFacts.downloaded = 0
        remoteFacts.has_cloud_provider = 1
        remoteFacts.is_cloud_recipe = 1
        remoteFacts.has_chat_label = 1
        precondition(lv_model_supports(&remoteFacts, LV_MODEL_CLEANUP) != 0)
        precondition(lv_model_supports(&remoteFacts, LV_MODEL_ASR) == 0)
        let fake = DiscoveryHTTP(#"{"data":[{"id":"remote.chat","recipe":"cloud","cloud_provider":"remote","labels":["chat"]},{"id":"gemma","downloaded":true,"recipe":"llamacpp","labels":["chat"]},{"id":"asr","downloaded":true,"recipe":"whispercpp","labels":["transcription"]},{"id":"embed","downloaded":true,"recipe":"llamacpp","labels":["embeddings"]}]}"#)
        let result = try await ModelDiscovery(transport: fake).load(endpoint: "http://127.0.0.1:13305/v1")
        precondition(result.transcription.map(\.id) == ["asr"])
        precondition(result.cleanup.map(\.id) == ["gemma", "remote.chat"])
        let discoveryRequests = await fake.requests
        precondition(discoveryRequests.first?.cachePolicy == .useProtocolCachePolicy && discoveryRequests.first?.httpMethod == "GET")
        let empty = try await ModelDiscovery(transport: DiscoveryHTTP(#"{"data":[],"reason":"No models available"}"#)).load(endpoint:"http://127.0.0.1:13305/v1")
        precondition(empty.reason == "No models available")
        do {
            _ = try await ModelDiscovery(transport: DiscoveryHTTP(#"{"data":[],"reason":"ok","message":123}"#)).load(endpoint:"http://127.0.0.1:13305/v1")
            fatalError("Discovery accepted wrong-typed metadata")
        } catch { }
        let version = await ModelDiscovery(transport: fake).version(endpoint: "http://127.0.0.1:13305/v1")
        precondition(version == "2026.44.1")
        let slow = ModelDiscovery(transport: DiscoveryHTTP(#"{"data":[]}"#, delay: 200_000_000))
        let pending = Task { try await slow.load(endpoint: "http://127.0.0.1:13305/v1") }
        pending.cancel()
        do { _ = try await pending.value; fatalError("Canceled discovery returned") } catch { }
        let upstream = DictationError.fromHTTP(401,data:Data(#"{"error":{"message":"api_key=hidden123 not authorized"}}"#.utf8))
        precondition(!SafeDisplay.error(upstream).contains("hidden123"))
        let unicodeUpstreamData = try JSONSerialization.data(withJSONObject: ["error": ["message": "Bearer\u{00A0}short-secret"]])
        let unicodeUpstream = DictationError.fromHTTP(401, data: unicodeUpstreamData)
        precondition(SafeDisplay.error(unicodeUpstream).contains("[redacted]") && !SafeDisplay.error(unicodeUpstream).contains("short-secret"))
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let log = DiagnosticLog(directory: directory, limit: 300)
        for _ in 0..<50 { log.record(.discoveryFailed, error: DictationError.http(401)) }
        let text = log.contents()
        precondition(text.contains("discovery_failed") && text.contains("http=401"))
        let logSize = try Data(contentsOf: log.file).count
        precondition(logSize <= 600)
        let secret = "Authorization: Bearer sk-supersecret01234567890123456789 api_key=secretvalue"
        let safe = SafeDisplay.message(secret)
        precondition(!safe.contains("supersecret") && !safe.contains("secretvalue"))
        precondition(!SafeDisplay.message("Invalid API key: abc123").contains("abc123"))
        precondition(!SafeDisplay.message("Access token = shortcredential").contains("shortcredential"))
        log.record(.dictationFailed, error: NSError(domain:"private transcript and token",code:7,userInfo:[NSLocalizedDescriptionKey:secret]))
        precondition(!log.contents().contains("supersecret") && !log.contents().contains("private transcript"))
        print("PASS: independent model capabilities/local-cloud choices, empty reason, bounded content-free logs and secret redaction")
    }
}
