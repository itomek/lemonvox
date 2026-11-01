import Foundation
struct UpdateSafety {
 var sessionBusy=false
 var pasteBusy=false
 var pendingText=false
 var settingsOpen=false
 var canInstall:Bool { !sessionBusy && !pasteBusy && !pendingText && !settingsOpen }
}
struct UpdateInstallGate {
 private var continuation:(() -> Void)?
 mutating func deferInstallation(_ action:@escaping () -> Void) { continuation=action }
 mutating func resumeIfSafe(_ safety:UpdateSafety) {
  guard safety.canInstall, let action=continuation else { return }
  continuation=nil;action()
 }
 mutating func cancel() { continuation=nil }
}

struct UpdateActivity {
 var manualCheckActive=false
 var installOnQuitPending=false
 var installationCommitted=false
 var canStartDictation:Bool { !manualCheckActive && !installationCommitted }
 func canTerminate(_ safety:UpdateSafety) -> Bool { (!installationCommitted && !installOnQuitPending) || safety.canInstall }
}
