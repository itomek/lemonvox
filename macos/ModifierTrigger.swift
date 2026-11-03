import AppKit
import ApplicationServices

@MainActor struct ModifierCommandSides {
    private enum State { case unknown, up, down }
    private var left = State.unknown
    private var right = State.unknown

    var rightOnlyHeld: Bool { right == .down && left == .up }

    mutating func reset(commandHeld: Bool) {
        let baseline: State = commandHeld ? .unknown : .up
        left = baseline
        right = baseline
    }

    mutating func observe(_ event: NSEvent) {
        guard event.modifierFlags.contains(.command) else {
            reset(commandHeld: false)
            return
        }
        let sideMask: UInt = 0x18
        let raw = event.modifierFlags.rawValue
        if raw & sideMask != 0 {
            left = raw & 0x8 != 0 ? .down : .up
            right = raw & 0x10 != 0 ? .down : .up
            return
        }
        guard event.type == .flagsChanged else { return }
        switch event.keyCode {
        case 54:
            if right == .up { right = .down }
            else if right == .down { right = .up }
        case 55:
            if left == .up { left = .down }
            else if left == .down { left = .up }
        default:
            return
        }
        if left == .up && right == .up {
            // Aggregate Command remains set, so an unobserved side may be held.
            reset(commandHeld: true)
        }
    }
}

/// Global observation is passive. A recognized cancellation is consumed locally.
@MainActor final class ModifierTrigger {
    private static let sideBits:[UInt16:UInt]=[55:0x8,54:0x10,58:0x20,61:0x40,59:0x1,62:0x2000,56:0x2,60:0x4]
    static func canObserveDefaultCancellation(_ choice: TriggerChoice, sessionActive: Bool) -> Bool {
        sessionActive && choice == .defaultMacCancellation
    }
    private var monitor:Any?
    var isMonitoring:Bool { monitor != nil }
    private var activation:Any?
    private var localMonitor:Any?
    private var commandSides = ModifierCommandSides()
    private let canCancel:() -> Bool
    private let cancel:() -> Void
    private var detector:DoubleTapDetector
    let allowed:() -> Bool
    let fire:() -> Void
    init(key:UInt16,allowed:@escaping () -> Bool,fire:@escaping () -> Void,
         canCancel:@escaping () -> Bool = { false }, cancel:@escaping () -> Void = {}) {
        self.canCancel=canCancel; self.cancel=cancel
        detector=DoubleTapDetector(key:key); self.allowed=allowed; self.fire=fire
    }
    func start() -> Bool {
        guard AXIsProcessTrusted() else { return false }
        reset()
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
    func reset() {
        detector.reset()
        commandSides.reset(commandHeld: NSEvent.modifierFlags.contains(.command))
    }
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
        commandSides.observe(event)
        guard canCancel(), event.type == .keyDown, event.keyCode == 53,
              !event.isARepeat else { return false }
        let flags=event.modifierFlags.intersection([.command,.option,.control,.shift,.capsLock,.function])
        guard flags == .command, commandSides.rightOnlyHeld else { return false }
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
