import Foundation

enum TriggerChoice: Codable, Equatable {
    case combo(key: UInt16, modifiers: UInt32)
    case doubleTap(key: UInt16)
    static let defaultMac = TriggerChoice.doubleTap(key: 0) // Either Command, same side per pair.
    static func migrate(_ legacy: Int?) -> TriggerChoice {
        legacy == 1 ? .combo(key:49,modifiers:768) : .defaultMac
    }
}

struct DoubleTapDetector {
    let key: UInt16
    private var core: LVDoubleTapDetector
    init(key:UInt16) {
        self.key=key
        core=LVDoubleTapDetector()
        let target=Self.normalizedTarget(key)
        lv_double_tap_init(&core,target.key,target.side)
    }
    mutating func reset() { lv_double_tap_cancel(&core) }
    mutating func intervening(time:TimeInterval) {
        _=lv_double_tap_event(&core,LVNormalizedKeyEvent(key:LV_KEY_UNKNOWN,side:LV_KEY_SIDE_NONE,action:LV_KEY_INTERVENING,timestamp:time))
    }
    mutating func event(key candidate:UInt16, down:Bool, time:TimeInterval) -> Bool {
        guard let normalized=Self.normalized(candidate) else { intervening(time:time); return false }
        let action:LVKeyAction=down ? LV_KEY_PRESS : LV_KEY_RELEASE
        return lv_double_tap_event(&core,LVNormalizedKeyEvent(key:normalized.key,side:normalized.side,action:action,timestamp:time)) != 0
    }
    private static func normalizedTarget(_ key:UInt16) -> (key:LVKeyIdentity,side:LVKeySide) {
        key == 0 ? (LV_KEY_COMMAND,LV_KEY_SIDE_ANY) : (normalized(key) ?? (LV_KEY_UNKNOWN,LV_KEY_SIDE_NONE))
    }
    private static func normalized(_ key:UInt16) -> (key:LVKeyIdentity,side:LVKeySide)? {
        switch key {
        case 54: return (LV_KEY_COMMAND,LV_KEY_SIDE_RIGHT)
        case 55: return (LV_KEY_COMMAND,LV_KEY_SIDE_LEFT)
        case 58: return (LV_KEY_OPTION,LV_KEY_SIDE_LEFT)
        case 61: return (LV_KEY_OPTION,LV_KEY_SIDE_RIGHT)
        case 59: return (LV_KEY_CONTROL,LV_KEY_SIDE_LEFT)
        case 62: return (LV_KEY_CONTROL,LV_KEY_SIDE_RIGHT)
        case 56: return (LV_KEY_SHIFT,LV_KEY_SIDE_LEFT)
        case 60: return (LV_KEY_SHIFT,LV_KEY_SIDE_RIGHT)
        default: return nil
        }
    }
}

enum TriggerError: LocalizedError {
    case unavailable
    var errorDescription:String? { "Shortcut unavailable; previous trigger retained. Check Accessibility or choose another combination." }
}
