import AppKit
import ApplicationServices

/// Global observation is passive. A recognized cancellation is consumed locally.
@MainActor final class ModifierTrigger {
    private static let sideBits:[UInt16:UInt]=[55:0x8,54:0x10,58:0x20,61:0x40,59:0x1,62:0x2000,56:0x2,60:0x4]
    private var monitor:Any?
    var isMonitoring:Bool { monitor != nil }
    private var activation:Any?
    private var localMonitor:Any?
    private let key:UInt16
    private let canCancel:() -> Bool
    private let cancel:() -> Void
    private var pressed:(key:UInt16,time:TimeInterval)?
    private var released:TimeInterval?
    private var detector:DoubleTapDetector
    let allowed:() -> Bool
    let fire:() -> Void
    init(key:UInt16,allowed:@escaping () -> Bool,fire:@escaping () -> Void,
         canCancel:@escaping () -> Bool = { false }, cancel:@escaping () -> Void = {}) {
        self.key=key; self.canCancel=canCancel; self.cancel=cancel
        detector=DoubleTapDetector(key:key); self.allowed=allowed; self.fire=fire
    }
    func start() -> Bool {
        guard AXIsProcessTrusted() else { return false }
        monitor=NSEvent.addGlobalMonitorForEvents(matching:[.flagsChanged,.keyDown,.leftMouseDown,.rightMouseDown,.otherMouseDown]) { [weak self] event in
            MainActor.assumeIsolated { self?.receive(event) }
        }
        localMonitor=NSEvent.addLocalMonitorForEvents(matching:[.flagsChanged,.keyDown,.leftMouseDown,.rightMouseDown,.otherMouseDown]) { [weak self] event in
            let consumed = MainActor.assumeIsolated { self?.observeCancellation(event) == true }
            return consumed ? nil : event
        }
        activation=NSWorkspace.shared.notificationCenter.addObserver(forName:NSWorkspace.didActivateApplicationNotification,object:nil,queue:.main) { [weak self] _ in
            MainActor.assumeIsolated { self?.reset() }
        }
        return monitor != nil
    }
    func reset() { detector.reset(); pressed=nil; released=nil }
    func receive(_ event:NSEvent) {
        if observeCancellation(event) { return }
        guard allowed() else { detector.reset(); return }
        guard event.type == .flagsChanged else { detector.intervening(time:event.timestamp); return }
        let key=event.keyCode
        let mask:NSEvent.ModifierFlags
        switch key {
        case 54,55: mask = .command
        case 58,61: mask = .option
        case 59,62: mask = .control
        case 56,60: mask = .shift
        default: detector.intervening(time:event.timestamp); return
        }
        let flags=event.modifierFlags.intersection([.command,.option,.control,.shift,.capsLock,.function])
        guard flags.subtracting(mask).isEmpty else { detector.intervening(time:event.timestamp); return }
        // Device-specific flag bits disambiguate release when the opposite side is held.
        let down=(event.modifierFlags.rawValue & (Self.sideBits[key] ?? 0)) != 0
        if detector.event(key:key,down:down,time:event.timestamp) { fire() }
    }
    private func observeCancellation(_ event:NSEvent) -> Bool {
        guard canCancel() else { pressed=nil; released=nil; return false }
        let flags=event.modifierFlags.intersection([.command,.option,.control,.shift,.capsLock,.function])
        let mask:NSEvent.ModifierFlags
        switch key {
        case 0,54,55: mask = .command
        case 58,61: mask = .option
        case 59,62: mask = .control
        case 56,60: mask = .shift
        default: return false
        }
        if event.type == .flagsChanged,
           event.keyCode == key || (key == 0 && [54,55].contains(event.keyCode)) {
            if flags == mask && event.modifierFlags.rawValue & (Self.sideBits[event.keyCode] ?? 0) != 0 {
                pressed=(event.keyCode,event.timestamp)
                released=nil
            } else {
                if flags.isEmpty, let pressed, pressed.key == event.keyCode,
                   (0...0.25).contains(event.timestamp-pressed.time) {
                    released=event.timestamp
                } else { released=nil }
                pressed=nil
            }
            return false
        }
        let tapped = released.map { (0...0.35).contains(event.timestamp-$0) } ?? false
        pressed=nil; released=nil
        let held = flags == mask && (key == 0 || event.modifierFlags.rawValue & (Self.sideBits[key] ?? 0) != 0)
        guard event.type == .keyDown, event.keyCode == 53, !event.isARepeat,
              held || (flags.isEmpty && tapped) else { return false }
        reset()
        cancel()
        return true
    }
    func stop() {
        if let localMonitor { NSEvent.removeMonitor(localMonitor) }; localMonitor=nil
        if let monitor { NSEvent.removeMonitor(monitor) }; monitor=nil
        if let activation { NSWorkspace.shared.notificationCenter.removeObserver(activation) }; activation=nil
        reset()
    }
}
