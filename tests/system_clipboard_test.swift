import AppKit
@main struct ClipboardTests {
    @MainActor static func main() async throws {
        let board = NSPasteboard.withUniqueName()
        defer { board.releaseGlobally() }
        let clipboard = SystemClipboard(board)
        let item = NSPasteboardItem()
        item.setData(Data([0,1,255]), forType: NSPasteboard.PasteboardType("org.lemonvox.test.binary"))
        precondition(board.writeObjects([item]))
        let result = await PasteTransaction(clipboard:clipboard).insert("synthetic new",validate:{true},dispatch:{true})
        precondition(result == .dispatched && board.string(forType: .string) == "synthetic new")
        precondition(board.data(forType: NSPasteboard.PasteboardType("org.lemonvox.test.binary")) == nil,
                     "Ordinary paste replaces old clipboard formats without restoration")
        precondition(InsertionMetadata(role:kAXTextAreaRole,secure:false).editable)
        precondition(!InsertionMetadata(role:kAXTextFieldRole,secure:true).editable)
        precondition(!InsertionMetadata(role:kAXButtonRole,secure:false).editable)
        print("PASS: isolated native pasteboard retains transcript and replaces previous formats")
    }
}
