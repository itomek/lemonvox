import Foundation

@MainActor protocol ClipboardAccess {
    var changeCount:Int { get }
    func snapshot() throws -> [[String:Data]]
    func write(_ items:[[String:Data]],expectedCount:Int) throws -> Int
    func owns(token:String,count:Int) -> Bool
}
enum PasteResult { case dispatched, targetChanged, unavailable, canceled }

@MainActor final class PasteTransaction {
    private let clipboard:ClipboardAccess
    private let delay:UInt64
    private(set) var active=false
    init(clipboard:ClipboardAccess,delay:UInt64=500_000_000) { self.clipboard=clipboard; self.delay=delay }
    func insert(_ text:String,validate:() -> Bool,dispatch:() -> Bool) async -> PasteResult {
        guard !active, !Task.isCancelled else { return .canceled }
        guard validate() else { return .targetChanged }
        active=true; defer { active=false }
        let previousCount=clipboard.changeCount
        let snapshot:[[String:Data]]
        do { snapshot=try clipboard.snapshot() } catch { return .unavailable }
        // Snapshotting lazy representations can take time; never overwrite a newer copy.
        guard validate(), clipboard.changeCount == previousCount else { return .targetChanged }
        let token=UUID().uuidString
        let ownedCount:Int
        do {
            ownedCount=try clipboard.write([["public.utf8-plain-text":Data(text.utf8),"com.itomek.lemonvox.paste-token":Data(token.utf8)]],expectedCount:previousCount)
        } catch { return .unavailable }
        defer {
            if clipboard.owns(token:token,count:ownedCount) { _ = try? clipboard.write(snapshot,expectedCount:ownedCount) }
        }
        guard !Task.isCancelled else { return .canceled }
        guard validate() else { return .targetChanged }
        guard dispatch() else { return .unavailable }
        // Keep data available while the destination handles its queued paste event.
        // There is no cross-application acknowledgement that the edit is visible.
        // Once dispatched, cancellation must not restore before this grace period.
        let grace=delay
        await Task.detached { try? await Task.sleep(nanoseconds:grace) }.value
        return .dispatched
    }
}
