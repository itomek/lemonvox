import AppKit

func installFixtureMenus() {
    let main = NSMenu()
    let applicationItem = NSMenuItem()
    main.addItem(applicationItem)
    let applicationMenu = NSMenu(title: "LemonVox Test Fields")
    let quit = NSMenuItem(title: "Quit LemonVox Test Fields", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
    quit.target = NSApp
    applicationMenu.addItem(quit)
    applicationItem.submenu = applicationMenu
    let editItem = NSMenuItem(title: "Edit", action: nil, keyEquivalent: "")
    let edit = NSMenu(title: "Edit")
    for (title, selector, key) in [
        ("Undo", "undo:", "z"), ("Redo", "redo:", "z"),
        ("Cut", "cut:", "x"), ("Copy", "copy:", "c"),
        ("Paste", "paste:", "v"), ("Select All", "selectAll:", "a")
    ] {
        let item = NSMenuItem(title: title, action: Selector(selector), keyEquivalent: key)
        item.keyEquivalentModifierMask = title == "Redo" ? [.command, .shift] : .command
        item.target = nil // Route through the active field editor's responder chain.
        edit.addItem(item)
        if title == "Redo" { edit.addItem(.separator()) }
    }
    editItem.submenu = edit
    main.addItem(editItem)
    NSApp.mainMenu = main
}

final class PasteProbe: NSView {
    var pasteCount = 0
    var copyCount = 0
    var cutCount = 0
    var selectCount = 0
    override var acceptsFirstResponder: Bool { true }
    @objc func paste(_ sender: Any?) { pasteCount += 1 }
    @objc func copy(_ sender: Any?) { copyCount += 1 }
    @objc func cut(_ sender: Any?) { cutCount += 1 }
    override func selectAll(_ sender: Any?) { selectCount += 1 }
}

func checkFixtureKeyboardRouting(completion: @escaping (Bool) -> Void) {
    let probe = PasteProbe(frame: NSRect(x: 0, y: 0, width: 100, height: 30))
    let window = NSWindow(contentRect: probe.frame, styleMask: [.titled], backing: .buffered, defer: false)
    window.contentView = probe
    window.makeKeyAndOrderFront(nil)
    NSApp.activate(ignoringOtherApps: true)
    window.makeFirstResponder(probe)
    Timer.scheduledTimer(withTimeInterval: 0.3, repeats: false) { _ in
    let event = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: .command,
        timestamp: 0, windowNumber: window.windowNumber, context: nil, characters: "v",
        charactersIgnoringModifiers: "v", isARepeat: false, keyCode: 9)!
    print("Probe key=\(NSApp.keyWindow === window) responder=\(window.firstResponder === probe) active=\(NSApp.isActive)")
    let handled = NSApp.mainMenu?.performKeyEquivalent(with: event) ?? false
    var otherHandled = true
    for (key, code) in [("c", UInt16(8)), ("x", UInt16(7)), ("a", UInt16(0))] {
        let other = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: .command,
            timestamp: 0, windowNumber: window.windowNumber, context: nil, characters: key,
            charactersIgnoringModifiers: key, isARepeat: false, keyCode: code)!
        otherHandled = (NSApp.mainMenu?.performKeyEquivalent(with: other) ?? false) && otherHandled
    }
    let passed = handled && otherHandled && probe.pasteCount == 1 && probe.copyCount == 1 && probe.cutCount == 1 && probe.selectCount == 1
    print("\(passed ? "PASS" : "FAIL"): Cmd-V/C/X/A menu routing handled=\(handled) pasteActions=\(probe.pasteCount); no clipboard read/write")
    window.orderOut(nil)
    completion(passed)
    }
}
