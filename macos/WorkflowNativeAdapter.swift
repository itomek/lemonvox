import AppKit
import ApplicationServices
import Foundation

@MainActor protocol WorkflowNativeCompleting: AnyObject {
    @discardableResult func completeOriginal(operation: UInt64, requestID: UInt64,
        error: LVWorkflowAdapterError, target: WorkflowBridgeTarget) -> Bool
    @discardableResult func completeCaptureStart(operation: UInt64, requestID: UInt64,
        error: LVWorkflowAdapterError) -> Bool
    @discardableResult func completeCaptureStop(operation: UInt64, requestID: UInt64,
        error: LVWorkflowAdapterError, wav: Data) -> Bool
    @discardableResult func completeTransport(operation: UInt64, requestID: UInt64,
        error: LVWorkflowAdapterError, httpStatus: Int, body: Data) -> Bool
    @discardableResult func completeInspect(operation: UInt64, requestID: UInt64,
        error: LVWorkflowAdapterError, target: WorkflowBridgeTarget) -> Bool
    @discardableResult func completeInsert(operation: UInt64, requestID: UInt64,
        error: LVWorkflowAdapterError) -> Bool
}

extension WorkflowBridge: WorkflowNativeCompleting {}

@MainActor protocol WorkflowNativeCapture: AnyObject {
    var isRecording: Bool { get }
    var level: Double { get }
    func start() async throws
    func finish() throws -> Data
    func cancel()
}

extension Recorder: WorkflowNativeCapture {}

@MainActor protocol WorkflowNativeTransport: AnyObject {
    func send(_ request: URLRequest) async throws -> (Data, Int)
    func close()
}

extension LocalHTTP: WorkflowNativeTransport {}

@MainActor protocol WorkflowNativePaste: AnyObject {
    var active: Bool { get }
    func insert(_ text: String, validate: () -> Bool, dispatch: () -> Bool) async -> PasteResult
}

extension PasteTransaction: WorkflowNativePaste {}

struct WorkflowNativeTarget {
    let identity: UInt64
    let pid: pid_t
    let editable: Bool
    let secure: Bool
}

@MainActor protocol WorkflowNativeTargeting: AnyObject {
    var accessibilityTrusted: Bool { get }
    func focusedTarget() -> WorkflowNativeTarget?
    func targetsMatch(_ left: WorkflowNativeTarget, _ right: WorkflowNativeTarget) -> Bool
    func frontmostPID() -> pid_t?
    func postPaste(to pid: pid_t) -> Bool
    func release(_ target: WorkflowNativeTarget)
}

struct WorkflowNativeTargetMetadata {
    let editable: Bool
    let secure: Bool
}

@MainActor final class SystemWorkflowTargeting: WorkflowNativeTargeting {
    private let trusted: () -> Bool
    private let focused: () -> AXUIElement?
    private let metadata: (AXUIElement) -> WorkflowNativeTargetMetadata
    private let frontmost: () -> pid_t?
    private let dispatch: (pid_t) -> Bool
    private var nextIdentity: UInt64 = 1
    private var elements: [UInt64: AXUIElement] = [:]

    init(trusted: @escaping () -> Bool, focused: @escaping () -> AXUIElement?,
        metadata: @escaping (AXUIElement) -> WorkflowNativeTargetMetadata,
        frontmost: @escaping () -> pid_t?, dispatch: @escaping (pid_t) -> Bool) {
        self.trusted = trusted
        self.focused = focused
        self.metadata = metadata
        self.frontmost = frontmost
        self.dispatch = dispatch
    }

    var accessibilityTrusted: Bool { trusted() }

    func focusedTarget() -> WorkflowNativeTarget? {
        guard let element = focused() else { return nil }
        var pid: pid_t = 0
        guard AXUIElementGetPid(element, &pid) == .success else { return nil }
        let details = metadata(element)
        let identity = nextIdentity
        nextIdentity &+= 1
        elements[identity] = element
        return WorkflowNativeTarget(identity: identity, pid: pid, editable: details.editable, secure: details.secure)
    }

    func targetsMatch(_ left: WorkflowNativeTarget, _ right: WorkflowNativeTarget) -> Bool {
        guard let leftElement = elements[left.identity], let rightElement = elements[right.identity] else { return false }
        return CFEqual(leftElement, rightElement)
    }

    func frontmostPID() -> pid_t? { frontmost() }
    func postPaste(to pid: pid_t) -> Bool { dispatch(pid) }
    func release(_ target: WorkflowNativeTarget) { elements.removeValue(forKey: target.identity) }
}

@MainActor final class WorkflowNativeAdapter: WorkflowBridgeNative {
    private struct StartContext {
        let original: WorkflowNativeTarget?
        let originalPID: pid_t?
        let useCurrentField: Bool
    }

    private struct TargetRecord {
        let operation: UInt64
        let target: WorkflowNativeTarget
    }

    private let capture: any WorkflowNativeCapture
    private let transport: any WorkflowNativeTransport
    private let paste: any WorkflowNativePaste
    private let targeting: any WorkflowNativeTargeting
    private weak var completion: (any WorkflowNativeCompleting)?
    private var preparedStart: StartContext?
    private var contexts: [UInt64: StartContext] = [:]
    private var targets: [UInt64: TargetRecord] = [:]
    private var nextTargetToken: UInt64 = 1
    private var captureTasks: [UInt64: Task<Void, Never>] = [:]
    private var transportTasks: [UInt64: Task<Void, Never>] = [:]
    private var insertionTasks: [UInt64: Task<Void, Never>] = [:]
    private var shuttingDown = false
    private var captureShutdownComplete = false
    private var transportShutdownComplete = false
    private var insertionShutdownComplete = false
    var onCaptureWillStart: () -> Void = {}

    init(capture: any WorkflowNativeCapture, transport: any WorkflowNativeTransport,
        paste: any WorkflowNativePaste, targeting: any WorkflowNativeTargeting) {
        self.capture = capture
        self.transport = transport
        self.paste = paste
        self.targeting = targeting
    }

    func attach(_ completion: any WorkflowNativeCompleting) {
        guard !shuttingDown else { return }
        self.completion = completion
    }

    func prepareForStart(original: WorkflowNativeTarget?, originalPID: pid_t?, useCurrentField: Bool) {
        guard !shuttingDown else {
            if let original { targeting.release(original) }
            return
        }
        discardPreparedStart()
        preparedStart = StartContext(original: original, originalPID: originalPID, useCurrentField: useCurrentField)
    }

    func discardPreparedStart() {
        if let original = preparedStart?.original { targeting.release(original) }
        preparedStart = nil
    }

    var isRecording: Bool { capture.isRecording }
    var level: Double { capture.level }
    var pasteActive: Bool { paste.active }

    func captureOriginal(operation: UInt64, requestID: UInt64) {
        guard !shuttingDown else { return }
        releaseOperation(operation)
        let context = preparedStart ?? StartContext(original: nil, originalPID: nil, useCurrentField: false)
        preparedStart = nil
        contexts[operation] = context
        guard targeting.accessibilityTrusted else {
            completeOriginal(operation: operation, requestID: requestID,
                error: LV_WORKFLOW_ADAPTER_INSERTION_UNAVAILABLE, target: emptyTarget)
            return
        }
        guard let original = context.original else {
            completeOriginal(operation: operation, requestID: requestID,
                error: LV_WORKFLOW_ADAPTER_UNAVAILABLE, target: emptyTarget)
            return
        }
        if original.secure {
            // Secure-field rejection is a fact about a successfully captured
            // target. WorkflowCore rejects this before any model/capture work.
            completeOriginal(operation: operation, requestID: requestID,
                error: LV_WORKFLOW_ADAPTER_NONE,
                target: bridgeTarget(original, token: 0, matchesOriginal: true))
            return
        }
        guard original.editable else {
            completeOriginal(operation: operation, requestID: requestID,
                error: LV_WORKFLOW_ADAPTER_UNAVAILABLE,
                target: bridgeTarget(original, token: 0, matchesOriginal: true))
            return
        }
        let token = allocate(original, operation: operation)
        completeOriginal(operation: operation, requestID: requestID, error: LV_WORKFLOW_ADAPTER_NONE,
            target: bridgeTarget(original, token: token, matchesOriginal: true))
    }

    func captureStart(operation: UInt64, requestID: UInt64) {
        guard !shuttingDown else { return }
        onCaptureWillStart()
        let task = Task { @MainActor [weak self] in
            guard let self else { return }
            let adapterError: LVWorkflowAdapterError
            do {
                try await capture.start()
                adapterError = Task.isCancelled ? LV_WORKFLOW_ADAPTER_CANCELED : LV_WORKFLOW_ADAPTER_NONE
            } catch is CancellationError {
                adapterError = LV_WORKFLOW_ADAPTER_CANCELED
            } catch DictationError.microphone {
                adapterError = LV_WORKFLOW_ADAPTER_UNAVAILABLE
            } catch {
                adapterError = LV_WORKFLOW_ADAPTER_INVALID_INPUT
            }
            self.captureTasks.removeValue(forKey: operation)
            guard !self.shuttingDown else { return }
            _ = self.completion?.completeCaptureStart(operation: operation, requestID: requestID, error: adapterError)
        }
        captureTasks[operation] = task
    }

    func captureStop(operation: UInt64, requestID: UInt64) {
        guard !shuttingDown else { return }
        let task = Task { @MainActor [weak self] in
            guard let self else { return }
            let adapterError: LVWorkflowAdapterError
            let wav: Data
            do {
                wav = try capture.finish()
                adapterError = Task.isCancelled ? LV_WORKFLOW_ADAPTER_CANCELED : LV_WORKFLOW_ADAPTER_NONE
            } catch is CancellationError {
                wav = Data()
                adapterError = LV_WORKFLOW_ADAPTER_CANCELED
            } catch {
                wav = Data()
                adapterError = LV_WORKFLOW_ADAPTER_INVALID_INPUT
            }
            self.captureTasks.removeValue(forKey: operation)
            guard !self.shuttingDown else { return }
            _ = self.completion?.completeCaptureStop(operation: operation, requestID: requestID, error: adapterError, wav: wav)
        }
        captureTasks[operation] = task
    }

    func transportSend(operation: UInt64, requestID: UInt64, request: WorkflowBridgeTransportRequest) {
        guard !shuttingDown else { return }
        guard let url = URL(string: request.url) else {
            _ = completion?.completeTransport(operation: operation, requestID: requestID,
                error: LV_WORKFLOW_ADAPTER_RESPONSE, httpStatus: 0, body: Data())
            return
        }
        var nativeRequest = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData)
        nativeRequest.httpMethod = request.method == LV_WORKFLOW_METHOD_POST ? "POST" : "GET"
        if !request.body.isEmpty { nativeRequest.httpBody = request.body }
        if let contentType = request.contentType { nativeRequest.setValue(contentType, forHTTPHeaderField: "Content-Type") }
        if request.timeoutMS != 0 { nativeRequest.timeoutInterval = TimeInterval(request.timeoutMS) / 1000 }

        let task = Task { @MainActor [weak self] in
            guard let self else { return }
            let adapterError: LVWorkflowAdapterError
            let status: Int
            let body: Data
            do {
                let response = try await transport.send(nativeRequest)
                body = response.0
                status = response.1
                adapterError = Task.isCancelled ? LV_WORKFLOW_ADAPTER_CANCELED : LV_WORKFLOW_ADAPTER_NONE
            } catch is CancellationError {
                body = Data()
                status = 0
                adapterError = LV_WORKFLOW_ADAPTER_CANCELED
            } catch DictationError.serverUnavailable {
                body = Data()
                status = 0
                adapterError = LV_WORKFLOW_ADAPTER_NETWORK
            } catch DictationError.response {
                body = Data()
                status = 0
                adapterError = LV_WORKFLOW_ADAPTER_RESPONSE
            } catch {
                body = Data()
                status = 0
                adapterError = LV_WORKFLOW_ADAPTER_NETWORK
            }
            self.transportTasks.removeValue(forKey: operation)
            guard !self.shuttingDown else { return }
            _ = self.completion?.completeTransport(operation: operation, requestID: requestID,
                error: adapterError, httpStatus: status, body: body)
        }
        transportTasks[operation] = task
    }

    func inspectTarget(operation: UInt64, requestID: UInt64, originalToken: UInt64) {
        guard !shuttingDown else { return }
        guard targeting.accessibilityTrusted else {
            completeInspect(operation: operation, requestID: requestID,
                error: LV_WORKFLOW_ADAPTER_INSERTION_UNAVAILABLE, target: emptyTarget)
            return
        }
        guard let context = contexts[operation] else {
            completeInspect(operation: operation, requestID: requestID,
                error: LV_WORKFLOW_ADAPTER_TARGET_CHANGED, target: emptyTarget)
            return
        }
        guard let current = targeting.focusedTarget() else {
            completeInspect(operation: operation, requestID: requestID,
                error: LV_WORKFLOW_ADAPTER_UNAVAILABLE, target: emptyTarget)
            return
        }
        if context.useCurrentField {
            let token = allocate(current, operation: operation)
            completeInspect(operation: operation, requestID: requestID, error: LV_WORKFLOW_ADAPTER_NONE,
                target: bridgeTarget(current, token: token, matchesOriginal: context.originalPID == current.pid))
            return
        }
        guard let original = targets[originalToken], original.operation == operation,
            targeting.targetsMatch(original.target, current) else {
            targeting.release(current)
            completeInspect(operation: operation, requestID: requestID,
                error: LV_WORKFLOW_ADAPTER_TARGET_CHANGED, target: emptyTarget)
            return
        }
        targeting.release(current)
        completeInspect(operation: operation, requestID: requestID, error: LV_WORKFLOW_ADAPTER_NONE,
            target: bridgeTarget(current, token: originalToken,
                matchesOriginal: context.originalPID == current.pid))
    }

    func insertText(operation: UInt64, requestID: UInt64, targetToken: UInt64, text: String) {
        guard !shuttingDown, let targetRecord = targets[targetToken],
            targetRecord.operation == operation, let context = contexts[operation] else {
            _ = completion?.completeInsert(operation: operation, requestID: requestID,
                error: LV_WORKFLOW_ADAPTER_TARGET_CHANGED)
            return
        }
        let target = targetRecord.target
        let task = Task { @MainActor [weak self] in
            guard let self else { return }
            let result = await self.paste.insert(text, validate: {
                guard !Task.isCancelled, let current = self.targeting.focusedTarget() else { return false }
                defer { self.targeting.release(current) }
                guard self.targeting.targetsMatch(target, current) else { return false }
                guard current.editable else { return false }
                return canInsert(currentTarget: context.useCurrentField,
                    originalMatches: context.originalPID == target.pid,
                    frontmostMatches: self.targeting.frontmostPID() == target.pid,
                    secure: current.secure)
            }, dispatch: { self.targeting.postPaste(to: target.pid) })
            self.insertionTasks.removeValue(forKey: operation)
            self.releaseOperation(operation)
            guard !self.shuttingDown else { return }
            let error: LVWorkflowAdapterError
            switch result {
            case .dispatched: error = LV_WORKFLOW_ADAPTER_NONE
            case .targetChanged: error = LV_WORKFLOW_ADAPTER_TARGET_CHANGED
            case .unavailable: error = LV_WORKFLOW_ADAPTER_UNAVAILABLE
            case .canceled: error = LV_WORKFLOW_ADAPTER_CANCELED
            }
            _ = self.completion?.completeInsert(operation: operation, requestID: requestID, error: error)
        }
        insertionTasks[operation] = task
    }

    func captureCancel(operation: UInt64) {
        captureTasks.removeValue(forKey: operation)?.cancel()
        capture.cancel()
    }

    func transportCancel(operation: UInt64) {
        transportTasks.removeValue(forKey: operation)?.cancel()
    }

    func insertionCancel(operation: UInt64) {
        insertionTasks.removeValue(forKey: operation)?.cancel()
        releaseOperation(operation)
    }

    // WorkflowCore's copy-fallback and successful terminal callbacks do not
    // issue insertion_cancel. The Controller calls this for either terminal
    // outcome to release native target ownership exactly once per operation.
    // It also handles a synchronous invalid-config terminal callback before
    // captureOriginal has consumed the prepared target snapshot.
    func finalizeOperation(_ operation: UInt64) {
        discardPreparedStart()
        captureTasks.removeValue(forKey: operation)?.cancel()
        transportTasks.removeValue(forKey: operation)?.cancel()
        insertionTasks.removeValue(forKey: operation)?.cancel()
        releaseOperation(operation)
    }

    func prepareForShutdown() {
        guard !shuttingDown else { return }
        shuttingDown = true
        completion = nil
        discardPreparedStart()
        captureShutdown()
        transportShutdown()
        insertionShutdown()
    }

    func captureShutdown() {
        guard !captureShutdownComplete else { return }
        captureShutdownComplete = true
        captureCancelAll()
    }

    func transportShutdown() {
        guard !transportShutdownComplete else { return }
        transportShutdownComplete = true
        transportCancelAll()
        transport.close()
    }

    func insertionShutdown() {
        guard !insertionShutdownComplete else { return }
        insertionShutdownComplete = true
        insertionCancelAll()
    }

    nonisolated func abandonWithoutBridge() {
        // The bridge may be the adapter's final owner. Retain only this native
        // adapter through cleanup; its completion reference stays weak.
        Task { @MainActor [self] in
            self.prepareForShutdown()
        }
    }

    private var emptyTarget: WorkflowBridgeTarget {
        WorkflowBridgeTarget(token: 0, editable: false, secure: false, frontmost: false, matchesOriginal: false)
    }

    private func allocate(_ target: WorkflowNativeTarget, operation: UInt64) -> UInt64 {
        let token = nextTargetToken
        nextTargetToken &+= 1
        targets[token] = TargetRecord(operation: operation, target: target)
        return token
    }

    private func bridgeTarget(_ target: WorkflowNativeTarget, token: UInt64,
        matchesOriginal: Bool) -> WorkflowBridgeTarget {
        WorkflowBridgeTarget(token: token, editable: target.editable, secure: target.secure,
            frontmost: targeting.frontmostPID() == target.pid, matchesOriginal: matchesOriginal)
    }

    private func completeOriginal(operation: UInt64, requestID: UInt64, error: LVWorkflowAdapterError,
        target: WorkflowBridgeTarget) {
        _ = completion?.completeOriginal(operation: operation, requestID: requestID, error: error, target: target)
    }

    private func completeInspect(operation: UInt64, requestID: UInt64, error: LVWorkflowAdapterError,
        target: WorkflowBridgeTarget) {
        _ = completion?.completeInspect(operation: operation, requestID: requestID, error: error, target: target)
    }

    private func releaseOperation(_ operation: UInt64) {
        let context = contexts.removeValue(forKey: operation)
        var released = Set<UInt64>()
        if let original = context?.original, released.insert(original.identity).inserted {
            targeting.release(original)
        }
        let owned = targets.filter { $0.value.operation == operation }
        for (token, record) in owned {
            if released.insert(record.target.identity).inserted {
                targeting.release(record.target)
            }
            targets.removeValue(forKey: token)
        }
    }

    private func captureCancelAll() {
        for task in captureTasks.values { task.cancel() }
        captureTasks.removeAll()
        capture.cancel()
    }

    private func transportCancelAll() {
        for task in transportTasks.values { task.cancel() }
        transportTasks.removeAll()
    }

    private func insertionCancelAll() {
        for task in insertionTasks.values { task.cancel() }
        insertionTasks.removeAll()
        let operations = Set(contexts.keys).union(targets.values.map(\.operation))
        for operation in operations { releaseOperation(operation) }
    }
}
