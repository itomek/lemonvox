import Foundation

enum TargetPreflightStatus: String {
    case idle
    case initialTarget
    case warming
    case warmedTarget
    case targetChanged
    case abandoned
    case startedWithTarget
    case startedWithoutTarget
    case canceled
}

// Fixed, content-free facts for explicit local debugging. Capture records a
// historical fact; it does not claim the target remains focused or editable.
struct TargetPreflightState {
    private(set) var warmupAttempts = 0
    private(set) var originalCaptured = false
    private(set) var status: TargetPreflightStatus = .idle

    mutating func begin(initialTargetCaptured: Bool) {
        warmupAttempts = 0
        originalCaptured = initialTargetCaptured
        status = initialTargetCaptured ? .initialTarget : .warming
    }

    mutating func warmupAttempt() {
        guard status == .warming else { return }
        warmupAttempts += 1
    }

    mutating func captureWarmedTarget() {
        guard status == .warming else { return }
        originalCaptured = true
        status = .warmedTarget
    }

    mutating func targetChanged() {
        status = .targetChanged
    }

    mutating func abandon() {
        status = .abandoned
    }

    mutating func handoff() {
        guard status == .initialTarget || status == .warmedTarget || status == .warming else { return }
        status = originalCaptured ? .startedWithTarget : .startedWithoutTarget
    }

    mutating func cancel() {
        status = .canceled
    }
}

struct WorkflowLaunchTracker {
    private(set) var activeOperation: UInt64?
    private(set) var phase = LV_WORKFLOW_PHASE_IDLE
    private var epoch: UInt64 = 0
    private var startingEpoch: UInt64?
    private var startingOperation: UInt64?
    private var startArmed = false
    private var terminalBeforeStartReturn = false

    mutating func begin() -> UInt64 {
        epoch &+= 1
        startingEpoch = epoch
        startingOperation = nil
        startArmed = false
        terminalBeforeStartReturn = false
        activeOperation = nil
        phase = LV_WORKFLOW_PHASE_IDLE
        return epoch
    }

    // AX target warmup may happen before WorkflowBridge.start(). Do not let a
    // callback from an older operation claim this epoch until that exact call
    // is about to begin.
    mutating func armStart(epoch: UInt64) -> Bool {
        guard startingEpoch == epoch, !startArmed, activeOperation == nil else { return false }
        startArmed = true
        return true
    }

    func acceptsSynchronousCallback(operation: UInt64) -> Bool {
        guard operation != 0 else { return false }
        if activeOperation == operation { return true }
        return startArmed && startingEpoch != nil && (startingOperation == nil || startingOperation == operation)
    }

    mutating func acceptPhase(operation: UInt64, phase: LVWorkflowPhase) -> Bool {
        guard acceptsSynchronousCallback(operation: operation) else { return false }
        if startingEpoch != nil { startingOperation = operation }
        self.phase = phase
        return true
    }

    mutating func acceptTerminal(operation: UInt64) -> Bool {
        guard acceptsSynchronousCallback(operation: operation) else { return false }
        if startingEpoch != nil { startingOperation = operation }
        terminalBeforeStartReturn = startingEpoch != nil && activeOperation == nil
        activeOperation = nil
        phase = LV_WORKFLOW_PHASE_IDLE
        return true
    }

    mutating func finishStart(operation: UInt64, epoch: UInt64) -> Bool {
        guard startingEpoch == epoch, startArmed else { return false }
        startingEpoch = nil
        defer {
            startingOperation = nil
            startArmed = false
            terminalBeforeStartReturn = false
        }
        guard operation != 0, startingOperation == nil || startingOperation == operation,
            !terminalBeforeStartReturn else { return false }
        activeOperation = operation
        return true
    }

    mutating func cancel() {
        epoch &+= 1
        startingEpoch = nil
        startingOperation = nil
        startArmed = false
        terminalBeforeStartReturn = false
        activeOperation = nil
        phase = LV_WORKFLOW_PHASE_IDLE
    }
}
