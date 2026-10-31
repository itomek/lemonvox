import AppKit
@main struct PickerTests {
    @MainActor static func main() {
        setbuf(stdout,nil)
        NSApplication.shared.setActivationPolicy(.accessory)
        let picker=ModelPicker()
        let choices=(0..<1200).map { ModelChoice(id:"openrouter.model-\($0)",title:"openrouter.model-\($0) · remote") }
        picker.update(choices:choices,selected:"openrouter.model-1199")
        precondition(picker.choices.count == 1200 && picker.selectedID == "openrouter.model-1199")
        picker.pickerContent.search.stringValue="MODEL-1199"; picker.pickerContent.filter()
        precondition(picker.pickerContent.visibleChoices.count == 1)
        picker.pickerContent.choose()
        precondition(picker.selectedID == "openrouter.model-1199")
        picker.pickerContent.search.stringValue="nothing-matches"; picker.pickerContent.filter()
        precondition(picker.pickerContent.visibleChoices.isEmpty && !picker.pickerContent.chooseButton.isEnabled)
        picker.pickerContent.choose()
        precondition(picker.selectedID == "openrouter.model-1199")
        picker.pickerContent.search.stringValue="model-25"; picker.pickerContent.filter()
        picker.pickerContent.cancelOperation(nil)
        precondition(picker.selectedID == "openrouter.model-1199")
        picker.update(choices:[],selected:"")
        picker.pickerContent.search.stringValue=""; picker.pickerContent.filter()
        precondition(picker.pickerContent.resultLabel.stringValue == "No models available")
        picker.update(choices:choices,selected:"openrouter.model-1199")
        precondition(picker.pickerContent.preferredContentSize.height == 360)
        print("PASS: full large catalog, case-insensitive search, no-match safety and canceled selection preservation")
        let window=NSWindow(contentRect:NSRect(x:0,y:0,width:560,height:120),styleMask:[.titled,.closable],backing:.buffered,defer:false)
        window.isReleasedWhenClosed=false; window.title="LemonVox picker test"
        picker.frame=NSRect(x:20,y:40,width:520,height:30); window.contentView?.addSubview(picker)
        Timer.scheduledTimer(withTimeInterval:0.2,repeats:false) { _ in
            Task { @MainActor in
                window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps:true)
                for dark in [false,true] {
                    window.appearance=NSAppearance(named:dark ? .darkAqua : .aqua)
                    picker.performClick(nil)
                    try? await Task.sleep(nanoseconds:350_000_000)
                    precondition(picker.popover.isShown && picker.popover.contentSize.height == 360)
                    precondition(picker.pickerContent.view.effectiveAppearance.bestMatch(from:[.darkAqua,.aqua]) == (dark ? .darkAqua : .aqua))
                    if CommandLine.arguments.contains("--render") {
                        let view=picker.pickerContent.view
                        let image=NSImage(size:view.bounds.size); image.lockFocus()
                        view.effectiveAppearance.performAsCurrentDrawingAppearance {
                            NSColor.windowBackgroundColor.setFill(); view.bounds.fill()
                            if let bitmap=view.bitmapImageRepForCachingDisplay(in:view.bounds) {
                                view.cacheDisplay(in:view.bounds,to:bitmap)
                                let part=NSImage(size:view.bounds.size); part.addRepresentation(bitmap); part.draw(in:view.bounds)
                            }
                        }
                        image.unlockFocus()
                        if let data=image.tiffRepresentation,let bitmap=NSBitmapImageRep(data:data) {
                            try? bitmap.representation(using:.png,properties:[:])?.write(to:URL(fileURLWithPath:dark ? ".build/picker-dark.png" : ".build/picker-light.png"))
                        }
                    }
                    precondition(picker.pickerContent.visibleChoices.count == 1200)
                    precondition(picker.pickerContent.table.selectedRow == 1199)
                    picker.pickerContent.search.stringValue="nothing-matches"; picker.pickerContent.filter()
                    precondition(picker.pickerContent.resultLabel.stringValue == "No matching models")
                    _ = picker.pickerContent.control(picker.pickerContent.search,textView:NSTextView(),doCommandBy:#selector(NSResponder.insertNewline(_:)))
                    precondition(picker.selectedID == "openrouter.model-1199")
                    picker.pickerContent.search.stringValue="model-7"; picker.pickerContent.filter()
                    _ = picker.pickerContent.control(picker.pickerContent.search,textView:NSTextView(),doCommandBy:#selector(NSResponder.moveDown(_:)))
                    let handled=picker.pickerContent.control(picker.pickerContent.search,textView:NSTextView(),doCommandBy:#selector(NSResponder.cancelOperation(_:)))
                    precondition(handled && picker.pickerContent.owner === picker)
                    try? await Task.sleep(nanoseconds:350_000_000)
                    precondition(!picker.popover.isShown && picker.selectedID == "openrouter.model-1199")
                    picker.performClick(nil)
                    try? await Task.sleep(nanoseconds:350_000_000)
                    precondition(picker.pickerContent.search.stringValue.isEmpty && picker.pickerContent.table.selectedRow == 1199)
                    picker.pickerContent.search.stringValue="model-117"; picker.pickerContent.filter()
                    _ = picker.pickerContent.control(picker.pickerContent.search,textView:NSTextView(),doCommandBy:#selector(NSResponder.insertNewline(_:)))
                    try? await Task.sleep(nanoseconds:350_000_000)
                    precondition(!picker.popover.isShown && picker.selectedID == "openrouter.model-117")
                    picker.update(choices:choices,selected:"openrouter.model-1199")
                }
                window.close()
                print("PASS: bounded live popover in light/dark; current row, search/arrows/Return/Escape, select/dismiss/reopen")
                NSApp.terminate(nil)
            }
        }
        withExtendedLifetime(window) { NSApp.run() }
    }
}
