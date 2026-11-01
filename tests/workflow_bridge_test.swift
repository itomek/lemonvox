import Foundation

@MainActor private final class NativeFake: WorkflowBridgeNative {
    weak var bridge: WorkflowBridge?
    var originalRequests: [(UInt64, UInt64)] = []
    var startRequests: [(UInt64, UInt64)] = []
    var stopRequests: [(UInt64, UInt64)] = []
    var transportRequests: [(UInt64, UInt64, WorkflowBridgeTransportRequest)] = []
    var inspectRequests: [(UInt64, UInt64, UInt64)] = []
    var insertRequests: [(UInt64, UInt64, UInt64, String)] = []
    var cancelEvents: [String] = []
    var shutdowns: [String] = []
    var preparedForShutdown = false
    var completeOriginalSynchronously = false
    var onCaptureOriginal: ((UInt64, UInt64) -> Void)?
    var onPrepareForShutdown: (() -> Void)?
    var delayedCompletionAccepted: Bool?
    var activeOperations = Set<UInt64>()
    var captureActive = false
    var transientWAV = Data()
    var delayedTask: Task<Void, Never>?
    var delayedTaskCanceled = false
    var emergencyCleanupRuns = 0

    func captureOriginal(operation: UInt64, requestID: UInt64) {
        activeOperations.insert(operation)
        originalRequests.append((operation, requestID))
        if let onCaptureOriginal {
            onCaptureOriginal(operation, requestID)
            return
        }
        if completeOriginalSynchronously {
            precondition(bridge?.completeOriginal(operation: operation, requestID: requestID,
                error: LV_WORKFLOW_ADAPTER_NONE,
                target: WorkflowBridgeTarget(token: 71, editable: true, secure: false,
                    frontmost: true, matchesOriginal: true)) == true)
        }
    }

    func captureStart(operation: UInt64, requestID: UInt64) {
        activeOperations.insert(operation)
        captureActive = true
        startRequests.append((operation, requestID))
    }

    func captureStop(operation: UInt64, requestID: UInt64) {
        activeOperations.insert(operation)
        stopRequests.append((operation, requestID))
    }

    func transportSend(operation: UInt64, requestID: UInt64, request: WorkflowBridgeTransportRequest) {
        activeOperations.insert(operation)
        transportRequests.append((operation, requestID, request))
    }

    func inspectTarget(operation: UInt64, requestID: UInt64, originalToken: UInt64) {
        activeOperations.insert(operation)
        inspectRequests.append((operation, requestID, originalToken))
    }

    func insertText(operation: UInt64, requestID: UInt64, targetToken: UInt64, text: String) {
        activeOperations.insert(operation)
        insertRequests.append((operation, requestID, targetToken, text))
    }

    func captureCancel(operation: UInt64) { cancelEvents.append("capture:\(operation)") }
    func transportCancel(operation: UInt64) { cancelEvents.append("transport:\(operation)") }
    func insertionCancel(operation: UInt64) { cancelEvents.append("insertion:\(operation)") }
    func prepareForShutdown() {
        preparedForShutdown = true
        onPrepareForShutdown?()
    }
    func captureShutdown() { shutdowns.append("capture") }
    func transportShutdown() { shutdowns.append("transport") }
    func insertionShutdown() { shutdowns.append("insertion") }

    nonisolated func abandonWithoutBridge() {
        let retainedNative = self
        Task { @MainActor in
            retainedNative.runEmergencyCleanup()
        }
    }

    private func runEmergencyCleanup() {
        guard emergencyCleanupRuns == 0 else { return }
        emergencyCleanupRuns = 1
        prepareForShutdown()
        for operation in activeOperations {
            captureCancel(operation: operation)
            transportCancel(operation: operation)
            insertionCancel(operation: operation)
        }
        delayedTask?.cancel()
        captureActive = false
        transientWAV.removeAll()
        captureShutdown()
        transportShutdown()
        insertionShutdown()
    }

    func armDelayedTaskAndCapture() {
        captureActive = true
        transientWAV = Data("RIFF transient WAV".utf8)
        let retainedNative = self
        delayedTask = Task { @MainActor in
            do {
                try await Task.sleep(nanoseconds: 5_000_000_000)
            } catch {
                retainedNative.delayedTaskCanceled = true
                return
            }
            retainedNative.completeDelayedOriginal()
        }
    }

    func completeDelayedOriginal() {
        guard let request = originalRequests.first, let bridge else {
            delayedCompletionAccepted = false
            return
        }
        delayedCompletionAccepted = bridge.completeOriginal(operation: request.0, requestID: request.1,
            error: LV_WORKFLOW_ADAPTER_NONE,
            target: WorkflowBridgeTarget(token: 71, editable: true, secure: false,
                frontmost: true, matchesOriginal: true))
    }
}

@main @MainActor struct WorkflowBridgeTests {
    static func main() async {
        synchronousRequestsAndStaleCompletions()
        ownedUnicodePayloads()
        callbackCanCancelAndRestart()
        await shutdownDuringNativeCallbackDefersDestruction()
        await droppingLastReferenceUsesNoCallbackTeardown()
        print("PASS: Swift workflow bridge rejects stale completions and survives synchronous reentry")
    }

    private static func validConfig() -> WorkflowBridgeConfig {
        WorkflowBridgeConfig(
            endpoint: WorkflowBridgeEndpointFacts(canonicalBaseURL: "http://127.0.0.1:13305/v1",
                schemeAllowed: true, hostLoopback: true, noUserinfo: true,
                noQueryFragment: true, pathEndsV1: true),
            preferenceEndpoint: "http://127.0.0.1:13305/v1", asrModel: "asr",
            cleanupModel: "cleanup", cleanupPrompt: "prompt", insertCurrentTarget: false)
    }

    private static func target(token: UInt64 = 71) -> WorkflowBridgeTarget {
        WorkflowBridgeTarget(token: token, editable: true, secure: false,
            frontmost: true, matchesOriginal: true)
    }

    private static var inventory: Data {
        Data(#"{"data":[{"id":"asr","downloaded":true,"recipe":"whispercpp","labels":["transcription"]},{"id":"cleanup","downloaded":true,"recipe":"llamacpp","labels":["chat"]}]}"#.utf8)
    }

    private static func synchronousRequestsAndStaleCompletions() {
        let native = NativeFake()
        native.completeOriginalSynchronously = true
        let bridge = WorkflowBridge(native: native)
        native.bridge = bridge
        let operation = bridge.start(validConfig())

        precondition(operation == 1)
        precondition(native.originalRequests.count == 1)
        precondition(native.transportRequests.count == 1,
            "synchronous original-target completion must advance before start returns")
        let models = native.transportRequests[0]
        precondition(models.2.method == LV_WORKFLOW_METHOD_GET)
        precondition(models.2.url == "http://127.0.0.1:13305/v1/models")
        precondition(!bridge.completeTransport(operation: operation, requestID: models.1 + 1,
            error: LV_WORKFLOW_ADAPTER_NONE, httpStatus: 200, body: Data()))
        precondition(!bridge.completeTransport(operation: operation + 1, requestID: models.1,
            error: LV_WORKFLOW_ADAPTER_NONE, httpStatus: 200, body: inventory))
        precondition(bridge.completeTransport(operation: operation, requestID: models.1,
            error: LV_WORKFLOW_ADAPTER_NONE, httpStatus: 200, body: inventory))
        precondition(!bridge.completeTransport(operation: operation, requestID: models.1,
            error: LV_WORKFLOW_ADAPTER_NONE, httpStatus: 200, body: inventory))
        precondition(native.startRequests.count == 1)

        var acceptedDuringPrepare = true
        native.onPrepareForShutdown = {
            acceptedDuringPrepare = bridge.completeCaptureStart(operation: operation,
                requestID: native.startRequests[0].1, error: LV_WORKFLOW_ADAPTER_NONE)
        }
        bridge.shutdown()
        precondition(native.preparedForShutdown)
        precondition(!acceptedDuringPrepare,
            "Swift must close and clear tokens before native shutdown preparation")
        precondition(native.shutdowns == ["capture", "transport", "insertion"])
        precondition(!bridge.completeTransport(operation: operation, requestID: models.1,
            error: LV_WORKFLOW_ADAPTER_NONE, httpStatus: 200, body: Data()))
        precondition(bridge.destroyIfReady())
    }

    private static func ownedUnicodePayloads() {
        var textEvents: [String] = []
        var outcomes: [WorkflowBridgeOutcome] = []
        let native = NativeFake()
        native.completeOriginalSynchronously = true
        let bridge = WorkflowBridge(native: native, callbacks: .init(
            textReady: { _, text in textEvents.append(text) },
            completed: { _, outcome in outcomes.append(outcome) }))
        native.bridge = bridge

        let operation = bridge.start(validConfig())
        let firstModels = native.transportRequests[0]
        precondition(bridge.completeTransport(operation: operation, requestID: firstModels.1,
            error: LV_WORKFLOW_ADAPTER_NONE, httpStatus: 200, body: inventory))
        let captureStart = native.startRequests[0]
        precondition(bridge.completeCaptureStart(operation: operation, requestID: captureStart.1,
            error: LV_WORKFLOW_ADAPTER_NONE))
        precondition(bridge.stop(boundary: "LemonVox-fixture"))
        let captureStop = native.stopRequests[0]
        precondition(bridge.completeCaptureStop(operation: operation, requestID: captureStop.1,
            error: LV_WORKFLOW_ADAPTER_NONE, wav: Data("RIFF".utf8)))
        let secondModels = native.transportRequests[1]
        precondition(bridge.completeTransport(operation: operation, requestID: secondModels.1,
            error: LV_WORKFLOW_ADAPTER_NONE, httpStatus: 200, body: inventory))
        let transcription = native.transportRequests[2]
        precondition(bridge.completeTransport(operation: operation, requestID: transcription.1,
            error: LV_WORKFLOW_ADAPTER_NONE, httpStatus: 200,
            body: Data(#"{"text":"Zażółć 🦊"}"#.utf8)))
        let cleanup = native.transportRequests[3]
        precondition(bridge.completeTransport(operation: operation, requestID: cleanup.1,
            error: LV_WORKFLOW_ADAPTER_NONE, httpStatus: 200,
            body: Data(#"{"choices":[{"message":{"content":"Zażółć 🦊"},"finish_reason":"stop"}]}"#.utf8)))
        let inspect = native.inspectRequests[0]
        precondition(bridge.completeInspect(operation: operation, requestID: inspect.1,
            error: LV_WORKFLOW_ADAPTER_NONE, target: target()))
        let insert = native.insertRequests[0]
        precondition(insert.3 == "Zażółć 🦊")
        precondition(bridge.completeInsert(operation: operation, requestID: insert.1,
            error: LV_WORKFLOW_ADAPTER_UNAVAILABLE))
        precondition(textEvents == ["Zażółć 🦊"], "text must be copied before the C++ callback returns")
        precondition(outcomes.count == 1 && outcomes[0].kind == LV_WORKFLOW_OUTCOME_COPY_FALLBACK)
        bridge.shutdown()
        precondition(bridge.destroyIfReady())

        var failures: [WorkflowBridgeFailure] = []
        let errorNative = NativeFake()
        errorNative.completeOriginalSynchronously = true
        let errorBridge = WorkflowBridge(native: errorNative,
            callbacks: .init(failed: { _, failure in failures.append(failure) }))
        errorNative.bridge = errorBridge
        let errorOperation = errorBridge.start(validConfig())
        let errorModels = errorNative.transportRequests[0]
        precondition(errorBridge.completeTransport(operation: errorOperation, requestID: errorModels.1,
            error: LV_WORKFLOW_ADAPTER_NONE, httpStatus: 401,
            body: Data(#"{"error":{"message":"żółw 🦊"}}"#.utf8)))
        precondition(failures.count == 1)
        precondition(failures[0].safeDetail == "żółw 🦊",
            "failure detail must be copied before the C++ callback returns")
        errorBridge.shutdown()
        precondition(errorBridge.destroyIfReady())
    }

    private static func callbackCanCancelAndRestart() {
        let native = NativeFake()
        var bridge: WorkflowBridge!
        var didCancel = false
        var restartedOperation: UInt64 = 0
        var callbacks = WorkflowBridgeCallbacks()
        callbacks.phaseChanged = { _, phase in
            guard phase == LV_WORKFLOW_PHASE_PREPARING, !didCancel else { return }
            didCancel = true
            bridge.cancel()
            restartedOperation = bridge.start(validConfig())
        }
        bridge = WorkflowBridge(native: native, callbacks: callbacks)
        native.bridge = bridge

        let originalOperation = bridge.start(validConfig())
        precondition(originalOperation == 1)
        precondition(restartedOperation == 2,
            "a restart after synchronous cancellation must have a distinct operation ID")
        precondition(native.cancelEvents == ["capture:1", "transport:1", "insertion:1"])
        precondition(native.originalRequests.count == 1 && native.originalRequests[0].0 == restartedOperation,
            "the canceled operation must not dispatch a request after its phase callback")
        bridge.shutdown()
        precondition(bridge.destroyIfReady())
    }

    private static func shutdownDuringNativeCallbackDefersDestruction() async {
        let native = NativeFake()
        var bridge: WorkflowBridge!
        var destroyDuringCallback = true
        native.onCaptureOriginal = { _, _ in
            bridge.shutdown()
            destroyDuringCallback = bridge.destroyIfReady()
        }
        bridge = WorkflowBridge(native: native)
        native.bridge = bridge

        let operation = bridge.start(validConfig())
        precondition(operation == 1)
        precondition(!destroyDuringCallback,
            "destroy must defer while a native request callback is on the stack")
        precondition(native.preparedForShutdown)
        precondition(native.cancelEvents == ["capture:1", "transport:1", "insertion:1"])
        precondition(native.shutdowns == ["capture", "transport", "insertion"])
        precondition(native.transportRequests.isEmpty,
            "shutdown from capture callback must prevent later request dispatch")
        precondition(bridge.start(validConfig()) == 0)
        precondition(!bridge.destroyIfReady(),
            "the C bridge must consume its deferred destroy after callback and start entries unwind")
        await Task.yield()
        precondition(!bridge.destroyIfReady(),
            "the Swift handle must clear only after C has finished its deferred delete")
    }

    private static func droppingLastReferenceUsesNoCallbackTeardown() async {
        let native = NativeFake()
        weak var weakBridge: WorkflowBridge?
        do {
            let bridge = WorkflowBridge(native: native)
            native.bridge = bridge
            weakBridge = bridge
            precondition(bridge.start(validConfig()) == 1)
            precondition(native.originalRequests.count == 1, "fixture requires a delayed native request")
            native.armDelayedTaskAndCapture()
        }
        precondition(weakBridge == nil, "the C handle must not retain Swift context")
        for _ in 0..<32 {
            if native.emergencyCleanupRuns == 1 && native.delayedTaskCanceled { break }
            await Task.yield()
        }
        precondition(native.emergencyCleanupRuns == 1 && native.delayedTask?.isCancelled == true &&
            native.delayedTaskCanceled,
            "emergency teardown must cancel native-owned delayed work")
        precondition(!native.captureActive && native.transientWAV.isEmpty,
            "emergency teardown must clear native capture and transient audio state")
        precondition(native.preparedForShutdown)
        precondition(native.cancelEvents == ["capture:1", "transport:1", "insertion:1"])
        precondition(native.shutdowns == ["capture", "transport", "insertion"])
        native.completeDelayedOriginal()
        precondition(native.delayedCompletionAccepted == false,
            "a delayed native task must not call through a dropped bridge context")
    }
}
