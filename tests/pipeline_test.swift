import Foundation

actor FakeHTTP: HTTPTransport {
    var requests: [URLRequest] = []
    var responses: [(Int, String)]
    let delay: UInt64
    let delayAt: Int?
    init(_ responses: [(Int, String)], delay: UInt64 = 0, delayAt: Int? = nil) {
        self.responses = responses; self.delay = delay; self.delayAt = delayAt
    }
    func send(_ request: URLRequest) async throws -> (Data, Int) {
        requests.append(request)
        if delay > 0 && (delayAt == nil || requests.count == delayAt) { try await Task.sleep(nanoseconds: delay) }
        guard !responses.isEmpty else { throw DictationError.response }
        let reply = responses.removeFirst()
        return (Data(reply.1.utf8), reply.0)
    }
}
@main struct PipelineTests {
    static let inventory = #"{"data":[{"id":"asr","downloaded":true,"recipe":"whispercpp","labels":["transcription"]},{"id":"chat","downloaded":true,"recipe":"llamacpp","labels":["chat"]}]}"#
    static func config(_ endpoint: String = "http://127.0.0.1:13305/v1") -> DictationConfig {
        DictationConfig(endpoint: endpoint, asrModel: "asr", cleanupModel: "chat", cleanupPrompt: DictationConfig.defaultPrompt)
    }
    static func rejected(_ body: () async throws -> Void) async {
        do { try await body(); fatalError("Expected rejection") } catch { }
    }
    static func main() async throws {
        for endpoint in ["https://example.com/v1", "http://127.0.0.1.evil.test/v1", "http://user:pass@127.0.0.1/v1", "http://127.0.0.1/v1?x=1", "http://127.0.0.1/v1#fragment", "http://127.0.0.1/v2"] {
            await rejected { _ = try config(endpoint).baseURL() }
        }
        for endpoint in ["http://127.0.0.1:13305/v1", "https://localhost/v1", "http://[::1]:13305/v1"] {
            _ = try config(endpoint).baseURL()
        }
        await rejected { _ = try DictationConfig(endpoint: "http://127.0.0.1:13305/v1", asrModel: "asr\nnext", cleanupModel: "chat", cleanupPrompt: "prompt").baseURL() }
        await rejected { _ = try DictationConfig(endpoint: "http://127.0.0.1:13305/v1", asrModel: "", cleanupModel: "chat", cleanupPrompt: "\n").baseURL() }
        _ = try DictationConfig(endpoint: "http://127.0.0.1:13305/v1", asrModel: "asr\0id", cleanupModel: "chat", cleanupPrompt: "prompt").baseURL()
        let transport = FakeHTTP([(200, inventory), (200, #"{"text":"um send Alex 14 invoices"}"#),
            (200, #"{"choices":[{"message":{"content":"Send Alex 14 invoices."},"finish_reason":"stop"}]}"#)])
        let result = try await LemonadeClient(config: config(), transport: transport).process(wav: Data("RIFF-test-wave".utf8))
        precondition(result == "Send Alex 14 invoices.")
        let requests = await transport.requests
        precondition(requests.count == 3 && requests[0].httpBody == nil && requests[0].httpMethod == "GET" && requests[1].url!.path == "/v1/audio/transcriptions")
        let multipart = String(data: requests[1].httpBody!, encoding: .utf8)!
        precondition(multipart.contains("audio/wav") && multipart.contains("RIFF-test-wave") && multipart.contains("name=\"model\""))
        let payload = try JSONSerialization.jsonObject(with: requests[2].httpBody!) as! [String: Any]
        let messages = payload["messages"] as! [[String: String]]
        precondition(messages[0]["role"] == "system" && messages[1]["content"] == "um send Alex 14 invoices")
        precondition(payload["tools"] == nil && payload["stream"] as? Bool == false)
        let metadataIgnored = String(inventory.dropLast()) + #", "reason":"ok","message":123}"#
        try await LemonadeClient(config: config(), transport: FakeHTTP([(200, metadataIgnored)])).validateModels()
        let whitespaceTransport = FakeHTTP([(200, inventory), (200, #"{"text":"  exact transcript  "}"#),
            (200, #"{"choices":[{"message":{"content":"Cleaned"},"finish_reason":"stop"}]}"#)])
        _ = try await LemonadeClient(config: config(), transport: whitespaceTransport).process(wav: Data([1]))
        let whitespaceRequests = await whitespaceTransport.requests
        let whitespacePayload = try JSONSerialization.jsonObject(with: whitespaceRequests[2].httpBody!) as! [String: Any]
        precondition((whitespacePayload["messages"] as! [[String: String]])[1]["content"] == "  exact transcript  ")
        for badInventory in [
            inventory.replacingOccurrences(of: "whispercpp", with: "cloud"),
            inventory.replacingOccurrences(of: "llamacpp", with: "collection.router"),
            inventory.replacingOccurrences(of: "true", with: "false"),
            inventory.replacingOccurrences(of: "[\"chat\"]", with: "[\"embeddings\"]"),
            inventory.replacingOccurrences(of: "\"recipe\":\"llamacpp\"", with: "\"recipe\":\"llamacpp\",\"cloud_provider\":\"provider\"")
        ] {
            let fake = FakeHTTP([(200, badInventory)])
            await rejected { _ = try await LemonadeClient(config: config(), transport: fake).process(wav: Data([1])) }
            let count = await fake.requests.count; precondition(count == 1, "Rejected models must not send audio")
        }
        for reply in [(500, "private server error"), (200, "invalid JSON"), (200, #"{"text":" "}"#)] {
            let fake = FakeHTTP([(200, inventory), reply])
            await rejected { _ = try await LemonadeClient(config: config(), transport: fake).process(wav: Data([1])) }
        }
        for content in [#"{"choices":[{"message":{"content":"Partial"},"finish_reason":"length"}]}"#,
                        #"{"choices":[{"message":{"content":" "},"finish_reason":"stop"}]}"#,
                        #"{"choices":[{"message":{"content":"First"},"finish_reason":"stop"},{"message":{"content":7},"finish_reason":"stop"}]}"#] {
            let fake = FakeHTTP([(200, inventory), (200, #"{"text":"test"}"#), (200, content)])
            await rejected { _ = try await LemonadeClient(config: config(), transport: fake).process(wav: Data([1])) }
        }
        let slow = FakeHTTP([(200, inventory)], delay: 500_000_000)
        let task = Task { try await LemonadeClient(config: config(), transport: slow).process(wav: Data([1])) }
        try await Task.sleep(nanoseconds: 20_000_000)
        task.cancel()
        await rejected { _ = try await task.value }
        let slowCount = await slow.requests.count; precondition(slowCount == 1)
        for stage in [2, 3] {
            let fake = FakeHTTP([(200, inventory), (200, #"{"text":"test"}"#),
                (200, #"{"choices":[{"message":{"content":"Test."},"finish_reason":"stop"}]}"#)], delay: 500_000_000, delayAt: stage)
            let pending = Task { try await LemonadeClient(config: config(), transport: fake).process(wav: Data([1])) }
            for _ in 0..<1000 {
                if await fake.requests.count >= stage { break }
                try await Task.sleep(nanoseconds: 1_000_000)
            }
            pending.cancel()
            await rejected { _ = try await pending.value }
            let count = await fake.requests.count
            precondition(count == stage, "Cancel must suppress subsequent stages")
        }
        let audio = try TransientAudio()
        let directory = audio.directory
        try Data([1,2,3]).write(to: audio.url)
        audio.remove()
        precondition(!FileManager.default.fileExists(atPath: directory.path))
        print("PASS: local-only configuration/models, WAV request, cleanup data boundary, errors, cancellation, transient cleanup")
    }
}
