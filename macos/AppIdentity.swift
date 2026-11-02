import Foundation

struct AppIdentity {
    let info: [String: Any]
    let path: String
    static var running: AppIdentity { AppIdentity(info: Bundle.main.infoDictionary ?? [:], path: Bundle.main.bundleURL.path) }
    private func value(_ key: String, fallback: String) -> String {
        guard let value = info[key] as? String, !value.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return fallback }
        return value.trimmingCharacters(in: .whitespacesAndNewlines)
    }
    var name: String { value("CFBundleDisplayName", fallback: value("CFBundleName", fallback: "LemonVox")) }
    var isDebug: Bool { value("CFBundleIdentifier", fallback: "") == "com.itomek.lemonvox.debug" }
    var version: String { "\(name) \(value("CFBundleShortVersionString", fallback: "unavailable")) (build \(value("CFBundleVersion", fallback: "unavailable")))" }
    func accessibilityGuidance(trusted: Bool, canPost: Bool) -> String {
        let facts = "Accessibility: \(trusted ? "available" : "unavailable"); event posting: \(canPost ? "available" : "unavailable")."
        guard !trusted || !canPost else { return facts }
        return facts + "\n\n1. In Accessibility settings, enable \(name). If it is not listed, use + and select:\n\(path)\n2. Return here and choose Check Again.\n3. If the entry is enabled but access is still unavailable, stop recording and copy any pending text before quitting and reopening \(name)."
    }
}
