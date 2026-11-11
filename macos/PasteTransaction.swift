import Foundation

@MainActor protocol ClipboardAccess {
    func setText(_ text: String) throws
}
enum PasteResult { case dispatched, targetChanged, unavailable, canceled }

@MainActor final class PasteTransaction {
    private let clipboard: ClipboardAccess
    private let modifiersReleased: () -> Bool
    private let modifierTimeout: UInt64
    private(set) var active = false
    init(clipboard: ClipboardAccess, modifiersReleased: @escaping () -> Bool = { true },
         modifierTimeout: UInt64 = 5_000_000_000) {
        self.clipboard = clipboard
        self.modifiersReleased = modifiersReleased
        self.modifierTimeout = modifierTimeout
    }
    func insert(_ text: String, validate: () -> Bool, dispatch: () -> Bool) async -> PasteResult {
        guard !active, !Task.isCancelled else { return .canceled }
        active = true
        defer { active = false }
        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: .nanoseconds(Int64(clamping: modifierTimeout)))
        while !modifiersReleased() {
            guard !Task.isCancelled, validate() else { return .canceled }
            guard clock.now < deadline else { return .unavailable }
            do { try await Task.sleep(nanoseconds: 25_000_000) }
            catch { return .canceled }
        }
        guard !Task.isCancelled, validate() else { return .canceled }
        // Ordinary paste owns the clipboard afterward, just like Ubuntu.
        // No old-data snapshot, AX field lookup, or asynchronous gap before dispatch.
        do { try clipboard.setText(text) }
        catch { return .unavailable }
        guard !Task.isCancelled, validate() else { return .canceled }
        return dispatch() ? .dispatched : .unavailable
    }
}
