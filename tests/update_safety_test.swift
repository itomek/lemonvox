import Foundation
@main struct UpdateSafetyTests {
 static func main() {
  precondition(UpdateSafety().canInstall)
  for state in [UpdateSafety(sessionBusy:true), UpdateSafety(pasteBusy:true), UpdateSafety(pendingText:true), UpdateSafety(settingsOpen:true)] { precondition(!state.canInstall) }
  var gate=UpdateInstallGate();var installs=0
  gate.deferInstallation { installs += 1 }
  gate.resumeIfSafe(UpdateSafety(sessionBusy:true));precondition(installs==0)
  gate.resumeIfSafe(UpdateSafety());gate.resumeIfSafe(UpdateSafety());precondition(installs==1)
  gate.deferInstallation { installs += 1 };gate.cancel();gate.resumeIfSafe(UpdateSafety());precondition(installs==1)
  var activity=UpdateActivity()
  precondition(activity.canStartDictation)
  activity.manualCheckActive=true;precondition(!activity.canStartDictation)
  activity.manualCheckActive=false;activity.installOnQuitPending=true
  precondition(activity.canStartDictation && !activity.canTerminate(UpdateSafety(sessionBusy:true)))
  precondition(activity.canTerminate(UpdateSafety()))
  activity.installationCommitted=true;precondition(!activity.canStartDictation)
  print("PASS: updater waits for busy work/pending text/settings; continuation resumes once; cancellation clears it")
 }
}
