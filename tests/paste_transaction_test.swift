import Foundation
@MainActor final class FakeClipboard: ClipboardAccess {
    var changeCount=0
    var items:[[String:Data]]=[["private/opaque":Data([1,2,3]),"public.text":Data([4])]]
    func snapshot() throws -> [[String:Data]] { items }
    var failWrite=false
    func write(_ items:[[String:Data]],expectedCount:Int) throws -> Int {
        guard changeCount == expectedCount, !failWrite else { throw CocoaError(.fileWriteUnknown) }
        self.items=items; changeCount += 1; return changeCount
    }
    func owns(token:String,count:Int) -> Bool { changeCount == count && items.first?["com.itomek.lemonvox.paste-token"] == Data(token.utf8) }
}
@main struct PasteTests {
    @MainActor static func main() async throws {
        let clipboard=FakeClipboard(); let original=clipboard.items
        var valid=true; var events=0
        let paste=PasteTransaction(clipboard:clipboard,delay:1_000_000)
        let result=await paste.insert("synthetic",validate:{ valid },dispatch:{ events += 1; return true })
        precondition(result == .dispatched && events == 1 && clipboard.items == original)
        let changed=await paste.insert("synthetic",validate:{ true },dispatch:{ clipboard.items=[["new-copy":Data([9])]]; clipboard.changeCount += 1; return true })
        precondition(changed == .dispatched && clipboard.items == [["new-copy":Data([9])]])
        clipboard.items=original
        var validations=0
        let rejected=await paste.insert("synthetic",validate:{ validations += 1; return validations == 1 },dispatch:{ events += 1; return true })
        precondition(rejected == .targetChanged && clipboard.items == original && events == 1)
        valid=false
        let initial=await paste.insert("synthetic",validate:{ valid },dispatch:{ events += 1; return true })
        precondition(initial == .targetChanged && events == 1)
        let failing=await paste.insert("synthetic",validate:{ true },dispatch:{ false })
        precondition(failing == .unavailable && clipboard.items == original)
        clipboard.failWrite=true
        let failure=await paste.insert("synthetic",validate:{ true },dispatch:{ fatalError("Failed clipboard write dispatched") })
        precondition(failure == .unavailable && clipboard.items == original)
        clipboard.failWrite=false
        let slow=PasteTransaction(clipboard:clipboard,delay:20_000_000)
        let cancelTask=Task { await slow.insert("synthetic",validate:{ true },dispatch:{ true }) }
        try? await Task.sleep(nanoseconds:1_000_000); cancelTask.cancel()
        _ = await cancelTask.value
        precondition(clipboard.items == original, "Canceled dispatched paste must still restore clipboard")
        print("PASS: single paste dispatch, opaque restore, intervening copy preservation, focus rechecks and failed dispatch cleanup")
    }
}
