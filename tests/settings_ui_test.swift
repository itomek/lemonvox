import AppKit

@MainActor private func debugDiagnosticsPreferenceTest() {
    _ = NSApplication.shared
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("lemonvox-settings-diagnostics-\(UUID().uuidString)")
    defer { try? FileManager.default.removeItem(at: directory) }

    let log = DiagnosticLog(directory: directory)
    let config = DictationConfig(
        endpoint: "http://127.0.0.1:13305/v1",
        asrModel: "asr",
        cleanupModel: "cleanup",
        cleanupPrompt: "Synthetic prompt"
    )
    let pane = SettingsController(log: log, discover: { _ in
        DiscoveredModels(transcription: [], cleanup: [], reason: nil, version: nil)
    })
    pane.window.alphaValue = 0
    var persisted: [Bool] = []
    pane.saveDiagnosticPreferences = { persisted.append($0) }

    pane.show(config: config, save: { _ in }, close: {})
    precondition(pane.debugDiagnostics.state == .off && !log.debugEnabled)
    precondition(pane.debugDiagnostics.title == "Enable Debug diagnostic traces")
    precondition(pane.debugDiagnostics.accessibilityLabel() == "Enable Debug diagnostic traces")
    let boundary = pane.debugDiagnostics.toolTip ?? ""
    for expected in ["dictated text", "audio", "credentials", "private fields", "clipboard", "provider responses"] {
        precondition(boundary.contains(expected), "Debug diagnostics boundary must mention \(expected)")
    }

    pane.debugDiagnostics.state = .on
    pane.endpoint.stringValue = "https://example.invalid/v1"
    pane.saveSettings()
    precondition(pane.window.isVisible && !log.debugEnabled && persisted.isEmpty)

    pane.endpoint.stringValue = config.endpoint
    pane.window.close()
    precondition(!log.debugEnabled && persisted.isEmpty)

    pane.show(config: config, save: { _ in }, close: {})
    precondition(pane.debugDiagnostics.state == .off)
    pane.debugDiagnostics.state = .on
    pane.saveSettings()
    precondition(log.debugEnabled && persisted == [true] && !pane.window.isVisible)

    pane.show(config: config, save: { _ in }, close: {})
    precondition(pane.debugDiagnostics.state == .on)
    pane.window.close()
    print("PASS: Debug diagnostics is quiet by default, documents exclusions, and persists only after Save")
}

@main struct SettingsUITests {
    @MainActor static func main() {
        setbuf(stdout,nil)
        for (info,expected) in [
            (["CFBundleShortVersionString":"1.2.3", "CFBundleVersion":"42"], "LemonVox 1.2.3 (build 42)"),
            (["CFBundleVersion":"1"], "LemonVox unavailable (build 1)"),
            (["CFBundleShortVersionString":"1.2.3"], "LemonVox 1.2.3 (build unavailable)"),
            ([:], "LemonVox unavailable (build unavailable)"),
            (["CFBundleShortVersionString":" ", "CFBundleVersion":42], "LemonVox unavailable (build unavailable)")
        ] as [([String:Any],String)] {
            precondition(RunningAppVersion.display(info:info) == expected)
        }
        if CommandLine.arguments.contains("--metadata-only") {
            print("PASS: running app version formats complete, missing, partial and malformed bundle metadata")
            return
        }
        if CommandLine.arguments.contains("--diagnostics-only") {
            debugDiagnosticsPreferenceTest()
            return
        }
        let directory=FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let log=DiagnosticLog(directory:directory)
        let asr=LocalModel(id:"Whisper-Base",downloaded:true,recipe:"whispercpp",labels:["transcription"],cloud_provider:nil)
        let gemma=LocalModel(id:"Gemma-4-E4B-it-GGUF",downloaded:true,recipe:"llamacpp",labels:["chat"],cloud_provider:nil)
        let remote=LocalModel(id:"remote.chat",downloaded:nil,recipe:"cloud",labels:["chat"],cloud_provider:"remote")
        var discoveries = 0
        let pane=SettingsController(log:log,discover:{ endpoint in
            discoveries += 1
            try await Task.sleep(nanoseconds:300_000_000)
            if endpoint.contains("13307") { throw DictationError.serverUnavailable }
            if endpoint.contains("13306") { throw DictationError.upstream(401,"api_key=private-secret denied") }
            return DiscoveredModels(transcription:[asr],cleanup:[gemma,remote],reason:nil,version:nil)
        })
        let footer=pane.window.contentView?.subviews.compactMap { $0 as? NSTextField }.first
        precondition(footer != nil, "Settings must show the running LemonVox version in its fixed footer")
        precondition(footer === pane.appVersion && pane.appVersion.isSelectable)
        precondition(pane.appVersion.accessibilityLabel() == "Running \(AppIdentity.running.name) version and build")
        precondition(!pane.appVersion.frame.intersects(pane.save.frame))
        let expectedVersion=Bundle.main.object(forInfoDictionaryKey:"CFBundleShortVersionString") as? String ?? "unavailable"
        let expectedBuild=Bundle.main.object(forInfoDictionaryKey:"CFBundleVersion") as? String ?? "unavailable"
        precondition(footer?.stringValue == "\(AppIdentity.running.name) \(expectedVersion) (build \(expectedBuild))")
        let guidance = AppIdentity(info:["CFBundleDisplayName":"LemonVox Debug"], path:"/Users/example/Applications/LemonVox Debug.app").accessibilityGuidance(trusted:false, canPost:false)
        pane.setAccessibilityStatus(guidance)
        precondition(pane.accessibilityStatus.toolTip == guidance && pane.accessibilityStatus.isSelectable)
        precondition(pane.accessibilityStatus.frame.maxY <= pane.logPath.frame.minY)
        let required = pane.accessibilityStatus.cell!.cellSize(forBounds: NSRect(x:0,y:0,width:pane.accessibilityStatus.frame.width,height:1000)).height
        precondition(required <= pane.accessibilityStatus.frame.height, "Accessibility guidance must fit")
        pane.window.setFrame(NSRect(x:0,y:0,width:820,height:480), display:true)
        pane.categoryList.selectRowIndexes(IndexSet(integer:2), byExtendingSelection:false)
        pane.accessibilityStatus.scrollToVisible(pane.accessibilityStatus.bounds)
        precondition(pane.accessibilityStatus.visibleRect.contains(pane.accessibilityStatus.bounds), "Accessibility guidance must be visible at minimum height")
        pane.categoryList.selectRowIndexes(IndexSet(integer:0), byExtendingSelection:false)
        let config=DictationConfig(endpoint:"http://127.0.0.1:13305/v1",asrModel:asr.id,cleanupModel:gemma.id,cleanupPrompt:"Preserved prompt")
        pane.showPreferences(trigger:.defaultMac,dark:true,current:false) { _,_,_ in }
        precondition(pane.trigger.choice == .defaultMac && pane.dark.state == .on && pane.currentField.state == .off)
        pane.trigger.mode.selectItem(at:1); pane.trigger.modeChanged(); pane.trigger.beginRecording()
        precondition(pane.trigger.capture(key:9,flags:[.command,.option],repeating:false))
        precondition(pane.trigger.choice == .combo(key:9,modifiers:2304) && !pane.trigger.recording)
        pane.trigger.beginRecording(); _=pane.trigger.capture(key:53,flags:[],repeating:false)
        precondition(!pane.trigger.recording && pane.trigger.choice == .combo(key:9,modifiers:2304))
        pane.trigger.beginRecording(); _=pane.trigger.capture(key:0,flags:[],repeating:false)
        precondition(pane.trigger.recording); pane.trigger.cancelRecording()
        var saved:DictationConfig?
        var closed=false
        var updateSaved:(Bool,Bool)?
        pane.updatePreferences={ (false,true) }
        pane.saveUpdatePreferences={ updateSaved=($0,$1) }
        pane.show(config:config,save:{ saved=$0 },close:{ closed=true })
        precondition(pane.window.isVisible && pane.loading && pane.endpoint.isEditable && pane.save.isEnabled)
        precondition(pane.categoryList.numberOfRows == 3)
        pane.prompt.string = "Unsaved synthetic draft"
        pane.dark.state = .on
        pane.trigger.beginRecording()
        let triggerBeforeSwitch = pane.trigger.choice
        pane.categoryList.selectRowIndexes(IndexSet(integer: 1), byExtendingSelection: false)
        precondition(!pane.trigger.capture(key:0, flags:[], repeating:false), "Hidden recorder must not consume typing")
        precondition(pane.trigger.choice == triggerBeforeSwitch && !pane.trigger.recording)
        precondition(pane.scrolls[1].isHidden == false && pane.scrolls[0].isHidden)
        pane.categoryList.selectRowIndexes(IndexSet(integer: 2), byExtendingSelection: false)
        pane.debugDiagnostics.state = .on
        pane.categoryList.selectRowIndexes(IndexSet(integer: 0), byExtendingSelection: false)
        precondition(pane.dark.state == .on && pane.prompt.string == "Unsaved synthetic draft")
        precondition(pane.debugDiagnostics.state == .on && pane.loading)
        precondition(discoveries <= 1, "Switching categories must not restart discovery")
        pane.window.setFrame(NSRect(x:0,y:0,width:820,height:480), display:true)
        pane.categoryList.selectRowIndexes(IndexSet(integer:1), byExtendingSelection:false)
        let modelClip = pane.scrolls[1].contentView
        modelClip.scroll(to:NSPoint(x:0,y:0))
        pane.endpoint.stringValue = "https://example.invalid/v1"
        pane.saveSettings()
        precondition(pane.window.isVisible && pane.categoryList.selectedRow == 1)
        precondition(pane.status.visibleRect.contains(pane.status.bounds), "Save error must be visible after scrolling")
        modelClip.scroll(to:NSPoint(x:0,y:0))
        pane.categoryList.selectRowIndexes(IndexSet(integer:0), byExtendingSelection:false)
        pane.saveSettings()
        precondition(pane.categoryList.selectedRow == 1 && pane.status.visibleRect.contains(pane.status.bounds))
        pane.endpoint.stringValue = config.endpoint
        pane.prompt.string = config.cleanupPrompt
        pane.debugDiagnostics.state = .off
        precondition(pane.prompt.string == config.cleanupPrompt)
        Timer.scheduledTimer(withTimeInterval:0.7,repeats:false) { _ in
            MainActor.assumeIsolated {
                precondition(!pane.loading && pane.cleanup.choices.count == 2 && pane.transcription.choices.count == 1)
                precondition(discoveries == 1, "Category changes must not refetch models")
                precondition(pane.cleanup.selectedID == gemma.id)
                if CommandLine.arguments.contains("--render"), let view=pane.window.contentView {
                    pane.window.setContentSize(NSSize(width:820,height:690))
                    let output=NSImage(size:NSSize(width:1640,height:2070))
                    output.lockFocus()
                    for row in 0..<3 {
                    pane.categoryList.selectRowIndexes(IndexSet(integer:row),byExtendingSelection:false)
                    for (column,dark) in [false,true].enumerated() {
                        pane.window.appearance=NSAppearance(named:dark ? .darkAqua : .aqua)
                        view.layoutSubtreeIfNeeded()
                        pane.window.displayIfNeeded()
                        RunLoop.main.run(until:Date().addingTimeInterval(0.15))
                        view.effectiveAppearance.performAsCurrentDrawingAppearance {
                            NSColor.windowBackgroundColor.setFill()
                            NSRect(x:column*820,y:(2-row)*690,width:820,height:690).fill()
                        }
                        if let bitmap=view.bitmapImageRepForCachingDisplay(in:view.bounds) {
                            view.effectiveAppearance.performAsCurrentDrawingAppearance { view.cacheDisplay(in:view.bounds,to:bitmap) }
                            let image=NSImage(size:view.bounds.size); image.addRepresentation(bitmap)
                            image.draw(in:NSRect(x:column*820,y:(2-row)*690,width:820,height:690))
                        }
                    }
                    }
                    output.unlockFocus()
                    if let data=output.tiffRepresentation, let bitmap=NSBitmapImageRep(data:data) {
                        let prefix="--render-output="
                        let path=CommandLine.arguments.first { $0.hasPrefix(prefix) }.map { String($0.dropFirst(prefix.count)) } ?? ".build/settings-light-dark.png"
                        try? bitmap.representation(using:.png,properties:[:])?.write(to:URL(fileURLWithPath:path))
                    }
                }
                pane.cleanup.commit(ModelChoice(id:remote.id,title:remote.id + " · remote"))
                pane.endpoint.stringValue="http://127.0.0.1:13306/v1"; pane.refresh()
                precondition(pane.loading && pane.save.isEnabled)
                Timer.scheduledTimer(withTimeInterval:0.6,repeats:false) { _ in
                    MainActor.assumeIsolated {
                        precondition(!pane.loading && pane.status.stringValue.contains("401"))
                        precondition(!pane.status.stringValue.contains("private-secret"))
                        let error = pane.status.stringValue
                        pane.categoryList.selectRowIndexes(IndexSet(integer:2), byExtendingSelection:false)
                        pane.categoryList.selectRowIndexes(IndexSet(integer:1), byExtendingSelection:false)
                        precondition(pane.status.stringValue == error && !pane.loading)
                        precondition(pane.cleanup.selectedID == remote.id)
                        pane.endpoint.stringValue="http://127.0.0.1:13307/v1"; pane.refresh()
                        Timer.scheduledTimer(withTimeInterval:0.6,repeats:false) { _ in
                            MainActor.assumeIsolated {
                                precondition(pane.status.stringValue.contains("unavailable") && !pane.loading)
                                precondition(pane.cleanup.selectedID == remote.id)
                                pane.endpoint.stringValue=config.endpoint; pane.refresh()
                                pane.window.close()
                        precondition(closed && !pane.window.isVisible)
                        pane.show(config:config,save:{ saved=$0 },close:{ closed=true })
                        Timer.scheduledTimer(withTimeInterval:0.6,repeats:false) { _ in
                            MainActor.assumeIsolated {
                                pane.prompt.string = "Saved across categories"
                                pane.categoryList.selectRowIndexes(IndexSet(integer:2), byExtendingSelection:false)
                                pane.saveSettings()
                                precondition(updateSaved?.0 == false && updateSaved?.1 == true)
                                precondition(saved?.asrModel == config.asrModel && saved?.cleanupModel == config.cleanupModel)
                                precondition(saved?.cleanupPrompt == "Saved across categories" && !pane.window.isVisible)
                                try? FileManager.default.removeItem(at:directory)
                                print("PASS: Settings opens during delayed discovery; selectors independent; offline/retry preserves choices; close/reopen and save preserve config; errors redacted")
                                NSApp.terminate(nil)
                            }
                        }
                            }
                        }
                    }
                }
            }
        }
        withExtendedLifetime(pane) { NSApp.run() }
    }
}
