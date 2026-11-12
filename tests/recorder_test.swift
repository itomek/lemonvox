import AVFoundation

@MainActor final class FakePermission: MicrophonePermission {
    let status: AVAuthorizationStatus
    var requests = 0
    private var pendingPermission: CheckedContinuation<Bool, Never>?
    private var requestStarted: CheckedContinuation<Void, Never>?
    init(_ status: AVAuthorizationStatus) { self.status = status }
    func request() async -> Bool {
        requests += 1
        return await withCheckedContinuation { continuation in
            pendingPermission = continuation
            requestStarted?.resume()
            requestStarted = nil
        }
    }
    func waitForRequest() async {
        if requests > 0 { return }
        await withCheckedContinuation { requestStarted = $0 }
    }
    func resolveRequest(allowed: Bool) {
        guard let continuation = pendingPermission else { preconditionFailure("Permission request has not started") }
        pendingPermission = nil
        continuation.resume(returning: allowed)
    }
}
@main struct RecorderTests {
    @MainActor static func main() async throws {
        let denied = FakePermission(.denied)
        let capture = Recorder(permission: denied)
        do { try await capture.start(); fatalError("Denied microphone unexpectedly recorded") }
        catch DictationError.microphone { }
        precondition(denied.requests == 0 && !capture.isRecording)
        let waiting = FakePermission(.notDetermined)
        let late = Recorder(permission: waiting)
        let start = Task { try await late.start() }
        // Establish the ordering explicitly: the permission request is suspended,
        // cancellation happens, then the fake permission response arrives.
        await waiting.waitForRequest()
        precondition(waiting.requests == 1 && !late.isRecording)
        start.cancel()
        waiting.resolveRequest(allowed: true)
        do { try await start.value; fatalError("Canceled permission unexpectedly recorded") }
        catch is CancellationError { }
        precondition(waiting.requests == 1 && !late.isRecording)
        print("PASS: denied mic does not prompt/record; canceled late permission never records (fake permission only)")
    }
}
