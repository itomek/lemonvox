import Foundation
final class TransientAudio {
    let directory: URL
    var url: URL { directory.appendingPathComponent("dictation.wav") }
    init() throws {
        directory = FileManager.default.temporaryDirectory.appendingPathComponent("lemonvox-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
    }
    func remove() { try? FileManager.default.removeItem(at: directory) }
    deinit { remove() }
}
