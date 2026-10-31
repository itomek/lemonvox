import Foundation

enum SafeDisplay {
    static func message(_ input: String) -> String {
        var value = String(input.prefix(2000))
        for pattern in [
            #"(?i)(authorization\s*[:=]\s*|bearer\s+)[^\s,;]+(?:\s+[^\s,;]+)?"#,
            #"(?i)([\"']?(?:api[\s_-]*key|access[\s_-]*token|refresh[\s_-]*token|password|secret)[\"']?\s*[:=]\s*)[\"']?[^\s,;\"'}]+"#,
            #"\b(?:sk-|fw-)[A-Za-z0-9_-]+"#,
            #"[A-Za-z0-9_+/=-]{32,}"#,
            #"https?://[^\s<>\"']+"#
        ] {
            value = value.replacingOccurrences(of: pattern, with: "[redacted]", options: .regularExpression)
        }
        return value.replacingOccurrences(of: "\u{0}", with: "")
    }
    static func error(_ error: Error) -> String {
        message((error as? LocalizedError)?.errorDescription ?? "Request failed; see diagnostic logs")
    }
}

enum DiagnosticEvent: String {
    case appStarted = "app_started", settingsOpened = "settings_opened", configSaved = "config_saved"
    case discoveryStarted = "discovery_started", discoverySucceeded = "discovery_succeeded", discoveryFailed = "discovery_failed"
    case recordingStarted = "recording_started", transcriptionStarted = "transcription_started", cleanupStarted = "cleanup_started"
    case pasteDispatched = "paste_dispatched"
    case canceled, insertionSucceeded = "insertion_succeeded", insertionFallback = "insertion_fallback", dictationFailed = "dictation_failed"
}

/// Only fixed event names, error categories and HTTP codes reach disk. Never accepts free-form log text.
final class DiagnosticLog {
    static let shared = DiagnosticLog()
    let file: URL
    private let limit: Int
    private let lock = NSLock()
    private(set) var problem: String?
    init(directory: URL = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/LemonVox"), limit: Int = 1_048_576) {
        file = directory.appendingPathComponent("lemonvox.log"); self.limit = limit
    }
    func record(_ event: DiagnosticEvent, error: Error? = nil) {
        lock.lock(); defer { lock.unlock() }
        var category = ""
        if let error {
            switch error {
            case DictationError.http(let code): category = " http=\(code)"
            case DictationError.upstream(let code, _): category = " http=\(code)"
            case is CancellationError: category = " category=canceled"
            case is DictationError: category = " category=dictation"
            case is URLError: category = " category=network"
            default: category = " category=other"
            }
        }
        let line = "\(ISO8601DateFormatter().string(from: Date())) \(event.rawValue)\(category)\n"
        do {
            let fm = FileManager.default
            try fm.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
            let size = (try? fm.attributesOfItem(atPath: file.path)[.size] as? Int) ?? 0
            if size + line.utf8.count > limit {
                let previous = file.appendingPathExtension("1")
                if fm.fileExists(atPath: previous.path) { try fm.removeItem(at: previous) }
                if fm.fileExists(atPath: file.path) { try fm.moveItem(at: file, to: previous) }
            }
            if !fm.fileExists(atPath: file.path) { guard fm.createFile(atPath: file.path, contents: nil, attributes: [.posixPermissions:0o600]) else { throw CocoaError(.fileWriteUnknown) } }
            let handle = try FileHandle(forWritingTo: file)
            defer { try? handle.close() }
            try handle.seekToEnd(); try handle.write(contentsOf: Data(line.utf8))
            problem = nil
        } catch { problem = "Diagnostic log could not be written" }
    }
    func contents() -> String {
        lock.lock(); defer { lock.unlock() }
        return (try? String(contentsOf: file, encoding: .utf8)) ?? "No diagnostic log yet."
    }
}
