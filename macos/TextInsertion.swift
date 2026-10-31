import AppKit
import ApplicationServices
import Carbon

@MainActor final class SystemClipboard: ClipboardAccess {
    private let board:NSPasteboard
    init(_ board:NSPasteboard = .general) { self.board=board }
    var changeCount:Int { board.changeCount }
    func snapshot() throws -> [[String:Data]] {
        var size=0
        return try (board.pasteboardItems ?? []).map { item in
            var values:[String:Data]=[:]
            for type in item.types {
                guard let data=item.data(forType:type) else { throw DictationError.response }
                size += data.count
                guard size <= 32*1024*1024 else { throw DictationError.response }
                values[type.rawValue]=data
            }
            return values
        }
    }
    func owns(token:String,count:Int) -> Bool {
        guard board.changeCount == count else { return false }
        let value=board.data(forType:NSPasteboard.PasteboardType("com.itomek.lemonvox.paste-token"))
        return value == Data(token.utf8) && board.changeCount == count
    }
    private func objects(_ items:[[String:Data]]) -> [NSPasteboardItem] {
        items.map { values in
            let item=NSPasteboardItem()
            for (type,data) in values { item.setData(data,forType:NSPasteboard.PasteboardType(type)) }
            return item
        }
    }
    func write(_ items:[[String:Data]],expectedCount:Int) throws -> Int {
        guard board.changeCount == expectedCount else { throw DictationError.response }
        let previous=try snapshot()
        guard board.changeCount == expectedCount else { throw DictationError.response }
        let receipt=board.clearContents()
        // clearContents returns the ownership count; writing data does not advance it.
        guard board.changeCount == receipt else { throw DictationError.response }
        if !items.isEmpty && !board.writeObjects(objects(items)) {
            // If our empty declaration is still current, recover the prior items.
            if board.changeCount == receipt { _ = board.writeObjects(objects(previous)) }
            throw DictationError.response
        }
        return receipt
    }

}

struct InsertionMetadata {
    let role:String?
    let secure:Bool
    var editable:Bool { [kAXTextFieldRole,kAXTextAreaRole,kAXComboBoxRole].contains(role ?? "") && !secure }
}
func insertionMetadata(_ target:AXUIElement) -> InsertionMetadata {
    var role:CFTypeRef?, subrole:CFTypeRef?
    AXUIElementCopyAttributeValue(target,kAXRoleAttribute as CFString,&role)
    AXUIElementCopyAttributeValue(target,kAXSubroleAttribute as CFString,&subrole)
    return InsertionMetadata(role:role as? String,secure:(subrole as? String) == kAXSecureTextFieldSubrole)
}
func postPaste(to pid:pid_t) -> Bool {
    guard CGPreflightPostEventAccess(),let source=CGEventSource(stateID:.privateState),
          let down=CGEvent(keyboardEventSource:source,virtualKey:CGKeyCode(kVK_ANSI_V),keyDown:true),
          let up=CGEvent(keyboardEventSource:source,virtualKey:CGKeyCode(kVK_ANSI_V),keyDown:false) else { return false }
    down.flags = .maskCommand; up.flags = .maskCommand
    down.postToPid(pid); up.postToPid(pid)
    return true
}
