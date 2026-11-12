import AppKit
@main struct OverlayTests {
    static func main() throws {
        setbuf(stdout, nil)
        let bounds = NSRect(x: -1000, y: 20, width: 1000, height: 800)
        let inside = NSRect(x: -600, y: 200, width: IndicatorPanel.speechBarSize.width,
            height: IndicatorPanel.speechBarSize.height)
        precondition(IndicatorPanel.clampedOrigin(inside, in: bounds) == inside.origin)
        precondition(IndicatorPanel.clampedOrigin(NSRect(x: 20, y: 900,
            width: IndicatorPanel.speechBarSize.width, height: IndicatorPanel.speechBarSize.height), in: bounds) ==
            NSPoint(x: -IndicatorPanel.speechBarSize.width, y: bounds.maxY - IndicatorPanel.speechBarSize.height - 120))
        precondition(IndicatorPanel.clampedOrigin(NSRect(x: -1100, y: -20,
            width: IndicatorPanel.speechBarSize.width, height: IndicatorPanel.speechBarSize.height), in: bounds) == NSPoint(x: -1000, y: 20))
        let display = NSRect(x: -1920, y: -200, width: 1920, height: 1080)
        let bottomDock = NSRect(x: -1920, y: -140, width: 1920, height: 996)
        let dockOrigin = IndicatorPanel.anchoredOrigin(screenFrame: display, visibleFrame: bottomDock,
            dockIsHidden: false, dockOrientation: "bottom")
        precondition(dockOrigin == NSPoint(x: -1020, y: -132),
            "The tile must center on its screen, 8 pt above a visible bottom Dock")
        for orientation in ["bottom", "left", "right"] {
            let origin = IndicatorPanel.anchoredOrigin(screenFrame: display, visibleFrame: bottomDock,
                dockIsHidden: true, dockOrientation: orientation)
            precondition(origin == NSPoint(x: -1020, y: -192),
                "An autohidden Dock must leave the tile 8 pt above the physical screen bottom")
        }
        for orientation in ["left", "right"] {
            let origin = IndicatorPanel.anchoredOrigin(screenFrame: display, visibleFrame: bottomDock,
                dockIsHidden: false, dockOrientation: orientation)
            precondition(origin == NSPoint(x: -1020, y: -192),
                "A side Dock must not alter screen-centered bottom positioning")
        }
        print("PASS: panel clamping, message clearance and Dock-aware bottom placement")
        if CommandLine.arguments.contains("--geometry") { return }
        NSApplication.shared.setActivationPolicy(.prohibited)
        let appearancePanel = IndicatorPanel()
        appearancePanel.appearance = NSAppearance(named: .aqua)
        appearancePanel.applyMessageTransparencyPreference(true)
        let solidView = appearancePanel.messagePanel.contentView!
        let lightColor = solidView.layer!.backgroundColor!
        appearancePanel.appearance = NSAppearance(named: .darkAqua)
        let darkColor = solidView.layer!.backgroundColor!
        precondition(lightColor != darkColor, "Solid message background must follow appearance changes")
        precondition(appearancePanel.messagePanel.contentView === solidView)
        appearancePanel.setPhase(.message("Appearance check"))
        precondition(appearancePanel.messageLabel.stringValue == "Appearance check")
        precondition(appearancePanel.messageLabel.window === appearancePanel.messagePanel)
        appearancePanel.appearance = NSAppearance(named: .aqua)
        precondition(solidView.layer!.backgroundColor! == lightColor)
        appearancePanel.applyMessageTransparencyPreference(false)
        precondition(appearancePanel.messagePanel.contentView is NSGlassEffectView)
        precondition(appearancePanel.messageLabel.window === appearancePanel.messagePanel)
        appearancePanel.appearance = NSAppearance(named: .darkAqua)
        appearancePanel.applyMessageTransparencyPreference(true)
        precondition(appearancePanel.messagePanel.contentView!.layer!.backgroundColor! == darkColor)
        appearancePanel.applyMessageTransparencyPreference(false)
        appearancePanel.appearance = nil
        precondition(appearancePanel.messagePanel.appearance == nil)
        print("PASS: message appearance and transparency transitions")
        let solidOverlay = appearancePanel.overlay
        var tileColors: [CGColor] = []
        for appearance in [NSAppearance.Name.aqua, .darkAqua] {
            appearancePanel.appearance = NSAppearance(named: appearance)
            solidOverlay.applyTransparencyPreference(true)
            tileColors.append(solidOverlay.content.layer!.backgroundColor!)
            precondition(solidOverlay.microphone.contentTintColor == .labelColor)
            precondition(solidOverlay.stopButton.contentTintColor == .labelColor)
            precondition(solidOverlay.cancelButton.contentTintColor == .labelColor)
        }
        precondition(tileColors[0] != tileColors[1],
            "The solid tile background must adapt between light and dark appearances")
        solidOverlay.applyTransparencyPreference(false)
        precondition(!solidOverlay.glass.isHidden && solidOverlay.glass.contentView === solidOverlay.content)
        print("PASS: slim tile appearance and Reduce Transparency transitions")
        if CommandLine.arguments.contains("--appearance") { return }
        if CommandLine.arguments.contains("--render-only") { try renderComparison(); return }
        for dark in [false, true] {
            let panel = IndicatorPanel()
            panel.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
            precondition(panel.messagePanel.appearance == panel.appearance, "Message panel must inherit the selected appearance")
            panel.setPhase(.recording)
            precondition(panel.frame.size == IndicatorPanel.speechBarSize,
                "Speech bar geometry must use the declared compact native size")
            precondition(panel.frame.width <= 128 && panel.frame.height <= 44,
                "Speech controls must fit a compact bar")
            precondition(panel.overlay.glass.frame.size == NSSize(width: 120, height: 32),
                "The visible tile must match the approved slim 120 × 32 pt design")
            precondition(panel.overlay.statusLabel.isHidden,
                "The compact speech bar must not show status text")
            precondition(panel.overlay.cancelButton.frame.width >= 20 && panel.overlay.cancelButton.frame.height >= 20)
            precondition(panel.overlay.bounds.contains(panel.overlay.cancelButton.frame),
                "The secondary cancel control must remain inside the panel hit region")
            precondition(panel.isMovableByWindowBackground)
            precondition(panel.overlay.hitTest(NSPoint(x:40,y:22))?.mouseDownCanMoveWindow == true)
            assertControlIsNotDragHandle(panel.overlay.cancelButton)
            precondition(panel.overlay.cancelButton.mouseDownCanMoveWindow == false)
            precondition(!panel.canBecomeKey && !panel.canBecomeMain)
            precondition(!panel.overlay.cancelButton.acceptsFirstResponder)
            precondition(panel.overlay.cancelButton.isEnabled)
            var finishes = 0
            panel.onStop = { finishes += 1 }
            assertControlIsNotDragHandle(panel.overlay.stopButton)
            panel.overlay.stopButton.performClick(nil)
            precondition(finishes == 1)
            var cancellations = 0
            panel.onCancel = { cancellations += 1 }
            panel.overlay.cancelButton.performClick(nil)
            precondition(cancellations == 1)
            panel.present(.recording)
            precondition(panel.isVisible && panel.overlay.glass.window === panel)
            let moved = NSPoint(x:panel.frame.minX+24,y:panel.frame.minY+24)
            panel.setFrameOrigin(moved)
            let movedOrigin = panel.frame.origin
            panel.present(.processing)
            precondition(panel.overlay.stopButton.isHidden && !panel.overlay.stopButton.isEnabled)
            panel.overlay.stopButton.performClick(nil)
            precondition(finishes == 1)
            precondition(panel.frame.origin == movedOrigin, "A new phase must retain the moved bar position")
            panel.overlay.cancelButton.performClick(nil)
            precondition(cancellations == 2)
            panel.showNotice("Cancellation shortcut unavailable · use X or menu")
            precondition(panel.overlay.cancelButton.isHidden == false && panel.messagePanel.isVisible,
                "A cancellation-binding warning must preserve the X fallback")
            precondition(panel.overlay.subviews.contains { $0 is NSGlassEffectView }, "Native Liquid Glass surface missing")
            precondition(panel.overlay.statusLabel.isHidden, "Speech bar must contain no visible text")
            panel.overlay.applyTransparencyPreference(true)
            precondition(panel.overlay.glass.isHidden && panel.overlay.content.superview === panel.overlay)
            panel.overlay.applyTransparencyPreference(false)
            precondition(!panel.overlay.glass.isHidden && panel.overlay.glass.contentView === panel.overlay.content)
            panel.overlay.isSample = false
            panel.overlay.progressLabel = "TRANSCRIBING"
            precondition(panel.overlay.statusLabel.stringValue == "Transcribing…")
            panel.overlay.progressLabel = "CLEANING UP"
            precondition(panel.overlay.statusLabel.stringValue == "Cleaning up…")
            panel.setPhase(.message("Target changed · text ready to copy"))
            precondition(!panel.overlay.cancelButton.isHidden,
                "X remains the sole dismiss action for a message panel")
            precondition(panel.overlay.statusLabel.stringValue == "Target changed · text ready to copy")
            precondition(panel.messagePanel.isVisible && panel.messagePanel.frame.minY >= panel.frame.maxY)
            precondition(panel.messageLabel.stringValue == "Target changed · text ready to copy")
            var opened = 0, retried = 0
            panel.onOpenSettings = { opened += 1 }
            panel.onRetryPermission = { retried += 1 }
            panel.showPermission("Enable Accessibility; the previous trigger remains active.")
            precondition(!panel.canBecomeKey && panel.overlay.openSettingsButton.isHidden == false)
            precondition(!panel.overlay.retryPermissionButton.isHidden)
            precondition(panel.overlay.openSettingsButton.frame.maxY < panel.messageLabel.frame.minY)
            precondition(panel.overlay.retryPermissionButton.frame.maxX < panel.messagePanel.contentView!.bounds.maxX)
            assertControlIsNotDragHandle(panel.overlay.openSettingsButton)
            assertControlIsNotDragHandle(panel.overlay.retryPermissionButton)
            panel.overlay.openSettingsButton.performClick(nil)
            panel.overlay.retryPermissionButton.performClick(nil)
            precondition(opened == 1 && retried == 1)
            // The move observer clamps first; posting the screen notification exercises its delivery,
            // without emulating display removal or independently proving screen-change recovery.
            panel.setFrameOrigin(NSPoint(x: 1_000_000, y: 1_000_000))
            NotificationCenter.default.post(name: NSApplication.didChangeScreenParametersNotification, object: nil)
            precondition(NSScreen.screens.contains { $0.visibleFrame.contains(panel.frame) },
                "Move/screen notifications left the bar unreachable")
            precondition(NSScreen.screens.contains { $0.visibleFrame.contains(panel.messagePanel.frame) },
                "Move/screen notifications left permission controls unreachable")
            let retainedOrigin = panel.frame.origin
            panel.setPhase(.recording)
            precondition(panel.frame.origin == retainedOrigin && !panel.messagePanel.isVisible)
            precondition(panel.overlay.openSettingsButton.isHidden && panel.overlay.retryPermissionButton.isHidden)
            panel.overlay.openSettingsButton.performClick(nil)
            precondition(opened == 1, "Hidden permission action must not run during dictation")
            panel.orderOut(nil)
            panel.overlay.startAnimation()
            print("PASS: native glass, separate messages, light/dark controls and phase callbacks")
        }
        if CommandLine.arguments.contains("--render") { try renderComparison() }
        focusCheck()
    }
    static func assertControlIsNotDragHandle(_ button: OverlayButton) {
        precondition(!button.mouseDownCanMoveWindow, "A control became a drag handle")
        let root = button.window!.contentView!
        let center = root.convert(NSPoint(x: button.bounds.midX, y: button.bounds.midY), from: button)
        precondition(root.hitTest(center) === button, "A control lost its mouse hit target")
    }
    static func focusCheck() {
        NSApp.setActivationPolicy(.regular)
        let field = NSTextField(string: "Disposable focus check")
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 400, height: 100),
            styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        field.frame = NSRect(x: 20, y: 30, width: 360, height: 24)
        window.contentView?.addSubview(field)
        Timer.scheduledTimer(withTimeInterval: 0.2, repeats: false) { _ in
            NSApp.activate(ignoringOtherApps: true)
            window.makeKeyAndOrderFront(nil)
            window.makeFirstResponder(field)
            Timer.scheduledTimer(withTimeInterval: 0.3, repeats: false) { _ in
                let responder = window.firstResponder
                print("Focus fixture: active=\(NSApp.isActive) key=\(NSApp.keyWindow === window) editor=\(field.currentEditor() != nil)")
                precondition(field.currentEditor() != nil && NSApp.keyWindow === window)
                let panel = IndicatorPanel()
                for dark in [false, true] {
                    panel.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
                    for phase in [OverlayPhase.recording, .processing,
                                  .message("Disposable message"), .permission("Disposable permission")] {
                        panel.present(phase)
                        // Exercise programmatic repositioning; this is not a physical pointer drag.
                        panel.setFrameOrigin(NSPoint(x: panel.frame.minX + 8, y: panel.frame.minY + 8))
                        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
                        precondition(NSApp.keyWindow === window && window.firstResponder === responder,
                            "A visible phase or reposition stole disposable text focus")
                    }
                    panel.present(.recording)
                    panel.overlay.cancelButton.performClick(nil)
                    precondition(window.firstResponder === responder)
                    panel.orderOut(nil)
                    let tick = panel.overlay.tick
                    panel.overlay.startAnimation()
                    RunLoop.main.run(until: Date().addingTimeInterval(0.2))
                    precondition(panel.overlay.tick == tick, "Hidden overlay restarted animation")
                }
                window.close()
                print("PASS: all phases and synthetic reposition preserve disposable field/key window; hidden animation stays stopped")
                NSApp.terminate(nil)
            }
        }
        NSApp.run()
    }
    static func renderComparison() throws {
        let image = NSImage(size: NSSize(width: 640, height: 200))
        image.lockFocus()
        NSColor(srgbRed: 0.47, green: 0.49, blue: 0.47, alpha: 1).setFill()
        NSRect(x: 0, y: 0, width: 640, height: 200).fill()
        for (column, dark) in [false, true].enumerated() {
            let title = dark ? "Dark appearance" : "Light appearance"
            (title as NSString).draw(at: NSPoint(x: 22 + column * 316, y: 171), withAttributes: [
                .font: NSFont.systemFont(ofSize: 12, weight: .medium), .foregroundColor: NSColor.white])
            for (row, phase) in [OverlayPhase.recording, .processing].enumerated() {
                let panel = IndicatorPanel()
                panel.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
                panel.setPhase(phase)
                panel.overlay.layoutSubtreeIfNeeded()
                let view = panel.overlay
                let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(view.bounds.width*2), pixelsHigh: Int(view.bounds.height*2),
                    bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                    colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
                bitmap.size = view.bounds.size
                view.effectiveAppearance.performAsCurrentDrawingAppearance {
                    view.cacheDisplay(in: view.bounds, to: bitmap)
                }
                let snapshot = NSImage(size: view.bounds.size)
                snapshot.addRepresentation(bitmap)
                let origin = NSPoint(x:CGFloat(20 + column * 316),y:CGFloat(99 - row * 72))
                snapshot.draw(in:NSRect(origin:origin,size:view.bounds.size))
            }
        }
        image.unlockFocus()
        let representation = NSBitmapImageRep(data: image.tiffRepresentation!)!
        let output = URL(fileURLWithPath: ".build/lemonvox-light-dark.png")
        try representation.representation(using: .png, properties: [:])!.write(to: output)
        print("Rendered app-only appearance comparison: \(output.path)")
    }
}
