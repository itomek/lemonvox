import Foundation

@main struct ProtocolBridgeTests {
    struct Transcript: Decodable { let text: String }
    static func withSlice<Result>(_ value: String, _ body: (LVProtocolSlice) -> Result) -> Result {
        withSlice(Data(value.utf8), body)
    }
    static func withSlice<Result>(_ data: Data, _ body: (LVProtocolSlice) -> Result) -> Result {
        return data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            var slice = LVProtocolSlice()
            slice.data = raw.bindMemory(to: UInt8.self).baseAddress
            slice.size = raw.count
            return body(slice)
        }
    }
    static func text(_ bytes: LVProtocolBytes) -> String {
        guard let pointer = bytes.data, bytes.size > 0 else { return "" }
        return String(decoding: UnsafeBufferPointer(start: pointer, count: bytes.size), as: UTF8.self)
    }
    static func protocolText(_ data: Data) -> String? {
        var result = withSlice(data) { lv_protocol_parse_transcription($0) }
        defer { lv_protocol_result_free(&result) }
        guard result.status == LV_PROTOCOL_OK else { return nil }
        return text(result.value)
    }
    static func jsonSerializationText(_ data: Data) -> String? {
        guard let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        return object["text"] as? String
    }
    static func protocolHTTPDetail(_ message: String) -> String? {
        let data = try! JSONSerialization.data(withJSONObject: ["error": ["message": message]])
        var result = withSlice(data) { lv_protocol_classify_http(401, $0) }
        defer { lv_protocol_result_free(&result) }
        guard result.status == LV_PROTOCOL_UPSTREAM else { return nil }
        return text(result.value)
    }
    static func main() {
        withSlice("http://127.0.0.1:13305/v1") { endpoint in
            withSlice("asr") { asr in
                withSlice("cleanup") { cleanup in
                    withSlice("prompt") { prompt in
                        var facts = LVEndpointFacts()
                        facts.canonical_base_url = endpoint
                        facts.scheme_allowed = 1; facts.host_loopback = 1; facts.no_userinfo = 1
                        facts.no_query_fragment = 1; facts.path_ends_v1 = 1
                        var config = LVProtocolConfig()
                        config.endpoint = facts; config.asr_model = asr; config.cleanup_model = cleanup; config.cleanup_prompt = prompt
                        precondition(lv_protocol_validate_config(&config) == LV_PROTOCOL_OK)
                        var request = lv_protocol_make_models_request(&config)
                        defer { lv_protocol_request_free(&request) }
                        precondition(request.status == LV_PROTOCOL_OK && text(request.url) == "http://127.0.0.1:13305/v1/models")
                    }
                }
            }
        }
        var transcription = withSlice(#"{"text":"  dictated text \n"}"#) { lv_protocol_parse_transcription($0) }
        defer { lv_protocol_result_free(&transcription) }
        precondition(transcription.status == LV_PROTOCOL_OK && text(transcription.value) == "  dictated text \n")
        let source = #"{"text":"x"}"#
        let encodings: [(String, Data)] = [
            ("utf8", Data(source.utf8)),
            ("utf8-bom", Data([0xEF, 0xBB, 0xBF]) + Data(source.utf8)),
            ("utf16le", source.data(using: .utf16LittleEndian)!),
            ("utf16be", source.data(using: .utf16BigEndian)!),
            ("utf16le-bom", Data([0xFF, 0xFE]) + source.data(using: .utf16LittleEndian)!),
            ("utf16be-bom", Data([0xFE, 0xFF]) + source.data(using: .utf16BigEndian)!),
            ("utf32le", source.data(using: .utf32LittleEndian)!),
            ("utf32be", source.data(using: .utf32BigEndian)!),
            ("utf32be-bom", Data([0, 0, 0xFE, 0xFF]) + source.data(using: .utf32BigEndian)!),
            ("utf32le-bom", Data([0xFF, 0xFE, 0, 0]) + source.data(using: .utf32LittleEndian)!)
        ]
        for (name, data) in encodings {
            let decoded = (try? JSONDecoder().decode(Transcript.self, from: data))?.text
            precondition(decoded == jsonSerializationText(data), "Foundation disagreement for \(name)")
            precondition(decoded == protocolText(data), "Protocol disagreement for \(name)")
        }
        for number in ["1e9999", "-1e9999", "1e-9999", "123456789012345678901"] {
            let data = Data("{\"text\":\"x\",\"n\":\(number)}".utf8)
            precondition((try? JSONDecoder().decode(Transcript.self, from: data))?.text == "x")
            precondition(protocolText(data) == "x")
        }
        let typedOverflow = Data(#"{"text":1e9999}"#.utf8)
        precondition((try? JSONDecoder().decode(Transcript.self, from: typedOverflow)) == nil)
        precondition(protocolText(typedOverflow) == nil)
        for secret in ["Bearer\u{00A0}short-secret", "Authorization\u{00A0}:\u{00A0}short-secret", "api\u{00A0}key\u{00A0}=\u{00A0}short-secret"] {
            precondition(protocolHTTPDetail(secret) == SafeDisplay.message(secret))
        }
        print("PASS: Swift bridge owns protocol outputs and preserves C++ request/response policy")
    }
}
