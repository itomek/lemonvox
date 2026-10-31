import AppKit
import Sparkle

@MainActor final class UpdateCoordinator:NSObject,SPUUpdaterDelegate,@preconcurrency SPUStandardUserDriverDelegate {
 let safety:() -> UpdateSafety
 let changed:() -> Void
 private let performCheck:(() -> Void)?
 private lazy var controller=SPUStandardUpdaterController(startingUpdater:false,updaterDelegate:self,userDriverDelegate:self)
 private var gate=UpdateInstallGate()
 private var timer:Timer?
 private var queuedCheck=false
 private(set) var updateAvailable=false
 private var activity=UpdateActivity()
 var blocksNewDictation:Bool { !activity.canStartDictation }
 var supportsGentleScheduledUpdateReminders:Bool { true }
 init(safety:@escaping () -> UpdateSafety,changed:@escaping () -> Void,performCheck:(() -> Void)?=nil) {
  self.safety=safety;self.changed=changed;self.performCheck=performCheck;super.init()
 }
 var automaticChecks:Bool {
  get { controller.updater.automaticallyChecksForUpdates }
  set { controller.updater.automaticallyChecksForUpdates=newValue }
 }
 var automaticInstallation:Bool {
  get { controller.updater.automaticallyDownloadsUpdates }
  set { controller.updater.automaticallyDownloadsUpdates=newValue }
 }
 func start() { controller.startUpdater() }
 private func runCheck() { if let performCheck { performCheck() } else { controller.checkForUpdates(nil) } }
 func check() {
  guard safety().canInstall else { queuedCheck=true;watch();changed();return }
  activity.manualCheckActive=true;runCheck()
 }
 var menuTitle:String { queuedCheck ? "Check for Updates… (waiting for current task)" : (updateAvailable ? "Update Available…" : "Check for Updates…") }
 private func watch() {
  guard timer == nil else { return }
  timer=Timer.scheduledTimer(withTimeInterval:0.5,repeats:true) { [weak self] _ in
   MainActor.assumeIsolated { self?.resume() }
  }
 }
 private func resume() {
  guard safety().canInstall else { return }
  timer?.invalidate();timer=nil
  gate.resumeIfSafe(safety())
  if queuedCheck { queuedCheck=false;changed();activity.manualCheckActive=true;runCheck() }
 }
 func updater(_ updater:SPUUpdater,mayPerform updateCheck:SPUUpdateCheck) throws {
  guard safety().canInstall else { throw NSError(domain:"LemonVox.Update",code:1,userInfo:[NSLocalizedDescriptionKey:"Updates will be available after the current dictation or Settings session finishes."]) }
 }
 func standardUserDriverShouldHandleShowingScheduledUpdate(_ update:SUAppcastItem,andInImmediateFocus immediateFocus:Bool) -> Bool { safety().canInstall }
 func standardUserDriverWillHandleShowingUpdate(_ handleShowingUpdate:Bool,forUpdate update:SUAppcastItem,state:SPUUserUpdateState) {
  updateAvailable=true;changed()
  if !handleShowingUpdate { queuedCheck=true;watch() }
 }
 func standardUserDriverDidReceiveUserAttention(forUpdate update:SUAppcastItem) { updateAvailable=false;changed() }
 func standardUserDriverWillFinishUpdateSession() {
  activity=UpdateActivity(installOnQuitPending:activity.installOnQuitPending);queuedCheck=false;gate.cancel();timer?.invalidate();timer=nil;changed()
 }
 func updater(_ updater:SPUUpdater,willInstallUpdate item:SUAppcastItem) { activity.installationCommitted=true }
 func updater(_ updater:SPUUpdater,shouldPostponeRelaunchForUpdate item:SUAppcastItem,untilInvokingBlock installHandler:@escaping () -> Void) -> Bool {
  guard !safety().canInstall else { activity.installationCommitted=true;return false }
  activity.installationCommitted=false
  gate.deferInstallation { [weak self] in self?.activity.installationCommitted=true;installHandler() }
  watch();return true
 }
 // This is checked at final termination too: Sparkle does not call the
 // postpone-relaunch delegate on every installation path.
 func updater(_ updater:SPUUpdater,willInstallUpdateOnQuit item:SUAppcastItem,immediateInstallationBlock immediateInstallHandler:@escaping () -> Void) -> Bool {
  activity.installOnQuitPending=true
  return false
 }
 var canTerminate:Bool { activity.canTerminate(safety()) }
}
