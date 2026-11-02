import Foundation

private func fixtureDirectory() throws -> URL {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("lemonvox-diagnostics-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    return directory
}

private func permissions(of url: URL) throws -> Int {
    guard let value = try FileManager.default.attributesOfItem(atPath: url.path)[.posixPermissions] as? NSNumber else {
        fatalError("Missing permissions for \(url.path)")
    }
    return value.intValue & 0o777
}

private func normalModeFiltersTraceEvents() throws {
    let directory = try fixtureDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }

    let log = DiagnosticLog(directory: directory, limit: 2_048)
    log.record(.appStarted)
    log.record(.settingsOpened)
    log.record(.configSaved)
    log.record(.discoveryStarted)
    log.record(.discoverySucceeded)
    log.record(.recordingStarted)
    log.record(.transcriptionStarted)
    log.record(.cleanupStarted)
    log.record(.pasteDispatched)
    log.record(.insertionSucceeded)
    log.record(.insertionFallback)
    log.record(.canceled)
    log.record(.discoveryFailed, error: DictationError.http(503))
    log.record(.dictationFailed, error: DictationError.upstream(502, "synthetic provider response"))

    let contents = log.contents()
    for expected in ["app_started", "config_saved", "insertion_fallback", "discovery_failed", "dictation_failed", "http=503", "http=502"] {
        precondition(contents.contains(expected), "Missing normal diagnostic \(expected)")
    }
    for excluded in ["settings_opened", "discovery_started", "discovery_succeeded", "recording_started", "transcription_started", "cleanup_started", "paste_dispatched", "insertion_succeeded", "canceled"] {
        precondition(!contents.contains(excluded), "Normal mode retained trace event \(excluded)")
    }
}

private func diagnosticsNeverPersistSyntheticPrivateContent(debugEnabled: Bool) throws {
    let directory = try fixtureDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }

    let transcript = "SYNTHETIC_TRANSCRIPT_DO_NOT_PERSIST"
    let audio = "SYNTHETIC_AUDIO_BYTES_DO_NOT_PERSIST"
    let credential = "Bearer SYNTHETIC_CREDENTIAL_DO_NOT_PERSIST"
    let clipboard = "SYNTHETIC_CLIPBOARD_DO_NOT_PERSIST"
    let privateField = "SYNTHETIC_PRIVATE_FIELD_DO_NOT_PERSIST"
    let providerResponse = "{\"error\":{\"message\":\"\(transcript) \(audio) \(credential) \(clipboard) \(privateField)\"}}"
    let log = DiagnosticLog(directory: directory, limit: 2_048, debugEnabled: debugEnabled)

    log.record(.dictationFailed, error: DictationError.upstream(502, providerResponse))
    log.record(.discoveryFailed, error: NSError(
        domain: providerResponse,
        code: 17,
        userInfo: [NSLocalizedDescriptionKey: providerResponse]
    ))

    let contents = log.contents()
    precondition(contents.contains("dictation_failed") && contents.contains("discovery_failed"))
    precondition(contents.contains("http=502") && contents.contains("category=other"))
    for forbidden in [transcript, audio, credential, clipboard, privateField, providerResponse] {
        precondition(!contents.contains(forbidden), "Diagnostic log persisted private content")
    }
}

private func debugModeRecordsFixedTraceEventsAndCanBeDisabled() throws {
    let directory = try fixtureDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }

    let log = DiagnosticLog(directory: directory, limit: 2_048, debugEnabled: true)
    precondition(log.debugEnabled)
    log.record(.recordingStarted)

    log.debugEnabled = false
    precondition(!log.debugEnabled)
    log.record(.transcriptionStarted)

    log.debugEnabled = true
    log.record(.cleanupStarted)

    let contents = log.contents()
    precondition(contents.contains("recording_started") && contents.contains("cleanup_started"))
    precondition(!contents.contains("transcription_started"))
}

private func existingBackupIsPrivateAfterNonRotatingWrite() throws {
    let directory = try fixtureDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }

    let log = DiagnosticLog(directory: directory, limit: 2_048)
    let previous = log.file.appendingPathExtension("1")
    try Data("synthetic previous diagnostic\n".utf8).write(to: previous)
    try FileManager.default.setAttributes([.posixPermissions: 0o644], ofItemAtPath: previous.path)

    log.record(.appStarted)
    let previousPermissions = try permissions(of: previous)
    precondition(previousPermissions == 0o600)
}

private func repeatedRotationKeepsOnlyCurrentAndPreviousPrivateFiles() throws {
    let directory = try fixtureDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }

    let limit = 128
    let log = DiagnosticLog(directory: directory, limit: limit)
    for _ in 0..<80 { log.record(.appStarted) }

    let previous = log.file.appendingPathExtension("1")
    let unexpected = log.file.appendingPathExtension("2")
    let currentSize = try Data(contentsOf: log.file).count
    let previousSize = try Data(contentsOf: previous).count
    let directoryPermissions = try permissions(of: directory)
    let currentPermissions = try permissions(of: log.file)
    let previousPermissions = try permissions(of: previous)
    precondition(currentSize <= limit && previousSize <= limit)
    precondition(currentSize + previousSize <= limit * 2)
    precondition(!FileManager.default.fileExists(atPath: unexpected.path))
    precondition(directoryPermissions == 0o700)
    precondition(currentPermissions == 0o600)
    precondition(previousPermissions == 0o600)
}

@main struct DiagnosticsTests {
    static func main() throws {
        try normalModeFiltersTraceEvents()
        try diagnosticsNeverPersistSyntheticPrivateContent(debugEnabled: false)
        try diagnosticsNeverPersistSyntheticPrivateContent(debugEnabled: true)
        try debugModeRecordsFixedTraceEventsAndCanBeDisabled()
        try existingBackupIsPrivateAfterNonRotatingWrite()
        try repeatedRotationKeepsOnlyCurrentAndPreviousPrivateFiles()
        print("PASS: quiet diagnostics, content exclusion, and bounded private rotation")
    }
}
