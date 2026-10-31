import AppKit
@main struct ClipboardTests {
    @MainActor static func main() async throws {
        let board=NSPasteboard.withUniqueName(); defer { board.releaseGlobally() }
        let clipboard=SystemClipboard(board)
        let original:[[String:Data]]=[["public.utf8-plain-text":Data("synthetic original".utf8),"org.lemonvox.test.binary":Data([0,1,255])],["org.lemonvox.test.second":Data([8,9])]]
        _ = try clipboard.write(original,expectedCount:board.changeCount)
        let result=await PasteTransaction(clipboard:clipboard,delay:1_000_000).insert("synthetic new",validate:{true},dispatch:{true})
        let restored=try clipboard.snapshot()
        precondition(result == .dispatched && restored == original)
        let token="synthetic-token"
        let receipt=try clipboard.write([["com.itomek.lemonvox.paste-token":Data(token.utf8)]],expectedCount:board.changeCount)
        precondition(clipboard.owns(token:token,count:receipt))
        _ = board.clearContents()
        precondition(!clipboard.owns(token:token,count:receipt))
        precondition(InsertionMetadata(role:kAXTextAreaRole,secure:false).editable)
        precondition(!InsertionMetadata(role:kAXTextFieldRole,secure:true).editable)
        precondition(!InsertionMetadata(role:kAXButtonRole,secure:false).editable)
        print("PASS: isolated native pasteboard preserves multi-item binary formats; ownership counters/token and secure/role guards")
    }
}
