import AppKit
import Carbon
@main struct HotKeyTests {
    static func main() {
        NSApplication.shared.setActivationPolicy(.prohibited)
        let dictation = HotKeyBinding()
        let cancellation = HotKeyBinding(eventID: HotKeyEventID.cancellation)
        precondition(dictation.eventID == HotKeyEventID.dictation)
        precondition(cancellation.eventID == HotKeyEventID.cancellation)
        precondition(dictation.replace(with: 0) == noErr, "Default shortcut busy; test requires free Command-Option-V")
        let original = dictation.reference
        precondition(dictation.replace(with: 0) == noErr && dictation.reference == original)
        precondition(dictation.press() && !dictation.press())
        dictation.release()
        precondition(dictation.press())
        precondition(dictation.matches(EventHotKeyID(signature: HotKeyEventID.signature,
            id: HotKeyEventID.dictation)))
        precondition(!dictation.matches(EventHotKeyID(signature: HotKeyEventID.signature,
            id: HotKeyEventID.cancellation)))
        let defaultCancellation = HotKeyBinding(eventID: HotKeyEventID.cancellation)
        precondition(defaultCancellation.replace(key: 53, modifiers: UInt32(cmdKey)) == noErr,
            "Default Command-Escape cancellation shortcut busy")
        precondition(defaultCancellation.matches(EventHotKeyID(signature: HotKeyEventID.signature,
            id: HotKeyEventID.cancellation)))
        defaultCancellation.clear()
        precondition(cancellation.replace(key: UInt16(kVK_ANSI_B),
            modifiers: UInt32(cmdKey | optionKey | controlKey)) == noErr,
            "Cancellation test shortcut busy")
        let cancellationOriginal = cancellation.reference
        precondition(cancellation.matches(EventHotKeyID(signature: HotKeyEventID.signature,
            id: HotKeyEventID.cancellation)))
        var competitor: EventHotKeyRef?
        precondition(RegisterEventHotKey(UInt32(kVK_Space), UInt32(cmdKey | shiftKey),
            EventHotKeyID(signature: 0x54455354, id: 9), GetApplicationEventTarget(), 0, &competitor) == noErr,
            "Alternative shortcut busy; test requires free Command-Shift-Space")
        precondition(cancellation.replace(with: 1) != noErr)
        precondition(cancellation.reference == cancellationOriginal)
        if let competitor { UnregisterEventHotKey(competitor) }
        precondition(cancellation.replace(with: 1) == noErr && cancellation.choice == 1)
        dictation.clear(); cancellation.clear()
        precondition(dictation.reference == nil && dictation.choice == nil)
        precondition(cancellation.reference == nil && cancellation.choice == nil)
        print("PASS: distinct dictation/cancellation IDs preserve bindings, collisions, repeat filtering and teardown")
    }
}
