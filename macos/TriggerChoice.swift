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
    private var pressed: (key:UInt16,time:TimeInterval)?
    private var first: (key:UInt16,time:TimeInterval)?
    init(key:UInt16) { self.key=key }
    mutating func reset() { pressed=nil; first=nil }
    mutating func event(key candidate:UInt16, down:Bool, time:TimeInterval) -> Bool {
        guard candidate == key || (key == 0 && [54,55].contains(candidate)) else { reset(); return false }
        if down {
            guard pressed == nil else { reset(); return false }
            if let first, first.key != candidate || time-first.time > 0.35 || time < first.time { self.first=nil }
            pressed=(candidate,time); return false
        }
        guard let press=pressed,press.key == candidate,time >= press.time,time-press.time <= 0.25 else { reset(); return false }
        pressed=nil
        if let first,first.key == candidate,time-first.time <= 0.60 {
            reset(); return true
        }
        first=(candidate,time); return false
    }
}

enum TriggerError: LocalizedError {
    case unavailable
    var errorDescription:String? { "Shortcut unavailable; previous trigger retained. Check Accessibility or choose another combination." }
}
