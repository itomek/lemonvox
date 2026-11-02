import Foundation

@MainActor private final class CompletionSpy: WorkflowNativeCompleting {
    var originals: [(UInt64, UInt64, LVWorkflowAdapterError, WorkflowBridgeTarget)] = []
    var captureStarts: [(UInt64, UInt64, LVWorkflowAdapterError)] = []
    var transports: [(UInt64, UInt64, LVWorkflowAdapterError, Int, Data)] = []
    var inspects: [(UInt64, UInt64, LVWorkflowAdapterError, WorkflowBridgeTarget)] = []
    var inserts: [(UInt64, UInt64, LVWorkflowAdapterError)] = []

    @discardableResult func completeOriginal(operation: UInt64, requestID: UInt64,
        error: LVWorkflowAdapterError, target: WorkflowBridgeTarget) -> Bool {
        originals.append((operation, requestID, error, target))
        return true
    }

    @discardableResult func completeCaptureStart(operation: UInt64, requestID: UInt64,
        error: LVWorkflowAdapterError) -> Bool {
        captureStarts.append((operation, requestID, error))
        return true
    }

    @discardableResult func completeCaptureStop(operation: UInt64, requestID: UInt64,
        error: LVWorkflowAdapterError, wav: Data) -> Bool { true }

    @discardableResult func completeTransport(operation: UInt64, requestID: UInt64,
        error: LVWorkflowAdapterError, httpStatus: Int, body: Data) -> Bool {
        transports.append((operation, requestID, error, httpStatus, body))
        return true
    }

    @discardableResult func completeInspect(operation: UInt64, requestID: UInt64,
        error: LVWorkflowAdapterError, target: WorkflowBridgeTarget) -> Bool {
        inspects.append((operation, requestID, error, target))
        return true
    }

    @discardableResult func completeInsert(operation: UInt64, requestID: UInt64,
        error: LVWorkflowAdapterError) -> Bool {
        inserts.append((operation, requestID, error))
        return true
    }
}

@MainActor private final class CaptureFake: WorkflowNativeCapture {
    var error: Error?
    var isRecording = false
    var level = 0.0
    var starts = 0
    var cancels = 0
    var hasTransientWAV = false

    func start() async throws {
        starts += 1
        if let error { throw error }
        isRecording = true
    }

    func finish() throws -> Data { isRecording = false; return Data([1]) }
    func cancel() { cancels += 1; isRecording = false; hasTransientWAV = false }
}

@MainActor private final class TransportFake: WorkflowNativeTransport {
    var requests: [URLRequest] = []
    var closes = 0
    var active = false

    func send(_ request: URLRequest) async throws -> (Data, Int) {
        requests.append(request)
        return (Data("{}".utf8), 200)
    }

    func close() { closes += 1; active = false }
}

@MainActor private final class PasteFake: WorkflowNativePaste {
    var active = false
    var inserts = 0

    func insert(_ text: String, validate: () -> Bool, dispatch: () -> Bool) async -> PasteResult {
        inserts += 1
        guard validate() else { return .targetChanged }
        return dispatch() ? .dispatched : .unavailable
    }
}

@MainActor private final class TargetingFake: WorkflowNativeTargeting {
    var accessibilityTrusted = true
    var releaseCalls = 0
    var released = Set<UInt64>()
    var focused: WorkflowNativeTarget?
    var posts: [pid_t] = []
    func focusedTarget() -> WorkflowNativeTarget? { focused }
    func targetsMatch(_ left: WorkflowNativeTarget, _ right: WorkflowNativeTarget) -> Bool { left.identity == right.identity }
    func frontmostPID() -> pid_t? { nil }
    func postPaste(to pid: pid_t) -> Bool { posts.append(pid); return true }
    func release(_ target: WorkflowNativeTarget) {
        releaseCalls += 1
        released.insert(target.identity)
    }
}

@main @MainActor struct WorkflowNativeAdapterTests {
    static func waitUntil(_ predicate: @escaping @MainActor () -> Bool) async {
        for _ in 0..<1000 {
            if predicate() { return }
            await Task.yield()
        }
        fatalError("Timed out waiting for native adapter completion")
    }

    private static func makeAdapter(capture: CaptureFake, transport: TransportFake) -> (WorkflowNativeAdapter, CompletionSpy) {
        let adapter = WorkflowNativeAdapter(capture: capture, transport: transport,
            paste: PasteFake(), targeting: TargetingFake())
        let completion = CompletionSpy()
        adapter.attach(completion)
        return (adapter, completion)
    }

    private static func validConfig() -> WorkflowBridgeConfig {
        WorkflowBridgeConfig(
            endpoint: WorkflowBridgeEndpointFacts(canonicalBaseURL: "http://127.0.0.1:13305/v1",
                schemeAllowed: true, hostLoopback: true, noUserinfo: true,
                noQueryFragment: true, pathEndsV1: true),
            preferenceEndpoint: "http://127.0.0.1:13305/v1", asrModel: "asr",
            cleanupModel: "cleanup", cleanupPrompt: "prompt", insertCurrentTarget: false)
    }

    private static func assertChangedTargetDoesNotPaste(secure: Bool, editable: Bool) async {
        let capture = CaptureFake()
        let transport = TransportFake()
        let paste = PasteFake()
        let targeting = TargetingFake()
        let adapter = WorkflowNativeAdapter(capture: capture, transport: transport,
            paste: paste, targeting: targeting)
        let completion = CompletionSpy()
        adapter.attach(completion)
        let identity: UInt64 = secure ? 60 : 61
        let original = WorkflowNativeTarget(identity: identity, pid: 77, editable: true, secure: false)
        adapter.prepareForStart(original: original, originalPID: original.pid, useCurrentField: false)
        adapter.captureOriginal(operation: identity, requestID: identity + 1)
        let token = completion.originals[0].3.token
        targeting.focused = WorkflowNativeTarget(identity: identity, pid: 77,
            editable: editable, secure: secure)
        adapter.inspectTarget(operation: identity, requestID: identity + 2, originalToken: token)
        guard let inspected = completion.inspects.last else {
            fatalError("Expected inspected target metadata")
        }
        precondition(inspected.2 == LV_WORKFLOW_ADAPTER_NONE)
        precondition(inspected.3.token == token)
        precondition(inspected.3.secure == secure && inspected.3.editable == editable,
            "Original-mode inspection must report current target metadata")
        adapter.insertText(operation: identity, requestID: identity + 3, targetToken: token, text: "private")
        await waitUntil { !completion.inserts.isEmpty }
        precondition(completion.inserts[0].2 == LV_WORKFLOW_ADAPTER_TARGET_CHANGED)
        precondition(targeting.posts.isEmpty,
            "Changed secure/editability metadata must prevent paste dispatch")
    }

    static func main() async {
        // Distinguish an original target never captured from a completion-time
        // lookup failure. These are synthetic adapter facts, not an AX repro.
        let missingTargeting = TargetingFake()
        let missingPaste = PasteFake()
        let missingAdapter = WorkflowNativeAdapter(capture: CaptureFake(), transport: TransportFake(),
            paste: missingPaste, targeting: missingTargeting)
        let missingCompletion = CompletionSpy()
        missingAdapter.attach(missingCompletion)
        missingAdapter.prepareForStart(original: nil, originalPID: 77, useCurrentField: true)
        missingAdapter.captureOriginal(operation: 70, requestID: 71)
        precondition(missingCompletion.originals.last?.2 == LV_WORKFLOW_ADAPTER_UNAVAILABLE)
        missingAdapter.inspectTarget(operation: 70, requestID: 72, originalToken: 0)
        precondition(missingCompletion.inspects.last?.2 == LV_WORKFLOW_ADAPTER_UNAVAILABLE)
        precondition(missingPaste.inserts == 0 && missingTargeting.posts.isEmpty)
        missingTargeting.focused = WorkflowNativeTarget(identity: 42, pid: 77, editable: true, secure: false)
        missingAdapter.inspectTarget(operation: 70, requestID: 73, originalToken: 0)
        precondition(missingCompletion.inspects.last?.2 == LV_WORKFLOW_ADAPTER_NONE)
        precondition(missingCompletion.inspects.last?.3.token != 0)
        precondition(missingPaste.inserts == 0 && missingTargeting.posts.isEmpty)
        missingAdapter.finalizeOperation(70)
        precondition(missingTargeting.released == Set([42]))

        let capture = CaptureFake()
        let transport = TransportFake()
        let (adapter, completion) = makeAdapter(capture: capture, transport: transport)

        adapter.transportSend(operation: 7, requestID: 11, request: WorkflowBridgeTransportRequest(
            method: LV_WORKFLOW_METHOD_GET, url: "http://127.0.0.1:13305/v1/models",
            body: Data(), contentType: nil, timeoutMS: 0))
        await waitUntil { !completion.transports.isEmpty }
        precondition(transport.requests.count == 1)
        precondition(transport.requests[0].httpMethod == "GET")
        precondition(transport.requests[0].httpBody == nil, "Empty GET must retain nil HTTP body")
        precondition(transport.requests[0].cachePolicy == .reloadIgnoringLocalCacheData)
        precondition(completion.transports[0].2 == LV_WORKFLOW_ADAPTER_NONE)

        capture.error = DictationError.microphone
        adapter.captureStart(operation: 8, requestID: 12)
        await waitUntil { completion.captureStarts.count == 1 }
        precondition(completion.captureStarts[0].2 == LV_WORKFLOW_ADAPTER_UNAVAILABLE,
            "Microphone denial must keep its specific workflow category")

        capture.error = DictationError.recording
        adapter.captureStart(operation: 9, requestID: 13)
        await waitUntil { completion.captureStarts.count == 2 }
        precondition(completion.captureStarts[1].2 == LV_WORKFLOW_ADAPTER_INVALID_INPUT,
            "Recorder/device failure must map to recording, not microphone denial")

        // A secure target is a successful target capture whose secure fact must
        // terminate the real bridge/core before models or microphone capture.
        let secureCapture = CaptureFake()
        let secureTransport = TransportFake()
        let secureTargeting = TargetingFake()
        let secureAdapter = WorkflowNativeAdapter(capture: secureCapture, transport: secureTransport,
            paste: PasteFake(), targeting: secureTargeting)
        var secureFailures: [WorkflowBridgeFailure] = []
        var secureCallbacks = WorkflowBridgeCallbacks()
        secureCallbacks.failed = { _, failure in secureFailures.append(failure) }
        let secureBridge = WorkflowBridge(native: secureAdapter, callbacks: secureCallbacks)
        secureAdapter.attach(secureBridge)
        let secureTarget = WorkflowNativeTarget(identity: 30, pid: 77, editable: true, secure: true)
        secureAdapter.prepareForStart(original: secureTarget, originalPID: secureTarget.pid, useCurrentField: false)
        let secureOperation = secureBridge.start(validConfig())
        precondition(secureOperation != 0)
        precondition(secureFailures.count == 1)
        precondition(secureFailures[0].source == LV_WORKFLOW_FAILURE_ADAPTER)
        precondition(secureFailures[0].adapterError == LV_WORKFLOW_ADAPTER_INSERTION_UNAVAILABLE)
        precondition(secureCapture.starts == 0 && secureTransport.requests.isEmpty,
            "Secure target must fail before microphone capture or model transport")
        secureAdapter.finalizeOperation(secureOperation)
        secureBridge.close()

        await assertChangedTargetDoesNotPaste(secure: true, editable: true)
        await assertChangedTargetDoesNotPaste(secure: false, editable: false)

        adapter.abandonWithoutBridge()
        await waitUntil { capture.cancels > 0 && transport.closes > 0 }
        let firstCaptureShutdowns = capture.cancels
        let firstTransportShutdowns = transport.closes
        adapter.abandonWithoutBridge()
        await Task.yield()
        await Task.yield()
        precondition(capture.cancels == firstCaptureShutdowns && transport.closes == firstTransportShutdowns,
            "Repeated emergency teardown must not repeat native shutdown side effects")

        // Replacing a synchronous pre-start snapshot must not retain its AX
        // target. Repeated discard is deliberately harmless.
        let preparedTargeting = TargetingFake()
        let preparedAdapter = WorkflowNativeAdapter(capture: CaptureFake(), transport: TransportFake(),
            paste: PasteFake(), targeting: preparedTargeting)
        let firstPrepared = WorkflowNativeTarget(identity: 40, pid: 77, editable: true, secure: false)
        let secondPrepared = WorkflowNativeTarget(identity: 41, pid: 77, editable: true, secure: false)
        preparedAdapter.prepareForStart(original: firstPrepared, originalPID: 77, useCurrentField: false)
        preparedAdapter.prepareForStart(original: secondPrepared, originalPID: 77, useCurrentField: false)
        preparedAdapter.discardPreparedStart()
        preparedAdapter.discardPreparedStart()
        let finalPrepared = WorkflowNativeTarget(identity: 42, pid: 77, editable: true, secure: false)
        preparedAdapter.prepareForStart(original: finalPrepared, originalPID: 77, useCurrentField: false)
        preparedAdapter.finalizeOperation(999)
        preparedAdapter.finalizeOperation(999)
        precondition(preparedTargeting.released == Set([40, 41, 42]))
        precondition(preparedTargeting.releaseCalls == 3,
            "Prepared-target overwrite and discard must each release once")

        // A target token belongs to exactly one operation. A stale token must
        // not dispatch a paste for another operation, and terminal finalizing
        // must release both the context's original and its allocated token.
        let ownershipCapture = CaptureFake()
        let ownershipTransport = TransportFake()
        let ownershipPaste = PasteFake()
        let ownershipTargeting = TargetingFake()
        let ownershipAdapter = WorkflowNativeAdapter(capture: ownershipCapture,
            transport: ownershipTransport, paste: ownershipPaste, targeting: ownershipTargeting)
        let ownershipCompletion = CompletionSpy()
        ownershipAdapter.attach(ownershipCompletion)
        let firstOwned = WorkflowNativeTarget(identity: 50, pid: 77, editable: true, secure: false)
        ownershipAdapter.prepareForStart(original: firstOwned, originalPID: 77, useCurrentField: false)
        ownershipAdapter.captureOriginal(operation: 20, requestID: 21)
        let firstToken = ownershipCompletion.originals[0].3.token
        let secondOwned = WorkflowNativeTarget(identity: 51, pid: 77, editable: true, secure: false)
        ownershipAdapter.prepareForStart(original: secondOwned, originalPID: 77, useCurrentField: false)
        ownershipAdapter.captureOriginal(operation: 22, requestID: 23)
        ownershipAdapter.insertText(operation: 22, requestID: 24, targetToken: firstToken, text: "private")
        precondition(ownershipCompletion.inserts.last?.2 == LV_WORKFLOW_ADAPTER_TARGET_CHANGED)
        precondition(ownershipPaste.inserts == 0, "Stale operation token must not paste")
        ownershipAdapter.finalizeOperation(20)
        ownershipAdapter.finalizeOperation(22)
        ownershipAdapter.finalizeOperation(22)
        precondition(ownershipTargeting.released == Set([50, 51]))
        precondition(ownershipTargeting.releaseCalls == 2,
            "Context/token aliases must release each native target only once")

        // Simulate bridge deinit being the final adapter owner. Its scheduled
        // cleanup must retain only native adapter state long enough to cancel
        // capture/remove transient audio, close transport, and release the
        // pending insertion target; afterward that adapter must be releasable.
        let emergencyCapture = CaptureFake()
        emergencyCapture.isRecording = true
        emergencyCapture.hasTransientWAV = true
        let emergencyTransport = TransportFake()
        emergencyTransport.active = true
        let emergencyTargeting = TargetingFake()
        let emergencyPaste = PasteFake()
        var emergencyAdapter: WorkflowNativeAdapter? = WorkflowNativeAdapter(
            capture: emergencyCapture, transport: emergencyTransport,
            paste: emergencyPaste, targeting: emergencyTargeting)
        weak let releasedAdapter = emergencyAdapter
        let emergencyCompletion = CompletionSpy()
        emergencyAdapter!.attach(emergencyCompletion)
        let original = WorkflowNativeTarget(identity: 99, pid: 77, editable: true, secure: false)
        emergencyAdapter!.prepareForStart(original: original, originalPID: original.pid, useCurrentField: false)
        emergencyAdapter!.captureOriginal(operation: 14, requestID: 15)
        precondition(emergencyCompletion.originals.count == 1)
        precondition(emergencyCompletion.originals[0].2 == LV_WORKFLOW_ADAPTER_NONE)
        precondition(emergencyCompletion.originals[0].3.token != 0,
            "Setup must retain a pending insertion target")
        emergencyAdapter!.abandonWithoutBridge()
        emergencyAdapter = nil
        await waitUntil {
            emergencyCapture.cancels > 0 && !emergencyCapture.hasTransientWAV &&
                emergencyTransport.closes > 0 && !emergencyTransport.active &&
                emergencyTargeting.released == Set([99]) && emergencyTargeting.releaseCalls == 1
        }
        await waitUntil { releasedAdapter == nil }
        precondition(releasedAdapter == nil,
            "Emergency cleanup must not retain the adapter after native cleanup finishes")
        print("PASS: native workflow adapter preserves GET/mic parity and target-safe emergency cleanup")
    }
}
