import AppKit
import Carbon
@main struct HotKeyTests {
    static func main() {
        NSApplication.shared.setActivationPolicy(.prohibited)
        let binding = HotKeyBinding()
        precondition(binding.replace(with: 0) == noErr, "Default shortcut busy; test requires free Command-Option-V")
        let original = binding.reference
        precondition(binding.replace(with: 0) == noErr && binding.reference == original)
        precondition(binding.press() && !binding.press())
        binding.release()
        precondition(binding.press())
        var competitor: EventHotKeyRef?
        precondition(RegisterEventHotKey(UInt32(kVK_Space), UInt32(cmdKey | shiftKey),
            EventHotKeyID(signature: 0x54455354, id: 9), GetApplicationEventTarget(), 0, &competitor) == noErr,
            "Alternative shortcut busy; test requires free Command-Shift-Space")
        precondition(binding.replace(with: 1) != noErr)
        precondition(binding.reference == original && binding.choice == 0)
        if let competitor { UnregisterEventHotKey(competitor) }
        precondition(binding.replace(with: 1) == noErr && binding.choice == 1)
        binding.clear()
        precondition(binding.reference == nil && binding.choice == nil)
        print("PASS: live Carbon collision preserves binding, replacement, repeat filter, teardown")
    }
}
