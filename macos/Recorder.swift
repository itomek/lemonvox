import AVFoundation

@MainActor protocol MicrophonePermission {
    var status: AVAuthorizationStatus { get }
    func request() async -> Bool
}
@MainActor struct SystemMicrophonePermission: MicrophonePermission {
    var status: AVAuthorizationStatus { AVCaptureDevice.authorizationStatus(for: .audio) }
    func request() async -> Bool { await AVCaptureDevice.requestAccess(for: .audio) }
}

@MainActor final class Recorder {
    private let permission: MicrophonePermission
    init(permission: MicrophonePermission) { self.permission = permission }
    convenience init() { self.init(permission: SystemMicrophonePermission()) }
    private var recorder: AVAudioRecorder?
    private var audio: TransientAudio?
    var isRecording: Bool { recorder?.isRecording == true }
    var level: Double {
        recorder?.updateMeters()
        let power = Double(recorder?.averagePower(forChannel: 0) ?? -60)
        return min(1, max(0, (power + 60) / 60))
    }
    func start() async throws {
        let status = permission.status
        if status == .notDetermined {
            guard await permission.request() else { throw DictationError.microphone }
        } else if status != .authorized { throw DictationError.microphone }
        try Task.checkCancellation()
        do {
            let owned = try TransientAudio()
            audio = owned
            let settings: [String: Any] = [AVFormatIDKey: kAudioFormatLinearPCM, AVSampleRateKey: 16000,
                AVNumberOfChannelsKey: 1, AVLinearPCMBitDepthKey: 16, AVLinearPCMIsBigEndianKey: false,
                AVLinearPCMIsFloatKey: false]
            let capture = try AVAudioRecorder(url: owned.url, settings: settings)
            recorder = capture
            capture.isMeteringEnabled = true
            guard capture.prepareToRecord(), capture.record(forDuration: 120) else { throw DictationError.recording }
            try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: owned.url.path)
        } catch {
            cancel()
            throw DictationError.recording
        }
    }
    func finish() throws -> Data {
        defer { cancel() }
        recorder?.stop()
        guard let audio else { throw DictationError.recording }
        let data = try Data(contentsOf: audio.url)
        guard data.count > 44 else { throw DictationError.recording }
        return data
    }
    func cancel() {
        recorder?.stop(); recorder = nil
        audio?.remove(); audio = nil
    }
}
