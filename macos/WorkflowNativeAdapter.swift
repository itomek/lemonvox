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

@MainActor protocol WorkflowNativeEventPosting: AnyObject {
    func postPaste() -> Bool
}

@MainActor final class SystemWorkflowEventPosting: WorkflowNativeEventPosting {
    func postPaste() -> Bool { postPasteAtCurrentFocus() }
}

@MainActor final class WorkflowNativeAdapter: WorkflowBridgeNative {
    private let capture: any WorkflowNativeCapture
    private let transport: any WorkflowNativeTransport
    private let paste: any WorkflowNativePaste
    private let posting: any WorkflowNativeEventPosting
    private weak var completion: (any WorkflowNativeCompleting)?
    private var captureTasks: [UInt64: Task<Void, Never>] = [:]
    private var transportTasks: [UInt64: Task<Void, Never>] = [:]
    private var insertionTasks: [UInt64: Task<Void, Never>] = [:]
    private var shuttingDown = false
    private var captureShutdownComplete = false
    private var transportShutdownComplete = false
    private var insertionShutdownComplete = false
    var onCaptureWillStart: () -> Void = {}

    init(capture: any WorkflowNativeCapture, transport: any WorkflowNativeTransport,
        paste: any WorkflowNativePaste, posting: any WorkflowNativeEventPosting) {
        self.capture = capture
        self.transport = transport
        self.paste = paste
        self.posting = posting
    }

    func attach(_ completion: any WorkflowNativeCompleting) {
        guard !shuttingDown else { return }
        self.completion = completion
    }

    var isRecording: Bool { capture.isRecording }
    var level: Double { capture.level }
    var pasteActive: Bool { paste.active }

    // Current-focus mode bypasses original capture and inspection in WorkflowCore.
    // Refuse an accidentally requested legacy target operation rather than invent AX facts.
    func captureOriginal(operation: UInt64, requestID: UInt64) {
        _ = completion?.completeOriginal(operation: operation, requestID: requestID,
            error: LV_WORKFLOW_ADAPTER_UNAVAILABLE, target: emptyTarget)
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
            } catch DictationError.serverTimeout {
                body = Data()
                status = 0
                adapterError = LV_WORKFLOW_ADAPTER_TIMEOUT
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
        _ = completion?.completeInspect(operation: operation, requestID: requestID,
            error: LV_WORKFLOW_ADAPTER_UNAVAILABLE, target: emptyTarget)
    }

    func insertText(operation: UInt64, requestID: UInt64, targetToken: UInt64, text: String) {
        guard !shuttingDown, targetToken == 0, insertionTasks[operation] == nil else { return }
        let task = Task { @MainActor [weak self] in
            guard let self, !Task.isCancelled, !self.shuttingDown else { return }
            let result = await self.paste.insert(text, validate: {
                !Task.isCancelled && !self.shuttingDown
            }, dispatch: { self.posting.postPaste() })
            self.insertionTasks.removeValue(forKey: operation)
            guard !self.shuttingDown else { return }
            let error: LVWorkflowAdapterError
            switch result {
            case .dispatched: error = LV_WORKFLOW_ADAPTER_NONE
            case .targetChanged, .canceled: error = LV_WORKFLOW_ADAPTER_CANCELED
            case .unavailable: error = LV_WORKFLOW_ADAPTER_UNAVAILABLE
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
    }

    // WorkflowCore's copy-fallback and successful terminal callbacks do not
    // issue insertion_cancel. The Controller calls this for either terminal
    // outcome to release outstanding native task ownership for that operation.
    func finalizeOperation(_ operation: UInt64) {
        captureTasks.removeValue(forKey: operation)?.cancel()
        transportTasks.removeValue(forKey: operation)?.cancel()
        insertionTasks.removeValue(forKey: operation)?.cancel()
    }

    func prepareForShutdown() {
        guard !shuttingDown else { return }
        shuttingDown = true
        completion = nil
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
    }
}
