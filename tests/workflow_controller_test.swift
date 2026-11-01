import Foundation

@main struct WorkflowControllerTests {
    static func main() {
        var tracker = WorkflowLaunchTracker()

        let recordingEpoch = tracker.begin()
        precondition(tracker.acceptsSynchronousCallback(operation: 41))
        precondition(tracker.acceptPhase(operation: 41, phase: LV_WORKFLOW_PHASE_PREPARING))
        precondition(tracker.phase == LV_WORKFLOW_PHASE_PREPARING)
        precondition(tracker.activeOperation == nil,
            "Preparing is delivered synchronously before start returns an operation ID")
        precondition(tracker.finishStart(operation: 41, epoch: recordingEpoch))
        precondition(tracker.activeOperation == 41)

        let invalidEpoch = tracker.begin()
        precondition(tracker.acceptPhase(operation: 42, phase: LV_WORKFLOW_PHASE_PREPARING))
        precondition(tracker.acceptPhase(operation: 42, phase: LV_WORKFLOW_PHASE_IDLE))
        precondition(tracker.acceptTerminal(operation: 42))
        precondition(!tracker.finishStart(operation: 42, epoch: invalidEpoch),
            "A synchronous terminal callback must prevent the returned nonzero ID from reviving the operation")
        precondition(tracker.activeOperation == nil && tracker.phase == LV_WORKFLOW_PHASE_IDLE)
        precondition(!tracker.acceptsSynchronousCallback(operation: 42))

        let staleEpoch = tracker.begin()
        precondition(tracker.acceptPhase(operation: 61, phase: LV_WORKFLOW_PHASE_PREPARING))
        precondition(!tracker.acceptPhase(operation: 62, phase: LV_WORKFLOW_PHASE_IDLE),
            "A new launch must reject a stale operation callback before start returns")
        precondition(!tracker.finishStart(operation: 62, epoch: staleEpoch),
            "The returned operation must match the callback that established this launch")
        precondition(!tracker.acceptsSynchronousCallback(operation: 61))

        let zeroEpoch = tracker.begin()
        precondition(!tracker.finishStart(operation: 0, epoch: zeroEpoch))
        precondition(!tracker.acceptsSynchronousCallback(operation: 73),
            "A zero start result must close the launch and reject later callbacks")

        print("PASS: launch tracker handles synchronous preparing and terminal callbacks")
    }
}
