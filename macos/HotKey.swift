import Carbon

enum HotKeyEventID {
    static let signature: OSType = 0x4c565850
    static let dictation: UInt32 = 1
    static let cancellation: UInt32 = 2
}

final class HotKeyBinding {
    let eventID: UInt32
    private(set) var reference: EventHotKeyRef?
    private(set) var choice: Int?
    private var pressed = false
    private var combination: TriggerChoice?

    init(eventID: UInt32 = HotKeyEventID.dictation) {
        precondition(eventID != 0)
        self.eventID = eventID
    }

    func matches(_ identifier: EventHotKeyID) -> Bool {
        reference != nil && identifier.signature == HotKeyEventID.signature && identifier.id == eventID
    }

    func replace(with value: Int) -> OSStatus {
        let result=replace(key:UInt16(value == 0 ? kVK_ANSI_V : kVK_Space),modifiers:UInt32(value == 0 ? cmdKey | optionKey : cmdKey | shiftKey))
        if result == noErr { choice=value }
        return result
    }
    func replace(key:UInt16,modifiers:UInt32) -> OSStatus {
        let next=TriggerChoice.combo(key:key,modifiers:modifiers)
        if combination == next, reference != nil { return noErr }
        var candidate: EventHotKeyRef?
        let result = RegisterEventHotKey(UInt32(key), modifiers,
            EventHotKeyID(signature: HotKeyEventID.signature, id: eventID), GetApplicationEventTarget(), 0, &candidate)
        guard result == noErr, let candidate else { return result == noErr ? OSStatus(paramErr) : result }
        if let reference { UnregisterEventHotKey(reference) }
        reference = candidate; combination=next; pressed = false
        return noErr
    }
    func press() -> Bool {
        guard !pressed else { return false }
        pressed = true
        return true
    }
    func release() { pressed = false }
    func clear() {
        if let reference { UnregisterEventHotKey(reference) }
        reference = nil; choice = nil; combination=nil; pressed = false
    }
    deinit { clear() }
}
