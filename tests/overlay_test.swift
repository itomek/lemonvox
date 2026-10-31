import AppKit
@main struct OverlayTests {
    static func main() throws {
        setbuf(stdout, nil)
        NSApplication.shared.setActivationPolicy(.prohibited)
        for dark in [false, true] {
            let panel = IndicatorPanel()
            panel.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
            panel.setPhase(.recording)
            precondition(!panel.canBecomeKey && !panel.canBecomeMain)
            precondition(!panel.overlay.cancelButton.acceptsFirstResponder)
            precondition(panel.overlay.stopButton.isEnabled)
            var cancellations = 0, stops = 0
            panel.onCancel = { cancellations += 1 }
            panel.onStop = { stops += 1 }
            panel.overlay.cancelButton.performClick(nil)
            panel.overlay.stopButton.performClick(nil)
            precondition(cancellations == 1 && stops == 1)
            panel.present(.recording)
            precondition(panel.isVisible && panel.overlay.glass.window === panel)
            panel.setPhase(.processing)
            precondition(!panel.overlay.stopButton.isEnabled)
            panel.overlay.stopButton.performClick(nil)
            precondition(stops == 1)
            panel.overlay.cancelButton.performClick(nil)
            precondition(cancellations == 2)
            precondition(panel.overlay.subviews.contains { $0 is NSGlassEffectView }, "Native Liquid Glass surface missing")
            let labels = panel.overlay.content.subviews.compactMap { $0 as? NSTextField }
            precondition(labels.contains { ($0.font?.pointSize ?? 0) >= 12 }, "Readable native status missing")
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
            precondition(panel.overlay.stopButton.isHidden)
            precondition(panel.overlay.statusLabel.stringValue == "Target changed · text ready to copy")
            panel.orderOut(nil)
            panel.overlay.startAnimation()
            print("PASS: native glass, readable status, light/dark controls and phase callbacks")
        }
        if CommandLine.arguments.contains("--render") { try renderComparison() }
        focusCheck()
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
                    panel.present(.recording)
                    RunLoop.main.run(until: Date().addingTimeInterval(0.2))
                    precondition(NSApp.keyWindow === window && window.firstResponder === responder)
                    panel.overlay.cancelButton.performClick(nil)
                    precondition(window.firstResponder === responder)
                    panel.orderOut(nil)
                    let tick = panel.overlay.tick
                    panel.overlay.startAnimation()
                    RunLoop.main.run(until: Date().addingTimeInterval(0.2))
                    precondition(panel.overlay.tick == tick, "Hidden overlay restarted animation")
                }
                window.close()
                print("PASS: live glass preserves disposable field/key window; hidden animation stays stopped")
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
                let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 560, pixelsHigh: 112,
                    bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                    colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
                bitmap.size = view.bounds.size
                view.effectiveAppearance.performAsCurrentDrawingAppearance {
                    view.cacheDisplay(in: view.bounds, to: bitmap)
                }
                let snapshot = NSImage(size: view.bounds.size)
                snapshot.addRepresentation(bitmap)
                snapshot.draw(in: NSRect(x: 20 + column * 316, y: 99 - row * 72, width: 280, height: 56))
            }
        }
        image.unlockFocus()
        let representation = NSBitmapImageRep(data: image.tiffRepresentation!)!
        let output = URL(fileURLWithPath: ".build/lemonvox-light-dark.png")
        try representation.representation(using: .png, properties: [:])!.write(to: output)
        print("Rendered app-only appearance comparison: \(output.path)")
    }
}
