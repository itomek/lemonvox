import Foundation
@MainActor final class FakeClipboard: ClipboardAccess {
    var text = "previous clipboard"
    var writes = 0
    var failWrite = false
    func setText(_ text: String) throws {
        if failWrite { throw CocoaError(.fileWriteUnknown) }
        writes += 1
        self.text = text
    }
}
@main struct PasteTests {
    @MainActor static func main() async throws {
        let clipboard = FakeClipboard()
        var events = 0
        let paste = PasteTransaction(clipboard: clipboard)
        let result = await paste.insert("synthetic", validate: { true }, dispatch: { events += 1; return true })
        precondition(result == .dispatched && events == 1 && clipboard.text == "synthetic")
        let denied = await paste.insert("dispatch denied", validate: { true }, dispatch: { false })
        precondition(denied == .unavailable && clipboard.text == "dispatch denied")
        clipboard.failWrite = true
        let failed = await paste.insert("synthetic", validate: { true }, dispatch: { fatalError("Failed clipboard write dispatched") })
        precondition(failed == .unavailable)
        clipboard.failWrite = false
        var released = false
        let waiting = PasteTransaction(clipboard: clipboard, modifiersReleased: { released })
        let canceled = Task { await waiting.insert("canceled", validate: { true }, dispatch: { fatalError("Canceled paste dispatched") }) }
        while !waiting.active { await Task.yield() }
        canceled.cancel()
        let cancelResult = await canceled.value
        precondition(cancelResult == .canceled && clipboard.text == "dispatch denied")
        let timeout = PasteTransaction(clipboard: clipboard, modifiersReleased: { false }, modifierTimeout: 0)
        let timed = await timeout.insert("timed out", validate: { true }, dispatch: { fatalError("Held modifiers dispatched") })
        precondition(timed == .unavailable && clipboard.text == "dispatch denied")
        let releaseTask = Task { await waiting.insert("released", validate: { true }, dispatch: { events += 1; return true }) }
        while !waiting.active { await Task.yield() }
        precondition(clipboard.text == "dispatch denied")
        released = true
        let releasedResult = await releaseTask.value
        precondition(releasedResult == .dispatched && events == 2 && clipboard.text == "released")
        print("PASS: one paste, retained clipboard, dispatch/write failures, modifier release/timeout and cancellation")
    }
}
