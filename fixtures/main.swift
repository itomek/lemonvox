import AppKit
import CoreGraphics

final class Fixture: NSObject, NSApplicationDelegate, NSTextFieldDelegate {
    var window: NSWindow!
    let first = NSTextField(string: "")
    let second = NSTextField(string: "")
    let secure = NSSecureTextField(string: "")
    let sample = "LemonVox prototype: send Alex the 14 invoices on Friday."
    let log = URL(fileURLWithPath: "/tmp/lemonvox-keyboard-fixture/state.json")
    func applicationDidFinishLaunching(_ note: Notification) {
        installFixtureMenus()
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 620, height: 300),
            styleMask: [.titled, .closable], backing: .buffered, defer: false)
        window.title = "LemonVox — keyboard test"
        window.isReleasedWhenClosed = false
        let description = NSTextField(wrappingLabelWithString:
            "Use Field A and Field B to check dictation, focus changes and keyboard copy/paste. Secure fields should reject insertion.")
        description.frame = NSRect(x: 22, y: 225, width: 576, height: 54)
        window.contentView?.addSubview(description)
        for (index, entry) in [first, second, secure].enumerated() {
            entry.placeholderString = index == 0 ? "Field A" : index == 1 ? "Field B" : "Secure field — insertion should be rejected"
            entry.frame = NSRect(x: 22, y: 174 - index * 44, width: 576, height: 30)
            entry.delegate = self
            window.contentView?.addSubview(entry)
        }
        let reset = NSButton(title: "Reset fields", target: self, action: #selector(resetFields))
        reset.frame = NSRect(x: 22, y: 32, width: 130, height: 32)
        window.contentView?.addSubview(reset)
        let change = NSButton(title: "Focus B after 0.35s", target: self, action: #selector(changeLater))
        change.frame = NSRect(x: 175, y: 32, width: 205, height: 32)
        window.contentView?.addSubview(change)
        if let data = try? Data(contentsOf: log),
           let saved = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] {
            if saved["firstEqualsSample"] as? Bool == true { first.stringValue = sample }
            if saved["secondEqualsSample"] as? Bool == true { second.stringValue = sample }
        }
        window.center(); window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        window.makeFirstResponder(first)
        report()
        if CommandLine.arguments.contains("--smoke-reopen") {
            precondition(first.placeholderString == "Field A" && second.placeholderString == "Field B")
            if let view = window.contentView, let bitmap = view.bitmapImageRepForCachingDisplay(in: view.bounds) {
                view.cacheDisplay(in: view.bounds, to: bitmap)
                try? bitmap.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: "/tmp/lemonvox-field-preview.png"))
            }
            first.stringValue = "fixture persistence check"
            for _ in 0..<3 {
                window.close()
                precondition(!window.isVisible)
                _ = applicationShouldHandleReopen(NSApp, hasVisibleWindows: false)
                precondition(window.isVisible && first.stringValue == "fixture persistence check")
            }
            first.stringValue = ""
            print("PASS: actual Field A / Field B window closes and reopens three times, preserving contents; Edit menu installed")
            NSApp.terminate(nil)
        }
    }
    @objc func resetFields() {
        first.stringValue = ""; second.stringValue = ""; secure.stringValue = ""
        window.makeFirstResponder(first); report()
    }
    @objc func changeLater() {
        Timer.scheduledTimer(withTimeInterval: 0.35, repeats: false) { [weak self] _ in
            guard let self else { return }
            self.window.makeFirstResponder(self.second); self.report()
        }
    }
    func controlTextDidChange(_ note: Notification) { report() }
    func report() {
        guard !CommandLine.arguments.contains("--smoke-reopen") else { return }
        func text(_ field: NSTextField) -> String {
            field.currentEditor()?.string ?? field.stringValue
        }
        let firstText = text(first), secondText = text(second), secureText = text(secure)
        let value: [String: Any] = [
            "firstLength": firstText.count, "secondLength": secondText.count,
            "secureLength": secureText.count,
            "firstEqualsSample": firstText == sample,
            "secondEqualsSample": secondText == sample,
            "automationPostEventAccess": CGPreflightPostEventAccess()
        ]
        if let data = try? JSONSerialization.data(withJSONObject: value, options: [.sortedKeys]) {
            do { try data.write(to: log, options: .atomic) }
            catch { print("Fixture state write failed: \(error)") }
        }
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        window.makeKeyAndOrderFront(nil)
        window.makeFirstResponder(first)
        return true
    }
}
let app = NSApplication.shared
app.setActivationPolicy(.regular)
let fixture = Fixture()
app.delegate = fixture
withExtendedLifetime(fixture) { app.run() }
