import AppKit
import ApplicationServices

/// Passive observation only. No character strings, event suppression or permission requests.
@MainActor final class ModifierTrigger {
    private var monitor:Any?
    private var activation:Any?
    private var detector:DoubleTapDetector
    let allowed:() -> Bool
    let fire:() -> Void
    init(key:UInt16,allowed:@escaping () -> Bool,fire:@escaping () -> Void) {
        detector=DoubleTapDetector(key:key); self.allowed=allowed; self.fire=fire
    }
    func start() -> Bool {
        guard AXIsProcessTrusted() else { return false }
        monitor=NSEvent.addGlobalMonitorForEvents(matching:[.flagsChanged,.keyDown,.leftMouseDown,.rightMouseDown,.otherMouseDown]) { [weak self] event in
            MainActor.assumeIsolated { self?.receive(event) }
        }
        activation=NSWorkspace.shared.notificationCenter.addObserver(forName:NSWorkspace.didActivateApplicationNotification,object:nil,queue:.main) { [weak self] _ in
            MainActor.assumeIsolated { self?.reset() }
        }
        return monitor != nil
    }
    func reset() { detector.reset() }
    func receive(_ event:NSEvent) {
        guard allowed(),event.type == .flagsChanged else { reset(); return }
        let key=event.keyCode
        let mask:NSEvent.ModifierFlags
        switch key {
        case 54,55: mask = .command
        case 58,61: mask = .option
        case 59,62: mask = .control
        case 56,60: mask = .shift
        default: reset(); return
        }
        let flags=event.modifierFlags.intersection([.command,.option,.control,.shift,.capsLock,.function])
        guard flags.subtracting(mask).isEmpty else { reset(); return }
        // Device-specific flag bits disambiguate release when the opposite side is held.
        let sideBits:[UInt16:UInt64]=[55:0x8,54:0x10,58:0x20,61:0x40,59:0x1,62:0x2000,56:0x2,60:0x4]
        let down=(event.modifierFlags.rawValue & UInt(sideBits[key] ?? 0)) != 0
        if detector.event(key:key,down:down,time:event.timestamp) { fire() }
    }
    func stop() {
        if let monitor { NSEvent.removeMonitor(monitor) }; monitor=nil
        if let activation { NSWorkspace.shared.notificationCenter.removeObserver(activation) }; activation=nil
        reset()
    }
}
