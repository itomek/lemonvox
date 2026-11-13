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
actor SetupHTTP: HTTPTransport {
    var requests: [URLRequest] = []
    var installed = false
    var fail = false
    var cancelOnPull = false
    func configure(fail: Bool = false, cancel: Bool = false) { self.fail=fail; self.cancelOnPull=cancel }
    func send(_ request: URLRequest) async throws -> (Data, Int) {
        requests.append(request)
        let path=request.url!.path
        if path.hasSuffix("health") { return (Data(#"{"version":"2026.46.2","all_models_loaded":[]}"#.utf8),200) }
        if path.hasSuffix("models") {
            return (Data("{\"data\":[{\"id\":\"setup-asr\",\"recipe\":\"whispercpp\",\"labels\":[\"transcription\"],\"downloaded\":\(installed)}]}".utf8),200)
        }
        if path.hasSuffix("pull") {
            if cancelOnPull { throw CancellationError() }
            return (Data(#"{"id":"model:setup-asr","status":"downloading","running":true}"#.utf8),200)
        }
        if path.hasSuffix("control") {
            precondition(request.httpMethod == "POST")
            return (Data(#"{"id":"model:setup-asr","status":"cancelled","running":false}"#.utf8),200)
        }
        if path.hasSuffix("downloads") {
            if !requests.contains(where: { $0.url?.path.hasSuffix("pull") == true }) { return (Data("[]".utf8),200) }
            if fail { return (Data(#"[{"id":"model:setup-asr","status":"error","running":false}]"#.utf8),200) }
            installed=true
            return (Data(#"[{"id":"model:setup-asr","status":"completed","running":false,"percent":100}]"#.utf8),200)
        }
        return (Data(),404)
    }
}
private func modelSetupTransportTests() async throws {
    for endpoint in ["http://127.0.0.1:13305/v1", "http://127.0.0.1:13305/api/v1"] {
        let fake=SetupHTTP()
        let client=ModelDiscovery(transport:fake)
        let before=try await client.load(endpoint:endpoint)
        precondition(before.transcription.map(\.id) == ["setup-asr"])
        let catalog=before.setupCatalog!
        precondition(catalog.managementSupported && catalog.info(modelID:"setup-asr",purpose:LV_MODEL_ASR).canInstall)
        let reads=await fake.requests
        precondition(reads.allSatisfy { $0.httpMethod == "GET" })
        precondition(reads.contains { $0.url?.absoluteString == endpoint + "/health" })
        try await client.install(endpoint:endpoint,modelID:"setup-asr",purpose:LV_MODEL_ASR,catalog:catalog,progress:{ _ in })
        let after=try await client.load(endpoint:endpoint)
        precondition(after.setupCatalog!.info(modelID:"setup-asr",purpose:LV_MODEL_ASR).state == LV_SETUP_INSTALLED_NOT_LOADED)
    }
    let fake=SetupHTTP()
    let discovery=ModelDiscovery(transport:fake), endpoint="http://127.0.0.1:13305/v1"
    let models=try await discovery.load(endpoint:endpoint), catalog=models.setupCatalog!
    await fake.configure(fail:true)
    do { try await discovery.install(endpoint:endpoint,modelID:"setup-asr",purpose:LV_MODEL_ASR,catalog:catalog,progress:{ _ in }); fatalError("Failed install succeeded") }
    catch is ModelSetupFailure { }
    await fake.configure(cancel:true)
    do { try await discovery.install(endpoint:endpoint,modelID:"setup-asr",purpose:LV_MODEL_ASR,catalog:catalog,progress:{ _ in }); fatalError("Canceled install succeeded") }
    catch is CancellationError { }
    let requests=await fake.requests
    precondition(requests.contains { $0.url?.path.hasSuffix("downloads/control") == true })
    await fake.configure()
    try await discovery.install(endpoint:endpoint,modelID:"setup-asr",purpose:LV_MODEL_ASR,catalog:catalog,progress:{ _ in })
    print("PASS: setup uses only read-only discovery, preserves endpoint prefixes, verifies installs, reports failure, cancels on the server and retries")
}
@main struct SettingsTests {
    static func main() async throws {
        if CommandLine.arguments.contains("--live-discovery") {
            let http=LocalHTTP(); defer { http.close() }
            let models=try await ModelDiscovery(transport:http).load(endpoint:"http://127.0.0.1:13305/v1")
            guard let catalog=models.setupCatalog else { fatalError("Live server supplied no catalog") }
            precondition(catalog.managementSupported,"Live server management API did not pass the read-only probes")
            precondition(catalog.info(modelID:"Whisper-Base",purpose:LV_MODEL_ASR).state == LV_SETUP_READY)
            precondition(catalog.info(modelID:"Gemma-4-E4B-it-GGUF",purpose:LV_MODEL_CLEANUP).state == LV_SETUP_READY)
            print("PASS: GET-only live Lemonade 2026.46.2 discovery verifies management support and installed, loaded ASR/cleanup states")
            return
        }
        try await modelSetupTransportTests()
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
        let catalogJSON = Data(#"{"data":[{"id":"missing-asr","downloaded":false,"recipe":"whispercpp","labels":["transcription"],"size":1.25},{"id":"ready-chat","downloaded":true,"loaded":false,"recipe":"llamacpp","labels":["chat"]}]}"#.utf8)
        let catalog = ModelSetupCatalog(json: catalogJSON, managementSupported: true)
        precondition(catalog.info(modelID: "missing-asr", purpose: LV_MODEL_ASR).canInstall)
        precondition(catalog.info(modelID: "ready-chat", purpose: LV_MODEL_CLEANUP).state == LV_SETUP_INSTALLED_NOT_LOADED)
        precondition(!catalog.info(modelID: "missing-asr", purpose: LV_MODEL_CLEANUP).canInstall)
        precondition(catalog.info(modelID: "", purpose: LV_MODEL_ASR).state == LV_SETUP_NO_SELECTION)
        precondition(discoveryRequests.allSatisfy { $0.httpMethod == "GET" }, "Discovery must never install or load a model")
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
