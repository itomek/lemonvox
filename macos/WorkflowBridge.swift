import Foundation

struct WorkflowBridgeEndpointFacts {
    let canonicalBaseURL: String
    let schemeAllowed: Bool
    let hostLoopback: Bool
    let noUserinfo: Bool
    let noQueryFragment: Bool
    let pathEndsV1: Bool
}

struct WorkflowBridgeConfig {
    let endpoint: WorkflowBridgeEndpointFacts
    let preferenceEndpoint: String
    let asrModel: String
    let cleanupModel: String
    let cleanupPrompt: String
    let insertCurrentTarget: Bool
}

struct WorkflowBridgeTarget {
    let token: UInt64
    let editable: Bool
    let secure: Bool
    let frontmost: Bool
    let matchesOriginal: Bool
}

struct WorkflowBridgeTransportRequest {
    let method: LVWorkflowMethod
    let url: String
    let body: Data
    let contentType: String?
    let timeoutMS: UInt32
}

struct WorkflowBridgeFailure {
    let source: LVWorkflowFailureSource
    let adapterError: LVWorkflowAdapterError
    let protocolStatus: LVProtocolStatus
    let httpStatus: Int
    let safeDetail: String
}

struct WorkflowBridgeOutcome {
    let kind: LVWorkflowOutcomeKind
    let fallbackReason: LVWorkflowFallbackReason
}

struct WorkflowBridgeCallbacks {
    var phaseChanged: (UInt64, LVWorkflowPhase) -> Void = { _, _ in }
    var textReady: (UInt64, String) -> Void = { _, _ in }
    var failed: (UInt64, WorkflowBridgeFailure) -> Void = { _, _ in }
    var completed: (UInt64, WorkflowBridgeOutcome) -> Void = { _, _ in }
    var diagnostic: (LVWorkflowDiagnosticEvent, LVWorkflowAdapterError, Int) -> Void = { _, _, _ in }
}

@MainActor protocol WorkflowBridgeNative: AnyObject {
    func captureOriginal(operation: UInt64, requestID: UInt64)
    func captureStart(operation: UInt64, requestID: UInt64)
    func captureStop(operation: UInt64, requestID: UInt64)
    func transportSend(operation: UInt64, requestID: UInt64, request: WorkflowBridgeTransportRequest)
    func inspectTarget(operation: UInt64, requestID: UInt64, originalToken: UInt64)
    func insertText(operation: UInt64, requestID: UInt64, targetToken: UInt64, text: String)
    func captureCancel(operation: UInt64)
    func transportCancel(operation: UInt64)
    func insertionCancel(operation: UInt64)
    func prepareForShutdown()
    func captureShutdown()
    func transportShutdown()
    func insertionShutdown()

    // This must be idempotent and must not retain or invoke WorkflowBridge.
    // It schedules native-owned cancellation, task-map cleanup, and capture /
    // transport / insertion shutdown on the main actor for emergency teardown.
    nonisolated func abandonWithoutBridge()
}

private final class WorkflowBridgeCallbackContext: @unchecked Sendable {
    private let lock = NSLock()
    private weak var bridge: WorkflowBridge?

    func install(_ bridge: WorkflowBridge) {
        lock.lock()
        self.bridge = bridge
        lock.unlock()
    }

    func invalidate() {
        lock.lock()
        bridge = nil
        lock.unlock()
    }

    func bridgeIfAlive() -> WorkflowBridge? {
        lock.lock()
        let bridge = self.bridge
        lock.unlock()
        return bridge
    }
}

private final class WorkflowBridgeLifetime: @unchecked Sendable {
    private let lock = NSLock()
    private let native: any WorkflowBridgeNative
    private let callbackContext: WorkflowBridgeCallbackContext
    private var handle: OpaquePointer?
    private var emergencyTeardownScheduled = false

    init(native: any WorkflowBridgeNative, callbackContext: WorkflowBridgeCallbackContext) {
        self.native = native
        self.callbackContext = callbackContext
    }

    func adopt(_ handle: OpaquePointer) {
        lock.lock()
        self.handle = handle
        lock.unlock()
    }

    func disarm() {
        callbackContext.invalidate()
        lock.lock()
        handle = nil
        lock.unlock()
    }

    // C has accepted a normal shutdown handle for deletion at the end of its
    // current entry. Keep the context alive through the current main-actor
    // turn so that remaining synchronous native shutdown callbacks still run.
    func markCDeleteDeferred() {
        lock.lock()
        guard handle != nil else {
            lock.unlock()
            return
        }
        handle = nil
        lock.unlock()
        let retainedLifetime = self
        Task { @MainActor in
            retainedLifetime.callbackContext.invalidate()
        }
    }

    // This is safe from nonisolated deinit: it first invalidates the weak
    // callback context, then its task retains only this lifecycle box/native.
    func scheduleEmergencyTeardown() {
        lock.lock()
        guard handle != nil && !emergencyTeardownScheduled else {
            lock.unlock()
            return
        }
        emergencyTeardownScheduled = true
        lock.unlock()
        callbackContext.invalidate()
        let retainedLifetime = self
        Task { @MainActor in
            retainedLifetime.runEmergencyTeardown()
        }
    }

    @MainActor private func runEmergencyTeardown() {
        lock.lock()
        let handle = self.handle
        self.handle = nil
        lock.unlock()
        guard let handle else { return }
        native.abandonWithoutBridge()
        lv_workflow_abandon(handle)
    }
}

@MainActor final class WorkflowBridge {
    private let native: WorkflowBridgeNative
    private var callbacks: WorkflowBridgeCallbacks
    private var handle: OpaquePointer?
    private let callbackContext: WorkflowBridgeCallbackContext
    private let lifetime: WorkflowBridgeLifetime
    private var closed = false
    private var callbackDepth = 0
    private var destroyDeferred = false
    private var cDestroyDeferred = false
    private var pendingRequestIDs = Set<UInt64>()

    init(native: WorkflowBridgeNative, callbacks: WorkflowBridgeCallbacks = .init()) {
        self.native = native
        self.callbacks = callbacks
        self.handle = nil
        self.callbackContext = WorkflowBridgeCallbackContext()
        self.lifetime = WorkflowBridgeLifetime(native: native, callbackContext: callbackContext)
        var nativeCallbacks = LVWorkflowNativeCallbacks()
        nativeCallbacks.capture_original = Self.captureOriginalRequest
        nativeCallbacks.capture_start = Self.captureStartRequest
        nativeCallbacks.capture_stop = Self.captureStopRequest
        nativeCallbacks.transport_send = Self.transportSendRequest
        nativeCallbacks.inspect_target = Self.inspectRequest
        nativeCallbacks.insert_text = Self.insertRequest
        nativeCallbacks.capture_cancel = Self.captureCancelRequest
        nativeCallbacks.transport_cancel = Self.transportCancelRequest
        nativeCallbacks.insertion_cancel = Self.insertionCancelRequest
        nativeCallbacks.capture_shutdown = Self.captureShutdownRequest
        nativeCallbacks.transport_shutdown = Self.transportShutdownRequest
        nativeCallbacks.insertion_shutdown = Self.insertionShutdownRequest

        var coreCallbacks = LVWorkflowCoreCallbacks()
        coreCallbacks.phase_changed = Self.phaseChanged
        coreCallbacks.text_ready = Self.textReady
        coreCallbacks.failed = Self.failed
        coreCallbacks.completed = Self.completed
        coreCallbacks.diagnostic = Self.diagnostic
        callbackContext.install(self)
        let created = lv_workflow_create(&nativeCallbacks, &coreCallbacks,
            Unmanaged.passUnretained(callbackContext).toOpaque())
        precondition(created != nil, "Workflow bridge construction requires every native callback")
        handle = created
        lifetime.adopt(created!)
    }

    deinit {
        lifetime.scheduleEmergencyTeardown()
    }

    func start(_ config: WorkflowBridgeConfig) -> UInt64 {
        guard !closed, let handle else { return 0 }
        return withWorkflowConfig(config) { raw in
            var raw = raw
            return lv_workflow_start(handle, &raw)
        }
    }

    func stop(boundary: String) -> Bool {
        guard !closed, let handle else { return false }
        return withSlice(boundary) { lv_workflow_stop(handle, $0) != 0 }
    }

    func cancel() {
        guard !closed, let handle else { return }
        lv_workflow_cancel(handle)
    }

    func shutdown() {
        guard !closed, let handle else { return }
        closed = true
        pendingRequestIDs.removeAll()
        native.prepareForShutdown()
        lv_workflow_shutdown(handle)
        if callbackDepth != 0 { destroyDeferred = true }
    }

    func close() {
        shutdown()
        _ = destroyIfReady()
    }

    @discardableResult func destroyIfReady() -> Bool {
        guard let handle, closed, !cDestroyDeferred else { return false }
        guard lv_workflow_destroy(handle) != 0 else {
            // A closed bridge can only receive this result while C is still
            // unwinding an entry. C now owns the deferred physical delete.
            self.handle = nil
            cDestroyDeferred = true
            lifetime.markCDeleteDeferred()
            destroyDeferred = false
            return false
        }
        self.handle = nil
        lifetime.disarm()
        destroyDeferred = false
        return true
    }

    @discardableResult func completeOriginal(operation: UInt64, requestID: UInt64,
        error: LVWorkflowAdapterError, target: WorkflowBridgeTarget) -> Bool {
        guard let handle, acceptCompletion(requestID) else { return false }
        let accepted = lv_workflow_complete_capture_original(handle, operation, requestID, error, rawTarget(target)) != 0
        if accepted { pendingRequestIDs.remove(requestID) }
        return accepted
    }

    @discardableResult func completeCaptureStart(operation: UInt64, requestID: UInt64,
        error: LVWorkflowAdapterError) -> Bool {
        guard let handle, acceptCompletion(requestID) else { return false }
        let accepted = lv_workflow_complete_capture_start(handle, operation, requestID, error) != 0
        if accepted { pendingRequestIDs.remove(requestID) }
        return accepted
    }

    @discardableResult func completeCaptureStop(operation: UInt64, requestID: UInt64,
        error: LVWorkflowAdapterError, wav: Data) -> Bool {
        guard let handle, acceptCompletion(requestID) else { return false }
        let accepted = withSlice(wav) {
            lv_workflow_complete_capture_stop(handle, operation, requestID, error, $0) != 0
        }
        if accepted { pendingRequestIDs.remove(requestID) }
        return accepted
    }

    @discardableResult func completeTransport(operation: UInt64, requestID: UInt64,
        error: LVWorkflowAdapterError, httpStatus: Int, body: Data) -> Bool {
        guard let handle, acceptCompletion(requestID) else { return false }
        let accepted = withSlice(body) {
            lv_workflow_complete_transport(handle, operation, requestID, error, Int32(httpStatus), $0) != 0
        }
        if accepted { pendingRequestIDs.remove(requestID) }
        return accepted
    }

    @discardableResult func completeInspect(operation: UInt64, requestID: UInt64,
        error: LVWorkflowAdapterError, target: WorkflowBridgeTarget) -> Bool {
        guard let handle, acceptCompletion(requestID) else { return false }
        let accepted = lv_workflow_complete_inspect(handle, operation, requestID, error, rawTarget(target)) != 0
        if accepted { pendingRequestIDs.remove(requestID) }
        return accepted
    }

    @discardableResult func completeInsert(operation: UInt64, requestID: UInt64,
        error: LVWorkflowAdapterError) -> Bool {
        guard let handle, acceptCompletion(requestID) else { return false }
        let accepted = lv_workflow_complete_insert(handle, operation, requestID, error) != 0
        if accepted { pendingRequestIDs.remove(requestID) }
        return accepted
    }

    private func acceptCompletion(_ requestID: UInt64) -> Bool {
        !closed && pendingRequestIDs.contains(requestID)
    }

    private func beginCallback() { callbackDepth += 1 }

    private func endCallback() {
        callbackDepth -= 1
        if callbackDepth == 0, destroyDeferred {
            Task { @MainActor [weak self] in
                _ = self?.destroyIfReady()
            }
        }
    }

    private func requestOriginal(operation: UInt64, requestID: UInt64) {
        guard !closed else { return }
        pendingRequestIDs.insert(requestID)
        native.captureOriginal(operation: operation, requestID: requestID)
    }

    private func requestCaptureStart(operation: UInt64, requestID: UInt64) {
        guard !closed else { return }
        pendingRequestIDs.insert(requestID)
        native.captureStart(operation: operation, requestID: requestID)
    }

    private func requestCaptureStop(operation: UInt64, requestID: UInt64) {
        guard !closed else { return }
        pendingRequestIDs.insert(requestID)
        native.captureStop(operation: operation, requestID: requestID)
    }

    private func requestTransport(operation: UInt64, requestID: UInt64,
        request: UnsafePointer<LVWorkflowTransportRequest>?) {
        guard !closed, let request else { return }
        let raw = request.pointee
        pendingRequestIDs.insert(requestID)
        native.transportSend(operation: operation, requestID: requestID,
            request: .init(method: raw.method, url: string(raw.url), body: data(raw.body),
                contentType: raw.content_type.size == 0 ? nil : string(raw.content_type), timeoutMS: raw.timeout_ms))
    }

    private func requestInspect(operation: UInt64, requestID: UInt64, originalToken: UInt64) {
        guard !closed else { return }
        pendingRequestIDs.insert(requestID)
        native.inspectTarget(operation: operation, requestID: requestID, originalToken: originalToken)
    }

    private func requestInsert(operation: UInt64, requestID: UInt64, targetToken: UInt64,
        text: LVProtocolSlice) {
        guard !closed else { return }
        pendingRequestIDs.insert(requestID)
        native.insertText(operation: operation, requestID: requestID, targetToken: targetToken, text: string(text))
    }

    private func notifyPhase(operation: UInt64, phase: LVWorkflowPhase) {
        guard !closed else { return }
        callbacks.phaseChanged(operation, phase)
    }

    private func notifyText(operation: UInt64, text: LVProtocolSlice) {
        guard !closed else { return }
        callbacks.textReady(operation, string(text))
    }

    private func notifyFailure(operation: UInt64, failure: LVWorkflowFailure) {
        guard !closed else { return }
        callbacks.failed(operation, .init(source: failure.source, adapterError: failure.adapter_error,
            protocolStatus: failure.protocol_status, httpStatus: Int(failure.http_status),
            safeDetail: string(failure.safe_detail)))
    }

    private func notifyCompleted(operation: UInt64, outcome: LVWorkflowOutcome) {
        guard !closed else { return }
        callbacks.completed(operation, .init(kind: outcome.kind, fallbackReason: outcome.fallback_reason))
    }

    private func notifyDiagnostic(event: LVWorkflowDiagnosticEvent, error: LVWorkflowAdapterError,
        httpStatus: Int32) {
        guard !closed else { return }
        callbacks.diagnostic(event, error, Int(httpStatus))
    }

    private static func withBridge(_ context: UnsafeMutableRawPointer?, _ body: (WorkflowBridge) -> Void) {
        guard let context else { return }
        let callbackContext = Unmanaged<WorkflowBridgeCallbackContext>.fromOpaque(context).takeUnretainedValue()
        MainActor.assumeIsolated {
            guard let bridge = callbackContext.bridgeIfAlive() else { return }
            bridge.beginCallback()
            defer { bridge.endCallback() }
            body(bridge)
        }
    }

    private static let captureOriginalRequest: @convention(c) (UnsafeMutableRawPointer?, OpaquePointer?, UInt64, UInt64) -> Void = {
        context, _, operation, requestID in
        withBridge(context) { $0.requestOriginal(operation: operation, requestID: requestID) }
    }

    private static let captureStartRequest: @convention(c) (UnsafeMutableRawPointer?, OpaquePointer?, UInt64, UInt64) -> Void = {
        context, _, operation, requestID in
        withBridge(context) { $0.requestCaptureStart(operation: operation, requestID: requestID) }
    }

    private static let captureStopRequest: @convention(c) (UnsafeMutableRawPointer?, OpaquePointer?, UInt64, UInt64) -> Void = {
        context, _, operation, requestID in
        withBridge(context) { $0.requestCaptureStop(operation: operation, requestID: requestID) }
    }

    private static let transportSendRequest: @convention(c) (UnsafeMutableRawPointer?, OpaquePointer?, UInt64, UInt64, UnsafePointer<LVWorkflowTransportRequest>?) -> Void = {
        context, _, operation, requestID, request in
        withBridge(context) { $0.requestTransport(operation: operation, requestID: requestID, request: request) }
    }

    private static let inspectRequest: @convention(c) (UnsafeMutableRawPointer?, OpaquePointer?, UInt64, UInt64, UInt64) -> Void = {
        context, _, operation, requestID, originalToken in
        withBridge(context) { $0.requestInspect(operation: operation, requestID: requestID, originalToken: originalToken) }
    }

    private static let insertRequest: @convention(c) (UnsafeMutableRawPointer?, OpaquePointer?, UInt64, UInt64, UInt64, LVProtocolSlice) -> Void = {
        context, _, operation, requestID, targetToken, text in
        withBridge(context) { $0.requestInsert(operation: operation, requestID: requestID, targetToken: targetToken, text: text) }
    }

    private static let captureCancelRequest: @convention(c) (UnsafeMutableRawPointer?, OpaquePointer?, UInt64) -> Void = {
        context, _, operation in withBridge(context) { $0.native.captureCancel(operation: operation) }
    }

    private static let transportCancelRequest: @convention(c) (UnsafeMutableRawPointer?, OpaquePointer?, UInt64) -> Void = {
        context, _, operation in withBridge(context) { $0.native.transportCancel(operation: operation) }
    }

    private static let insertionCancelRequest: @convention(c) (UnsafeMutableRawPointer?, OpaquePointer?, UInt64) -> Void = {
        context, _, operation in withBridge(context) { $0.native.insertionCancel(operation: operation) }
    }

    private static let captureShutdownRequest: @convention(c) (UnsafeMutableRawPointer?, OpaquePointer?) -> Void = {
        context, _ in withBridge(context) { $0.native.captureShutdown() }
    }

    private static let transportShutdownRequest: @convention(c) (UnsafeMutableRawPointer?, OpaquePointer?) -> Void = {
        context, _ in withBridge(context) { $0.native.transportShutdown() }
    }

    private static let insertionShutdownRequest: @convention(c) (UnsafeMutableRawPointer?, OpaquePointer?) -> Void = {
        context, _ in withBridge(context) { $0.native.insertionShutdown() }
    }

    private static let phaseChanged: @convention(c) (UnsafeMutableRawPointer?, OpaquePointer?, UInt64, LVWorkflowPhase) -> Void = {
        context, _, operation, phase in withBridge(context) { $0.notifyPhase(operation: operation, phase: phase) }
    }

    private static let textReady: @convention(c) (UnsafeMutableRawPointer?, OpaquePointer?, UInt64, LVProtocolSlice) -> Void = {
        context, _, operation, text in withBridge(context) { $0.notifyText(operation: operation, text: text) }
    }

    private static let failed: @convention(c) (UnsafeMutableRawPointer?, OpaquePointer?, UInt64, LVWorkflowFailure) -> Void = {
        context, _, operation, failure in withBridge(context) { $0.notifyFailure(operation: operation, failure: failure) }
    }

    private static let completed: @convention(c) (UnsafeMutableRawPointer?, OpaquePointer?, UInt64, LVWorkflowOutcome) -> Void = {
        context, _, operation, outcome in withBridge(context) { $0.notifyCompleted(operation: operation, outcome: outcome) }
    }

    private static let diagnostic: @convention(c) (UnsafeMutableRawPointer?, OpaquePointer?, LVWorkflowDiagnosticEvent, LVWorkflowAdapterError, Int32) -> Void = {
        context, _, event, error, httpStatus in
        withBridge(context) { $0.notifyDiagnostic(event: event, error: error, httpStatus: httpStatus) }
    }
}

@MainActor private func rawTarget(_ target: WorkflowBridgeTarget) -> LVWorkflowTarget {
    var raw = LVWorkflowTarget()
    raw.token = target.token
    raw.editable = target.editable ? 1 : 0
    raw.secure = target.secure ? 1 : 0
    raw.frontmost = target.frontmost ? 1 : 0
    raw.matches_original = target.matchesOriginal ? 1 : 0
    return raw
}

@MainActor private func data(_ slice: LVProtocolSlice) -> Data {
    guard let pointer = slice.data, slice.size != 0 else { return Data() }
    return Data(bytes: pointer, count: slice.size)
}

@MainActor private func string(_ slice: LVProtocolSlice) -> String {
    String(decoding: data(slice), as: UTF8.self)
}

@MainActor private func withSlice<Result>(_ value: String, _ body: (LVProtocolSlice) -> Result) -> Result {
    withSlice(Data(value.utf8), body)
}

@MainActor private func withSlice<Result>(_ value: Data, _ body: (LVProtocolSlice) -> Result) -> Result {
    value.withUnsafeBytes { raw in
        var slice = LVProtocolSlice()
        slice.data = raw.bindMemory(to: UInt8.self).baseAddress
        slice.size = raw.count
        return body(slice)
    }
}

@MainActor private func withWorkflowConfig<Result>(_ config: WorkflowBridgeConfig,
    _ body: (LVWorkflowConfig) -> Result) -> Result {
    withSlice(config.endpoint.canonicalBaseURL) { canonical in
        withSlice(config.preferenceEndpoint) { preferenceEndpoint in
            withSlice(config.asrModel) { asrModel in
                withSlice(config.cleanupModel) { cleanupModel in
                    withSlice(config.cleanupPrompt) { cleanupPrompt in
                        var endpoint = LVEndpointFacts()
                        endpoint.canonical_base_url = canonical
                        endpoint.scheme_allowed = config.endpoint.schemeAllowed ? 1 : 0
                        endpoint.host_loopback = config.endpoint.hostLoopback ? 1 : 0
                        endpoint.no_userinfo = config.endpoint.noUserinfo ? 1 : 0
                        endpoint.no_query_fragment = config.endpoint.noQueryFragment ? 1 : 0
                        endpoint.path_ends_v1 = config.endpoint.pathEndsV1 ? 1 : 0
                        var raw = LVWorkflowConfig()
                        raw.endpoint = endpoint
                        raw.preference_endpoint = preferenceEndpoint
                        raw.asr_model = asrModel
                        raw.cleanup_model = cleanupModel
                        raw.cleanup_prompt = cleanupPrompt
                        raw.insert_current_target = config.insertCurrentTarget ? 1 : 0
                        return body(raw)
                    }
                }
            }
        }
    }
}
