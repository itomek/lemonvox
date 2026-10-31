import AppKit
@main struct UpdateCoordinatorTests {
 @MainActor static func main() {
  var state=UpdateSafety(sessionBusy:true);var checks=0
  let coordinator=UpdateCoordinator(safety:{ state },changed:{},performCheck:{ checks += 1 })
  coordinator.check();coordinator.check();precondition(checks==0)
  precondition(coordinator.menuTitle.contains("waiting"))
  state=UpdateSafety();RunLoop.main.run(until:Date().addingTimeInterval(0.7))
  precondition(checks==1 && coordinator.blocksNewDictation)
  coordinator.standardUserDriverWillFinishUpdateSession()
  precondition(!coordinator.blocksNewDictation)
  state=UpdateSafety(pendingText:true);coordinator.check();precondition(checks==1)
  coordinator.standardUserDriverWillFinishUpdateSession()
  state=UpdateSafety();RunLoop.main.run(until:Date().addingTimeInterval(0.7))
  precondition(checks==1,"Canceled queued check must not resume")
  print("PASS: real update coordinator queues busy checks once, holds new dictation during manual UI and clears canceled requests")
 }
}
