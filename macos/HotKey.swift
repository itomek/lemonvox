import Carbon

final class HotKeyBinding {
    private(set) var reference: EventHotKeyRef?
    private(set) var choice: Int?
    private var pressed = false
    private var combination: TriggerChoice?
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
            EventHotKeyID(signature: 0x4c565850, id: 1), GetApplicationEventTarget(), 0, &candidate)
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
