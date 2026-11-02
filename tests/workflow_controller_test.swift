import Foundation

@main struct WorkflowControllerTests {
    static func main() {
        var tracker = WorkflowLaunchTracker()

        let preflightEpoch = tracker.begin()
        precondition(!tracker.acceptPhase(operation: 99, phase: LV_WORKFLOW_PHASE_PREPARING),
            "A callback that arrives before workflow.start begins must not claim a preflight epoch")
        precondition(!tracker.acceptTerminal(operation: 99),
            "A terminal callback before workflow.start begins must not cancel a preflight epoch")
        precondition(tracker.armStart(epoch: preflightEpoch))
        precondition(tracker.acceptPhase(operation: 100, phase: LV_WORKFLOW_PHASE_PREPARING))
        precondition(tracker.finishStart(operation: 100, epoch: preflightEpoch))
        tracker.cancel()

        let canceledEpoch = tracker.begin()
        tracker.cancel()
        precondition(!tracker.armStart(epoch: canceledEpoch),
            "A canceled preflight epoch must not be armed later")
        precondition(!tracker.acceptPhase(operation: 101, phase: LV_WORKFLOW_PHASE_PREPARING),
            "A callback after cancellation must not claim a later start")
        precondition(!tracker.acceptTerminal(operation: 101),
            "A terminal callback after cancellation must not claim a later start")

        let doubleArmEpoch = tracker.begin()
        precondition(tracker.armStart(epoch: doubleArmEpoch))
        precondition(!tracker.armStart(epoch: doubleArmEpoch),
            "Only the one imminent workflow.start call may be armed")
        precondition(tracker.acceptPhase(operation: 102, phase: LV_WORKFLOW_PHASE_PREPARING))
        precondition(tracker.finishStart(operation: 102, epoch: doubleArmEpoch))
        tracker.cancel()

        let recordingEpoch = tracker.begin()
        precondition(tracker.armStart(epoch: recordingEpoch))
        precondition(tracker.acceptsSynchronousCallback(operation: 41))
        precondition(tracker.acceptPhase(operation: 41, phase: LV_WORKFLOW_PHASE_PREPARING))
        precondition(tracker.phase == LV_WORKFLOW_PHASE_PREPARING)
        precondition(tracker.activeOperation == nil,
            "Preparing is delivered synchronously before start returns an operation ID")
        precondition(tracker.finishStart(operation: 41, epoch: recordingEpoch))
        precondition(tracker.activeOperation == 41)

        let invalidEpoch = tracker.begin()
        precondition(tracker.armStart(epoch: invalidEpoch))
        // Preserve the core's synchronous invalid-config sequence: idle then terminal.
        precondition(tracker.acceptPhase(operation: 42, phase: LV_WORKFLOW_PHASE_PREPARING))
        precondition(tracker.acceptPhase(operation: 42, phase: LV_WORKFLOW_PHASE_IDLE))
        precondition(tracker.acceptTerminal(operation: 42))
        precondition(!tracker.finishStart(operation: 42, epoch: invalidEpoch),
            "A synchronous terminal callback must prevent the returned nonzero ID from reviving the operation")
        precondition(tracker.activeOperation == nil && tracker.phase == LV_WORKFLOW_PHASE_IDLE)
        precondition(!tracker.acceptsSynchronousCallback(operation: 42))

        let staleEpoch = tracker.begin()
        precondition(tracker.armStart(epoch: staleEpoch))
        precondition(tracker.acceptPhase(operation: 61, phase: LV_WORKFLOW_PHASE_PREPARING))
        precondition(!tracker.acceptPhase(operation: 62, phase: LV_WORKFLOW_PHASE_IDLE),
            "A new launch must reject a stale operation callback before start returns")
        precondition(!tracker.finishStart(operation: 62, epoch: staleEpoch),
            "The returned operation must match the callback that established this launch")
        precondition(!tracker.acceptsSynchronousCallback(operation: 61))

        let zeroEpoch = tracker.begin()
        precondition(tracker.armStart(epoch: zeroEpoch))
        precondition(!tracker.finishStart(operation: 0, epoch: zeroEpoch))
        precondition(!tracker.acceptsSynchronousCallback(operation: 73),
            "A zero start result must close the launch and reject later callbacks")

        var preflight = TargetPreflightState()
        preflight.begin(initialTargetCaptured: false)
        precondition(preflight.status == .warming && !preflight.originalCaptured)
        preflight.warmupAttempt()
        preflight.warmupAttempt()
        precondition(preflight.warmupAttempts == 2)
        preflight.captureWarmedTarget()
        precondition(preflight.status == .warmedTarget && preflight.originalCaptured)
        preflight.handoff()
        precondition(preflight.status == .startedWithTarget)

        preflight.begin(initialTargetCaptured: true)
        precondition(preflight.status == .initialTarget && preflight.originalCaptured)
        preflight.targetChanged()
        precondition(preflight.status == .targetChanged)
        preflight.begin(initialTargetCaptured: false)
        preflight.warmupAttempt()
        preflight.cancel()
        precondition(preflight.status == .canceled && preflight.warmupAttempts == 1)

        print("PASS: launch tracker rejects preflight stale callbacks and handles synchronous starts")
    }
}
