import Foundation

@MainActor private final class CaptureFake: WorkflowNativeCapture {
    var error: Error?
    var isRecording = false
    var level = 0.0
    var starts = 0
    var cancels = 0
    func start() async throws {
        starts += 1
        if let error { throw error }
        isRecording = true
    }
    func finish() throws -> Data { isRecording = false; return Data([1]) }
    func cancel() { cancels += 1; isRecording = false }
}

@MainActor private final class TransportFake: WorkflowNativeTransport {
    var requests: [URLRequest] = []
    var closes = 0
    var cleanupError: Error?
    func send(_ request: URLRequest) async throws -> (Data, Int) {
        requests.append(request)
        let body: String
        if request.url!.path.hasSuffix("/models") {
            precondition(request.httpMethod == "GET" && request.httpBody == nil)
            precondition(request.cachePolicy == .reloadIgnoringLocalCacheData)
            body = #"{"data":[{"id":"asr","downloaded":true,"recipe":"whispercpp","labels":["transcription"]},{"id":"cleanup","downloaded":true,"recipe":"llamacpp","labels":["chat"]}]}"#
        } else if request.url!.path.hasSuffix("/transcriptions") {
            body = #"{"text":"synthetic transcript"}"#
        } else {
            if let cleanupError { throw cleanupError }
            body = #"{"choices":[{"message":{"content":"Synthetic transcript."},"finish_reason":"stop"}]}"#
        }
        return (Data(body.utf8), 200)
    }
    func close() { closes += 1 }
}

@MainActor private final class ClipboardFake: ClipboardAccess {
    var text = "prior copy"
    func setText(_ text: String) throws { self.text = text }
}

@MainActor private final class PostingFake: WorkflowNativeEventPosting {
    // A destination may be a different app, an undiscoverable field, or no field.
    // None of those facts are queried by the production adapter.
    var currentFocus = "initial app"
    var destinations: [String] = []
    var available = true
    func postPaste() -> Bool {
        guard available else { return false }
        destinations.append(currentFocus)
        return true
    }
}

@main @MainActor struct WorkflowNativeAdapterTests {
    static func waitUntil(_ predicate: @escaping @MainActor () -> Bool) async {
        for _ in 0..<2000 {
            if predicate() { return }
            try? await Task.sleep(nanoseconds: 1_000_000)
        }
        fatalError("Timed out waiting for native adapter completion")
    }
    private static func validConfig() -> WorkflowBridgeConfig {
        WorkflowBridgeConfig(endpoint: WorkflowBridgeEndpointFacts(canonicalBaseURL: "http://127.0.0.1:13305/v1",
            schemeAllowed: true, hostLoopback: true, noUserinfo: true, noQueryFragment: true, pathEndsV1: true),
            preferenceEndpoint: "http://127.0.0.1:13305/v1", asrModel: "asr", cleanupModel: "cleanup",
            cleanupPrompt: "prompt", insertCurrentTarget: false, pasteAtCurrentFocus: true)
    }
    private static func exercise(destination: String, dispatchAvailable: Bool = true, cancel: Bool = false) async {
        let capture = CaptureFake()
        let transport = TransportFake()
        let clipboard = ClipboardFake()
        let posting = PostingFake()
        posting.available = dispatchAvailable
        var released = false
        let paste = PasteTransaction(clipboard: clipboard, modifiersReleased: { released })
        let adapter = WorkflowNativeAdapter(capture: capture, transport: transport, paste: paste, posting: posting)
        var outcomes: [WorkflowBridgeOutcome] = []
        var texts: [String] = []
        var failures: [WorkflowBridgeFailure] = []
        var callbacks = WorkflowBridgeCallbacks()
        callbacks.completed = { _, result in outcomes.append(result) }
        callbacks.textReady = { _, text in texts.append(text) }
        callbacks.failed = { _, failure in failures.append(failure) }
        let bridge = WorkflowBridge(native: adapter, callbacks: callbacks)
        adapter.attach(bridge)
        let operation = bridge.start(validConfig())
        precondition(operation != 0)
        await waitUntil { capture.isRecording }
        precondition(bridge.stop(boundary: "LemonVox-fixture"))
        precondition(!bridge.stop(boundary: "LemonVox-fixture"), "Stop may finish capture only once")
        await waitUntil { paste.active }
        precondition(clipboard.text == "prior copy" && posting.destinations.isEmpty)
        posting.currentFocus = destination // Move focus after cleanup and before dispatch.
        if cancel { bridge.cancel() }
        released = true
        await waitUntil { !paste.active }
        if cancel {
            precondition(outcomes.isEmpty && texts.isEmpty && posting.destinations.isEmpty)
            precondition(clipboard.text == "prior copy")
        } else {
            await waitUntil { outcomes.count == 1 }
            precondition(failures.isEmpty && clipboard.text == "Synthetic transcript.")
            precondition(capture.starts == 1 && transport.requests.count == 4)
            if dispatchAvailable {
                precondition(outcomes[0].kind == LV_WORKFLOW_OUTCOME_INSERTED)
                precondition(posting.destinations == [destination] && texts.isEmpty)
            } else {
                precondition(outcomes[0].fallbackReason == LV_WORKFLOW_FALLBACK_PASTE_UNAVAILABLE)
                precondition(texts == ["Synthetic transcript."] && posting.destinations.isEmpty)
            }
        }
        bridge.close()
        precondition(capture.cancels > 0 && transport.closes == 1)
        adapter.prepareForShutdown()
        precondition(transport.closes == 1, "Shutdown must be idempotent")
    }
    private static func captureFailuresAndEmergencyCleanup() async {
        for (failure, expected) in [(DictationError.microphone, LV_WORKFLOW_ADAPTER_UNAVAILABLE),
                                    (DictationError.recording, LV_WORKFLOW_ADAPTER_INVALID_INPUT)] {
            let capture = CaptureFake()
            capture.error = failure
            let transport = TransportFake()
            let adapter = WorkflowNativeAdapter(capture: capture, transport: transport,
                paste: PasteTransaction(clipboard: ClipboardFake()), posting: PostingFake())
            var failures: [WorkflowBridgeFailure] = []
            var callbacks = WorkflowBridgeCallbacks()
            callbacks.failed = { _, failure in failures.append(failure) }
            let bridge = WorkflowBridge(native: adapter, callbacks: callbacks)
            adapter.attach(bridge)
            precondition(bridge.start(validConfig()) != 0)
            await waitUntil { !failures.isEmpty }
            precondition(failures[0].adapterError == expected)
            bridge.close()
        }
        let capture = CaptureFake()
        let transport = TransportFake()
        var adapter: WorkflowNativeAdapter? = WorkflowNativeAdapter(capture: capture, transport: transport,
            paste: PasteTransaction(clipboard: ClipboardFake()), posting: PostingFake())
        weak let released = adapter
        adapter!.abandonWithoutBridge()
        adapter!.abandonWithoutBridge()
        adapter = nil
        await waitUntil { released == nil }
        precondition(capture.cancels == 1 && transport.closes == 1,
            "Emergency teardown must retain native state until idempotent cleanup completes")
    }
    static func main() async {
        let capture = CaptureFake()
        let transport = TransportFake()
        transport.cleanupError = DictationError.serverTimeout
        let adapter = WorkflowNativeAdapter(capture: capture, transport: transport,
            paste: PasteTransaction(clipboard: ClipboardFake()), posting: PostingFake())
        var timeoutFailures: [WorkflowBridgeFailure] = []
        var timeoutCallbacks = WorkflowBridgeCallbacks()
        timeoutCallbacks.failed = { _, failure in timeoutFailures.append(failure) }
        let bridge = WorkflowBridge(native: adapter, callbacks: timeoutCallbacks)
        adapter.attach(bridge)
        precondition(bridge.start(validConfig()) != 0)
        await waitUntil { capture.isRecording }
        precondition(bridge.stop(boundary: "LemonVox-timeout"))
        await waitUntil { !timeoutFailures.isEmpty }
        precondition(timeoutFailures[0].adapterError == LV_WORKFLOW_ADAPTER_TIMEOUT)
        precondition(DictationError.serverTimeout.errorDescription!.contains("timed out"))
        bridge.close()
        await captureFailuresAndEmergencyCleanup()
        for destination in ["another native field", "different browser app", "Electron without AX field", "no selected field"] {
            await exercise(destination: destination)
        }
        await exercise(destination: "dispatch denied", dispatchAvailable: false)
        await exercise(destination: "canceled", cancel: true)
        print("PASS: production core/native adapter pastes once at final focus without AX, cancels queued paste and offers Copy on dispatch denial")
    }
}
