import Foundation
@main struct AppIdentityTests {
    static func main() {
        let debug = AppIdentity(info:["CFBundleName":"Other", "CFBundleDisplayName":"LemonVox Debug", "CFBundleIdentifier":"com.itomek.lemonvox.debug", "CFBundleVersion":"42"], path:"/tmp/LemonVox Debug.app")
        precondition(debug.name == "LemonVox Debug" && debug.isDebug)
        precondition(debug.version.contains("LemonVox Debug") && debug.version.contains("42"))
        let missing = debug.accessibilityGuidance(trusted:false, canPost:false)
        precondition(missing.contains("LemonVox Debug") && missing.contains(debug.path) && missing.contains("Check Again") && missing.contains("+"))
        precondition(missing.contains("\n") && missing.contains("pending text") && missing.contains("recording"))
        precondition(!debug.accessibilityGuidance(trusted:true, canPost:true).contains("reopen"))
        let pasteMissing = debug.accessibilityGuidance(trusted:true, canPost:false)
        precondition(pasteMissing.contains("Accessibility: available") && pasteMissing.contains("event posting: unavailable") && pasteMissing.contains("Check Again"))
        let targetingMissing = debug.accessibilityGuidance(trusted:false, canPost:true)
        precondition(targetingMissing.contains("Accessibility: unavailable") && targetingMissing.contains("event posting: available") && targetingMissing.contains(debug.path))
        precondition(!AppIdentity(info:["CFBundleIdentifier":"com.itomek.lemonvox.prototype"], path:"/tmp/LemonVox.app").isDebug)
        print("PASS: bundle identity and conditional Accessibility guidance")
    }
}
