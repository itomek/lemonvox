import AVFoundation

@MainActor final class FakePermission: MicrophonePermission {
    let status: AVAuthorizationStatus
    var requests = 0
    init(_ status: AVAuthorizationStatus) { self.status = status }
    func request() async -> Bool {
        requests += 1
        try? await Task.sleep(nanoseconds: 50_000_000)
        return true
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
        try await Task.sleep(nanoseconds: 10_000_000)
        start.cancel()
        do { try await start.value; fatalError("Canceled permission unexpectedly recorded") }
        catch is CancellationError { }
        precondition(waiting.requests == 1 && !late.isRecording)
        print("PASS: denied mic does not prompt/record; canceled late permission never records (fake permission only)")
    }
}
