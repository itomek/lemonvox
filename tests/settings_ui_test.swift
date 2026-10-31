import AppKit

@main struct SettingsUITests {
    @MainActor static func main() {
        setbuf(stdout,nil)
        let directory=FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let log=DiagnosticLog(directory:directory)
        let asr=LocalModel(id:"Whisper-Base",downloaded:true,recipe:"whispercpp",labels:["transcription"],cloud_provider:nil)
        let gemma=LocalModel(id:"Gemma-4-E4B-it-GGUF",downloaded:true,recipe:"llamacpp",labels:["chat"],cloud_provider:nil)
        let remote=LocalModel(id:"remote.chat",downloaded:nil,recipe:"cloud",labels:["chat"],cloud_provider:"remote")
        let pane=SettingsController(log:log,discover:{ endpoint in
            try await Task.sleep(nanoseconds:300_000_000)
            if endpoint.contains("13307") { throw DictationError.serverUnavailable }
            if endpoint.contains("13306") { throw DictationError.upstream(401,"api_key=private-secret denied") }
            return DiscoveredModels(transcription:[asr],cleanup:[gemma,remote],reason:nil,version:nil)
        })
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
        pane.show(config:config,save:{ saved=$0 },close:{ closed=true })
        precondition(pane.window.isVisible && pane.loading && pane.endpoint.isEditable && pane.save.isEnabled)
        precondition(pane.prompt.string == config.cleanupPrompt)
        Timer.scheduledTimer(withTimeInterval:0.7,repeats:false) { _ in
            MainActor.assumeIsolated {
                precondition(!pane.loading && pane.cleanup.choices.count == 2 && pane.transcription.choices.count == 1)
                precondition(pane.cleanup.selectedID == gemma.id)
                if CommandLine.arguments.contains("--render"), let view=pane.window.contentView {
                    let output=NSImage(size:NSSize(width:1280,height:690))
                    output.lockFocus()
                    for (column,dark) in [false,true].enumerated() {
                        pane.window.appearance=NSAppearance(named:dark ? .darkAqua : .aqua)
                        view.layoutSubtreeIfNeeded()
                        pane.window.displayIfNeeded()
                        RunLoop.main.run(until:Date().addingTimeInterval(0.15))
                        view.effectiveAppearance.performAsCurrentDrawingAppearance {
                            NSColor.windowBackgroundColor.setFill()
                            NSRect(x:column*640,y:0,width:640,height:690).fill()
                        }
                        if let bitmap=view.bitmapImageRepForCachingDisplay(in:view.bounds) {
                            view.effectiveAppearance.performAsCurrentDrawingAppearance { view.cacheDisplay(in:view.bounds,to:bitmap) }
                            let image=NSImage(size:view.bounds.size); image.addRepresentation(bitmap)
                            image.draw(in:NSRect(x:column*640,y:0,width:640,height:690))
                        }
                    }
                    output.unlockFocus()
                    if let data=output.tiffRepresentation, let bitmap=NSBitmapImageRep(data:data) {
                        try? bitmap.representation(using:.png,properties:[:])?.write(to:URL(fileURLWithPath:".build/settings-light-dark.png"))
                    }
                }
                pane.cleanup.commit(ModelChoice(id:remote.id,title:remote.id + " · remote"))
                pane.endpoint.stringValue="http://127.0.0.1:13306/v1"; pane.refresh()
                precondition(pane.loading && pane.save.isEnabled)
                Timer.scheduledTimer(withTimeInterval:0.6,repeats:false) { _ in
                    MainActor.assumeIsolated {
                        precondition(!pane.loading && pane.status.stringValue.contains("401"))
                        precondition(!pane.status.stringValue.contains("private-secret"))
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
                                pane.saveSettings()
                                precondition(saved?.asrModel == config.asrModel && saved?.cleanupModel == config.cleanupModel)
                                precondition(saved?.cleanupPrompt == config.cleanupPrompt && !pane.window.isVisible)
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
