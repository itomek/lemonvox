import Foundation

struct WorkflowLaunchTracker {
    private(set) var activeOperation: UInt64?
    private(set) var phase = LV_WORKFLOW_PHASE_IDLE
    private var epoch: UInt64 = 0
    private var startingEpoch: UInt64?
    private var startingOperation: UInt64?
    private var terminalBeforeStartReturn = false

    mutating func begin() -> UInt64 {
        epoch &+= 1
        startingEpoch = epoch
        startingOperation = nil
        terminalBeforeStartReturn = false
        activeOperation = nil
        phase = LV_WORKFLOW_PHASE_IDLE
        return epoch
    }

    func acceptsSynchronousCallback(operation: UInt64) -> Bool {
        guard operation != 0 else { return false }
        if activeOperation == operation { return true }
        return startingEpoch != nil && (startingOperation == nil || startingOperation == operation)
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
        guard startingEpoch == epoch else { return false }
        startingEpoch = nil
        defer {
            startingOperation = nil
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
        terminalBeforeStartReturn = false
        activeOperation = nil
        phase = LV_WORKFLOW_PHASE_IDLE
    }
}
