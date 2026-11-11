import AppKit
import ApplicationServices
import Carbon

@MainActor final class SystemClipboard: ClipboardAccess {
    private let board:NSPasteboard
    init(_ board:NSPasteboard = .general) { self.board=board }
    func setText(_ text: String) throws {
        board.clearContents()
        guard board.setString(text, forType: .string) else { throw DictationError.response }
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
func pasteModifiersReleased() -> Bool {
    CGEventSource.flagsState(.combinedSessionState).intersection([.maskCommand, .maskControl, .maskAlternate, .maskShift]).isEmpty
}
func postPasteAtCurrentFocus() -> Bool {
    guard CGPreflightPostEventAccess(),let source=CGEventSource(stateID:.privateState),
          let down=CGEvent(keyboardEventSource:source,virtualKey:CGKeyCode(kVK_ANSI_V),keyDown:true),
          let up=CGEvent(keyboardEventSource:source,virtualKey:CGKeyCode(kVK_ANSI_V),keyDown:false) else { return false }
    down.flags = .maskCommand; up.flags = .maskCommand
    down.post(tap: .cghidEventTap); up.post(tap: .cghidEventTap)
    return true
}
