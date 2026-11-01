import AppKit
import Carbon
import ApplicationServices
import AVFoundation

@MainActor func focusedElement() -> AXUIElement? {
    guard AXIsProcessTrusted(),let pid=NSWorkspace.shared.frontmostApplication?.processIdentifier else { return nil }
    for root in [AXUIElementCreateApplication(pid),AXUIElementCreateSystemWide()] {
        AXUIElementSetMessagingTimeout(root,0.2)
        var value:CFTypeRef?
        if AXUIElementCopyAttributeValue(root,kAXFocusedUIElementAttribute as CFString,&value) == .success,
           let value,CFGetTypeID(value) == AXUIElementGetTypeID() {
            let element=value as! AXUIElement
            var owner:pid_t=0
            if AXUIElementGetPid(element,&owner) == .success && owner == pid { return element }
        }
    }
    return nil
}
@MainActor func editableFocusedElement() -> AXUIElement? {
    guard let element=focusedElement(),insertionMetadata(element).editable else { return nil }
    return element
}
@MainActor enum FocusAccessibility {
    private static var prepared=Set<pid_t>()
    static func prepareIfNeeded() {
        guard AXIsProcessTrusted(),editableFocusedElement() == nil,let app=NSWorkspace.shared.frontmostApplication,
              !prepared.contains(app.processIdentifier),let bundle=app.bundleURL else { return }
        let frameworks=(try? FileManager.default.contentsOfDirectory(atPath:bundle.appendingPathComponent("Contents/Frameworks").path)) ?? []
        guard frameworks.contains(where: { $0.contains("Electron Framework") || $0.contains("Google Chrome Framework") || $0.contains("Chromium") }) else { return }
        let target=AXUIElementCreateApplication(app.processIdentifier)
        // Chromium/Electron expose web focus on demand. These are runtime AX modes,
        // not TCC grants; only the focused Chromium-based app is prepared once.
        let manual=AXUIElementSetAttributeValue(target,"AXManualAccessibility" as CFString,kCFBooleanTrue)
        let result=manual == .success ? manual : AXUIElementSetAttributeValue(target,"AXEnhancedUserInterface" as CFString,kCFBooleanTrue)
        if result == .success { prepared.insert(app.processIdentifier) }
    }
}

let hotKeyHandler: EventHandlerUPP = { _, event, context in
    guard let event, let context else { return OSStatus(eventNotHandledErr) }
    var identifier = EventHotKeyID()
    let result = GetEventParameter(event, EventParamName(kEventParamDirectObject),
        EventParamType(typeEventHotKeyID), nil, MemoryLayout<EventHotKeyID>.size, nil, &identifier)
    guard result == noErr, identifier.signature == 0x4c565850 else { return OSStatus(eventNotHandledErr) }
    let controller = Unmanaged<Controller>.fromOpaque(context).takeUnretainedValue()
    MainActor.assumeIsolated {
        if GetEventKind(event) == UInt32(kEventHotKeyReleased) { controller.binding.release() }
        else if controller.binding.press() { controller.toggle() }
    }
    return noErr
}

@MainActor final class Controller: NSObject, NSApplicationDelegate, NSMenuDelegate {
    var session = LVSession()
    let panel = IndicatorPanel()
    var statusItem: NSStatusItem!
    let binding = HotKeyBinding()
    var handler: EventHandlerRef?
    var timer: Timer?
    var originalElement: AXUIElement?
    var originalPID: pid_t?
    var pendingText: String?
    var useCurrentField = UserDefaults.standard.bool(forKey: "currentField")
    var triggerChoice = UserDefaults.standard.data(forKey:"triggerChoice").flatMap { try? JSONDecoder().decode(TriggerChoice.self,from:$0) }
        ?? TriggerChoice.migrate(UserDefaults.standard.object(forKey:"shortcut") as? Int)
    var pendingPermissionTrigger:TriggerChoice?
    var modifierTrigger:ModifierTrigger?
    var sampleMode = CommandLine.arguments.contains("--smoke-cancel") || CommandLine.arguments.contains("--sample")
    let capture = Recorder()
    let transport = LocalHTTP()
    var operation: Task<Void, Never>?
    var meter: Timer?
    var captureReady = false
    var inserting = false
    lazy var paste = PasteTransaction(clipboard:SystemClipboard())
    var configuring = false
    lazy var updater=UpdateCoordinator(safety:{ [weak self] in
        guard let self else { return UpdateSafety(sessionBusy:true) }
        return UpdateSafety(sessionBusy:self.session.state != LV_IDLE || self.capture.isRecording,
            pasteBusy:self.inserting || self.paste.active,pendingText:self.pendingText != nil,settingsOpen:self.configuring)
    },changed:{ [weak self] in self?.updateMenuIndicator() })
    let diagnostics = DiagnosticLog.shared
    lazy var settings = SettingsController(log: diagnostics,
        discover: { [transport] endpoint in try await ModelDiscovery(transport: transport).load(endpoint: endpoint) },
        readVersion: { [transport] endpoint in await ModelDiscovery(transport: transport).version(endpoint: endpoint) })
    var config = UserDefaults.standard.data(forKey: "dictationConfig").flatMap { try? JSONDecoder().decode(DictationConfig.self, from: $0) } ?? .empty
    let sample = "LemonVox prototype: send Alex the 14 invoices on Friday."

    override init() {
        super.init(); lv_init(&session)
        panel.onCancel = { [weak self] in
            guard let self else { return }
            if self.session.state == LV_IDLE && !self.inserting { self.pendingPermissionTrigger=nil; self.panel.orderOut(nil) }
            else { self.cancel() }
        }
        panel.onStop = { [weak self] in
            guard let self, self.session.state == LV_RECORDING else { return }
            self.toggle()
        }
        panel.onOpenSettings = { [weak self] in self?.openAccessibilitySettings() }
        panel.onRetryPermission = { [weak self] in self?.retryPermissionTrigger() }
        applyAppearance()
    }
    func applyAppearance() {
        panel.appearance = UserDefaults.standard.bool(forKey: "overlayAlwaysDark") ? NSAppearance(named: .darkAqua) : nil
    }
    @objc func changeAppearance() {
        let dark = !UserDefaults.standard.bool(forKey: "overlayAlwaysDark")
        UserDefaults.standard.set(dark, forKey: "overlayAlwaysDark")
        applyAppearance(); refreshMenu()
    }
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.prohibited)
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        statusItem.button?.image = LemonIcon.image(size:20,template:true)
        statusItem.button?.setAccessibilityLabel("LemonVox")
        statusItem.button?.toolTip="LemonVox"
        if !CommandLine.arguments.dropFirst().contains(where: { $0.hasPrefix("--") }) { updater.start() }
        var specs = [
            EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed)),
            EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyReleased))
        ]
        let status = InstallEventHandler(GetApplicationEventTarget(), hotKeyHandler, specs.count, &specs,
            Unmanaged.passUnretained(self).toOpaque(), &handler)
        if status == noErr { _ = installTrigger(triggerChoice) }
        else { panel.show("Shortcut handler unavailable (\(status)); use LV menu") }
        refreshMenu()
        diagnostics.record(.appStarted)
        if CommandLine.arguments.contains("--smoke-menu") {
            let menu=NSMenu(); menu.autoenablesItems=false
            menuNeedsUpdate(menu)
            precondition(menu.items.map(\.title) == ["Start Dictation","","Settings…","","Quit LemonVox"])
            precondition(statusItem.button?.image?.isTemplate == true)
            _=lv_toggle(&session); menuNeedsUpdate(menu)
            precondition(menu.items.first?.title == "Stop Dictation" && menu.item(withTitle:"Cancel") != nil)
            _=lv_toggle(&session); menuNeedsUpdate(menu); precondition(menu.items.first?.isEnabled == false)
            cancel(); pendingText="synthetic pending text"; menuNeedsUpdate(menu)
            precondition(menu.item(withTitle:"Copy Text") != nil && menu.item(withTitle:"Cancel") == nil)
            pendingText=nil
            print("PASS: everyday menu, contextual Cancel/Copy, processing disabled, template accessible icon")
            Timer.scheduledTimer(withTimeInterval:0.1,repeats:false) { _ in NSApp.terminate(nil) }
        } else if CommandLine.arguments.contains("--smoke-focus-change") || CommandLine.arguments.contains("--smoke-focus-cancel") {
            originalElement=nil; originalPID = -1
            let generation=lv_toggle(&session)
            beginRealRecording(generation)
            if CommandLine.arguments.contains("--smoke-focus-cancel") { cancel() }
            Timer.scheduledTimer(withTimeInterval:0.7,repeats:false) { [weak self] _ in
                MainActor.assumeIsolated {
                    guard let self else { return }
                    precondition(self.session.state == LV_IDLE && !self.capture.isRecording && !self.captureReady)
                    print("PASS: focus warmup cancellation leaves session idle with no capture")
                    self.cancel(); NSApp.terminate(nil)
                }
            }
        } else if CommandLine.arguments.contains("--smoke-canceled-completion") {
            let generation=session.generation
            operation=Task { [weak self] in guard let self else { return }; await self.complete(self.sample,generation:generation) }
            cancel()
            Timer.scheduledTimer(withTimeInterval:0.2,repeats:false) { [weak self] _ in
                MainActor.assumeIsolated {
                    guard let self else { return }
                    precondition(self.pendingText == nil && !self.inserting && !self.panel.isVisible)
                    print("PASS: queued canceled completion does not resurrect text, UI or paste")
                    NSApp.terminate(nil)
                }
            }
        } else if CommandLine.arguments.contains("--settings") {
            configure()
        } else if CommandLine.arguments.contains("--smoke-settings") {
            guard let entry=statusItem.menu?.item(withTitle:"Settings…"), let action=entry.action else { fatalError("Settings menu missing") }
            precondition(NSApp.sendAction(action,to:entry.target,from:entry))
            precondition(settings.window.isVisible && configuring && !capture.isRecording)
            Timer.scheduledTimer(withTimeInterval:2,repeats:false) { [weak self] _ in
                MainActor.assumeIsolated {
                    guard let self else { return }
                    precondition(self.settings.window.isVisible && self.configuring && !self.capture.isRecording)
                    print("PASS: Settings menu opens native pane immediately, async discovery active/completed; no recording")
                    self.settings.window.close()
                    precondition(!self.configuring)
                    NSApp.terminate(nil)
                }
            }
        } else if CommandLine.arguments.contains("--smoke-show") {
            runShowCheck()
        } else if CommandLine.arguments.contains("--smoke-config-guard") {
            sampleMode = true; configuring = true; toggle()
            let blocked = session.state == LV_IDLE && !captureReady
            cancel()
            print("\(blocked ? "PASS" : "FAIL"): configuration blocks recording hotkey")
            exit(blocked ? 0 : 1)
        } else if CommandLine.arguments.contains("--smoke-cancel") {
            toggle()
            panel.overlay.cancelButton.performClick(nil)
            precondition(session.state == LV_IDLE)
            toggle()
            panel.overlay.stopButton.performClick(nil)
            precondition(session.state == LV_PROCESSING)
            panel.overlay.cancelButton.performClick(nil)
            Timer.scheduledTimer(withTimeInterval: 1, repeats: false) { [weak self] _ in
                MainActor.assumeIsolated {
                    guard let self else { fatalError("Controller released early") }
                    precondition(self.session.state == LV_IDLE && self.pendingText == nil && self.originalElement == nil)
                    print("PASS: overlay cancel in recording and processing; no late completion")
                    NSApp.terminate(nil)
                }
            }
        } else if CommandLine.arguments.contains("--show") {
            showSamplePreview()
        } else if CommandLine.arguments.contains("--smoke-controller") {
            print("Controller launched: handler=\(handler != nil) hotkey=\(binding.reference != nil) menu=\(statusItem.button != nil)")
            Timer.scheduledTimer(withTimeInterval: 1, repeats: false) { _ in NSApp.terminate(nil) }
        }
    }
    func runShowCheck() {
        let generation=session.generation
        pendingText="pending test result"
        showSamplePreview()
        precondition(!panel.isVisible, "Sample menu must not show listening overlay")
        precondition(session.state == LV_IDLE && session.generation == generation)
        precondition(!capture.isRecording && pendingText == "pending test result")
        Timer.scheduledTimer(withTimeInterval: 2, repeats: false) { _ in
            MainActor.assumeIsolated {
                let running = NSRunningApplication.runningApplications(withBundleIdentifier: "com.itomek.lemonvox.keyboardtest")
                precondition(!running.isEmpty, "Menu did not launch existing test app")
                print("PASS: menu launches Field A / Field B test app; overlay hidden; dictation idle")
                NSApp.terminate(nil)
            }
        }
    }
    func installTrigger(_ choice:TriggerChoice) -> Bool {
        switch choice {
        case .combo(let key,let modifiers):
            let result=binding.replace(key:key,modifiers:modifiers)
            guard result == noErr else { panel.show("Shortcut unavailable (\(result)); previous retained"); return false }
            modifierTrigger?.stop(); modifierTrigger=nil
        case .doubleTap(let key):
            guard AXIsProcessTrusted() else {
                pendingPermissionTrigger=choice
                let fallback = binding.reference != nil || modifierTrigger != nil ? "The previous trigger stays active." : "Start dictation from the menu meanwhile."
                panel.showPermission("Enable LemonVox in Accessibility, then Check Again. " + fallback)
                return false
            }
            let candidate=ModifierTrigger(key:key,allowed:{ [weak self] in
                guard let self else { return false }; return !self.configuring && !self.inserting
            },fire:{ [weak self] in self?.toggle() })
            guard candidate.start() else { candidate.stop(); panel.show("Modifier trigger unavailable; previous trigger retained. Try again in Settings."); return false }
            modifierTrigger?.stop(); modifierTrigger=candidate; binding.clear()
        }
        pendingPermissionTrigger=nil; triggerChoice=choice; return true
    }
    func openAccessibilitySettings() {
        // User action only. macOS owns the permission dialog and switch.
        let options=[kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String:true] as CFDictionary
        _ = AXIsProcessTrustedWithOptions(options)
        guard let url=URL(string:"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"),
              NSWorkspace.shared.open(url) else {
            panel.showPermission("Open System Settings → Privacy & Security → Accessibility. Enable LemonVox, then Check Again.")
            return
        }
    }
    func retryPermissionTrigger() {
        guard session.state == LV_IDLE && !inserting, let choice=pendingPermissionTrigger else { return }
        guard AXIsProcessTrusted() else {
            panel.showPermission("Accessibility is still off. Enable the current LemonVox app; relaunch if macOS requires it.")
            return
        }
        // A failed Settings save remains a draft. Permission recovery must not
        // commit that draft independently of the Settings Save action.
        if configuring {
            panel.show("Accessibility available · return to Settings and Save")
            return
        }
        guard installTrigger(choice) else { return }
        if let data=try? JSONEncoder().encode(choice) { UserDefaults.standard.set(data,forKey:"triggerChoice") }
        panel.show("Accessibility available · trigger ready")
    }
    func item(_ title: String, _ action: Selector) -> NSMenuItem {
        let entry = NSMenuItem(title: title, action: action, keyEquivalent: "")
        entry.target = self
        return entry
    }
    func refreshMenu() {
        let menu=NSMenu(); menu.autoenablesItems=false; menu.delegate=self; menuNeedsUpdate(menu); statusItem.menu=menu
    }
    func menuNeedsUpdate(_ menu:NSMenu) {
        menu.removeAllItems()
        let active=session.state != LV_IDLE || inserting
        let action=item(session.state == LV_RECORDING ? "Stop Dictation" : "Start Dictation",#selector(toggle))
        action.isEnabled = !configuring && !inserting && session.state != LV_PROCESSING
        menu.addItem(action)
        if active { menu.addItem(item("Cancel",#selector(cancel))) }
        if pendingText != nil && !inserting { menu.addItem(item("Copy Text",#selector(copyPending))) }
        menu.addItem(.separator())
        let settingsItem=item("Settings…",#selector(configure)); settingsItem.keyEquivalent=","; menu.addItem(settingsItem)
        menu.addItem(item(updater.menuTitle,#selector(checkForUpdates)))
        menu.addItem(.separator()); menu.addItem(item("Quit LemonVox",#selector(quit)))
    }
    func updateMenuIndicator() {
        statusItem?.button?.title=updater.updateAvailable ? " •" : ""
        statusItem?.button?.toolTip=updater.updateAvailable ? "LemonVox · Update available" : "LemonVox"
    }
    @objc func checkForUpdates() { updater.check() }
    @objc func showSamplePreview() {
        let url = Bundle.main.bundleURL.appendingPathComponent("Contents/Resources/LemonVox Keyboard Test.app")
        let configuration = NSWorkspace.OpenConfiguration()
        configuration.activates = true
        NSWorkspace.shared.openApplication(at: url, configuration: configuration) { _, error in
            if let error { NSLog("Sample test window could not open: %@", error.localizedDescription) }
        }
    }
    @objc func configure() {
        // Finish the explicit updater interaction before opening another editor.
        // Normal Settings/dictation behavior remains unchanged.
        if updater.blocksNewDictation { updater.check(); return }
        settings.updatePreferences={ [weak self] in (self?.updater.automaticChecks ?? true,self?.updater.automaticInstallation ?? false) }
        settings.saveUpdatePreferences={ [weak self] checks,install in
            self?.updater.automaticChecks=checks;self?.updater.automaticInstallation=install
        }
        if configuring { settings.window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps:true); return }
        cancel(); modifierTrigger?.reset(); configuring=true
        settings.showPreferences(trigger:triggerChoice,dark:UserDefaults.standard.bool(forKey:"overlayAlwaysDark"),current:useCurrentField) { [weak self] choice,dark,current in
            guard let self else { return }
            if choice != self.triggerChoice || (self.binding.reference == nil && self.modifierTrigger == nil) {
                guard self.installTrigger(choice) else { throw TriggerError.unavailable }
            }
            UserDefaults.standard.set(try JSONEncoder().encode(choice),forKey:"triggerChoice")
            UserDefaults.standard.set(dark,forKey:"overlayAlwaysDark"); UserDefaults.standard.set(current,forKey:"currentField")
            self.useCurrentField=current; self.applyAppearance()
        }
        settings.show(config:config, save: { [weak self] updated in
            guard let self else { return }
            UserDefaults.standard.set(try JSONEncoder().encode(updated), forKey:"dictationConfig")
            self.config=updated
        }, close: { [weak self] in
            guard let self else { return }
            self.configuring=false
            if self.pendingPermissionTrigger != nil { self.panel.orderOut(nil) }
            self.pendingPermissionTrigger=nil
        })
    }
    @objc func toggle() {
        guard !configuring, !inserting, !updater.blocksNewDictation else { return }
        if session.state == LV_PROCESSING { return }
        if session.state == LV_RECORDING && !sampleMode {
            if captureReady { stopRealRecording() }
            return
        }
        if session.state == LV_IDLE {
            if let element=focusedElement(),insertionMetadata(element).secure { panel.show("Secure field · dictation unavailable"); return }
            pendingText = nil
            originalPID = NSWorkspace.shared.frontmostApplication?.processIdentifier
            FocusAccessibility.prepareIfNeeded()
            originalElement = editableFocusedElement()
        }
        let previous = session.state
        let generation = lv_toggle(&session)
        panel.overlay.isSample = sampleMode
        if session.state == LV_RECORDING {
            if sampleMode { panel.present(.recording) }
            else { beginRealRecording(generation) }
        } else if previous == LV_RECORDING {
            panel.overlay.progressLabel = "PROCESSING SAMPLE"
            panel.present(.processing)
            timer = Timer.scheduledTimer(withTimeInterval: 0.7, repeats: false) { [weak self] _ in
                MainActor.assumeIsolated {
                    guard let self, lv_finish(&self.session, generation) != 0 else { return }
                    self.operation=Task { [weak self] in guard let self else { return }; await self.complete(self.sample,generation:generation) }
                }
            }
        }
    }
    func current(_ generation: UInt64) -> Bool {
        session.generation == generation && session.state != LV_IDLE
    }
    func beginRealRecording(_ generation: UInt64) {
        panel.overlay.progressLabel = "CHECKING LOCAL MODELS"
        panel.present(.processing)
        let selected = config
        operation = Task { [weak self] in
            guard let self else { return }
            do {
                if self.originalElement == nil {
                    for _ in 0..<8 {
                        try await Task.sleep(nanoseconds:60_000_000)
                        guard self.current(generation) else { return }
                        guard NSWorkspace.shared.frontmostApplication?.processIdentifier == self.originalPID else {
                            self.cancel(); self.panel.show("Target changed before recording · try again"); return
                        }
                        self.originalElement=editableFocusedElement()
                        if self.originalElement != nil { break }
                    }
                }
                let client = LemonadeClient(config: selected, transport: self.transport)
                try await client.validateModels()
                try Task.checkCancellation()
                guard self.current(generation) else { return }
                self.panel.overlay.progressLabel = "WAITING FOR MICROPHONE"
                self.panel.overlay.needsDisplay = true
                try await self.capture.start()
                try Task.checkCancellation()
                guard self.current(generation) else { self.capture.cancel(); return }
                self.diagnostics.record(.recordingStarted)
                self.captureReady = true
                self.panel.present(.recording)
                self.meter = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
                    MainActor.assumeIsolated {
                        guard let self, self.captureReady else { return }
                        self.panel.overlay.level = self.capture.level
                        self.panel.overlay.needsDisplay = true
                        if !self.capture.isRecording { self.stopRealRecording() }
                    }
                }
                if let meter = self.meter { RunLoop.main.add(meter, forMode: .common) }
            } catch is CancellationError { }
            catch { self.fail(error, generation: generation) }
        }
    }
    func stopRealRecording() {
        let generation = lv_toggle(&session)
        captureReady = false; meter?.invalidate(); meter = nil
        do {
            let wav = try capture.finish()
            let selected = config
            diagnostics.record(.transcriptionStarted)
            panel.overlay.progressLabel = "TRANSCRIBING"
            panel.present(.processing)
            operation = Task { [weak self] in
                guard let self else { return }
                do {
                    let text = try await LemonadeClient(config: selected, transport: self.transport).process(wav: wav) { [weak self] in
                        await MainActor.run {
                            guard let self, self.current(generation) else { return }
                            self.diagnostics.record(.cleanupStarted)
                            self.panel.overlay.progressLabel = "CLEANING UP"
                            self.panel.overlay.needsDisplay = true
                        }
                    }
                    guard lv_finish(&self.session, generation) != 0 else { return }
                    await self.complete(text,generation:generation)
                } catch is CancellationError { }
                catch { self.fail(error, generation: generation) }
            }
        } catch { fail(error, generation: generation) }
    }
    func fail(_ error: Error, generation: UInt64) {
        guard current(generation) else { return }
        diagnostics.record(.dictationFailed,error:error)
        cancel()
        panel.show(SafeDisplay.error(error))
    }
    func complete(_ text:String,generation:UInt64) async {
        guard session.generation == generation, !Task.isCancelled else { return }
        pendingText=text; inserting=true
        defer { inserting=false }
        guard AXIsProcessTrusted() else {
            diagnostics.record(.insertionFallback); panel.show("Accessibility needed · text ready to copy"); clearTarget(); return
        }
        guard let target=useCurrentField ? focusedElement() : originalElement else {
            diagnostics.record(.insertionFallback); panel.show("Target unavailable · text ready to copy"); clearTarget(); return
        }
        let metadata=insertionMetadata(target)
        guard metadata.editable else {
            diagnostics.record(.insertionFallback)
            panel.show(metadata.secure ? "Secure field · insertion stopped" : "Field unsupported · text ready to copy")
            clearTarget(); return
        }
        var pid:pid_t=0
        guard AXUIElementGetPid(target,&pid) == .success else { clearTarget(); panel.show("Target unavailable · text ready to copy"); return }
        let original=originalPID
        let currentPolicy=useCurrentField
        let result=await paste.insert(text,validate:{
            guard self.session.generation == generation, let current=focusedElement(),CFEqual(target,current) else { return false }
            let metadata=insertionMetadata(current)
            guard metadata.editable else { return false }
            return canInsert(currentTarget: currentPolicy, originalMatches: original == pid,
                frontmostMatches: NSWorkspace.shared.frontmostApplication?.processIdentifier == pid,
                secure: metadata.secure)
        },dispatch:{ postPaste(to:pid) })
        guard session.generation == generation, !Task.isCancelled else { return }
        clearTarget()
        switch result {
        case .dispatched:
            diagnostics.record(.pasteDispatched); pendingText=nil; panel.orderOut(nil)
        case .targetChanged:
            diagnostics.record(.insertionFallback); panel.show("Target changed · text ready to copy")
        case .unavailable:
            diagnostics.record(.insertionFallback); panel.show("Paste unavailable · text ready to copy")
        case .canceled: break
        }
    }
    func clearTarget() { originalElement = nil; originalPID = nil }
    @objc func cancel() {
        if session.state != LV_IDLE { diagnostics.record(.canceled) }
        operation?.cancel(); operation = nil
        meter?.invalidate(); meter = nil; capture.cancel(); captureReady = false
        timer?.invalidate(); timer = nil; lv_cancel(&session)
        clearTarget(); pendingText = nil; panel.orderOut(nil)
    }
    @objc func copyPending() {
        guard let pendingText else { panel.show("No pending text"); return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(pendingText, forType: .string)
        self.pendingText = nil; panel.orderOut(nil)
    }
    @objc func permissionStatus() {
        panel.show(AXIsProcessTrusted() ? "Accessibility available" : "Allow LemonVox Prototype in Accessibility")
    }
    @objc func quit() { cancel(); NSApp.terminate(nil) }
    func applicationShouldTerminate(_ sender:NSApplication) -> NSApplication.TerminateReply {
        guard updater.canTerminate else { return .terminateCancel }
        guard paste.active else { return .terminateNow }
        operation?.cancel()
        Task { [weak self] in
            guard let self else { return }
            while self.paste.active { try? await Task.sleep(nanoseconds:10_000_000) }
            NSApp.reply(toApplicationShouldTerminate:true)
        }
        return .terminateLater
    }
    func applicationWillTerminate(_ notification: Notification) {
        modifierTrigger?.stop(); cancel(); transport.close(); binding.clear()
        if let handler { RemoveEventHandler(handler) }
    }
}

@MainActor func probeWindowMatches(pid:pid_t,marker:String) -> Bool {
    guard !marker.isEmpty,NSWorkspace.shared.frontmostApplication?.processIdentifier == pid else { return false }
    var window:CFTypeRef?,title:CFTypeRef?
    guard AXUIElementCopyAttributeValue(AXUIElementCreateApplication(pid),kAXFocusedWindowAttribute as CFString,&window) == .success,
          let window,CFGetTypeID(window) == AXUIElementGetTypeID() else { return false }
    guard AXUIElementCopyAttributeValue(window as! AXUIElement,kAXTitleAttribute as CFString,&title) == .success else { return false }
    return (title as? String)?.contains(marker) == true
}
@MainActor func runInsertionProbe() {
    guard AXIsProcessTrusted(),CGPreflightPostEventAccess() else {
        print("BLOCKED: app-context Accessibility/event-post permission unavailable; no field or clipboard changed")
        return
    }
    guard let markerArg=CommandLine.arguments.first(where: { $0.hasPrefix("--probe-marker=") }) else { return }
    let marker=String(markerArg.dropFirst("--probe-marker=".count))
    Timer.scheduledTimer(withTimeInterval:0.1,repeats:false) { _ in
        Task { @MainActor in
            defer { NSApp.terminate(nil) }
            let front=NSWorkspace.shared.frontmostApplication
            guard ["com.apple.TextEdit","com.google.Chrome"].contains(front?.bundleIdentifier ?? ""),let pid=front?.processIdentifier else { print("BLOCKED: disposable external app not focused"); return }
            guard probeWindowMatches(pid:pid,marker:marker) else { print("BLOCKED: probe document marker absent; no insertion"); return }
            FocusAccessibility.prepareIfNeeded()
            var focused=editableFocusedElement()
            for _ in 0..<8 where focused == nil {
                try? await Task.sleep(nanoseconds:60_000_000)
                guard NSWorkspace.shared.frontmostApplication?.processIdentifier == pid else { print("BLOCKED: target app changed"); return }
                focused=editableFocusedElement()
            }
            guard let target=focused else { print("BLOCKED: no native focused element after app/system focus query"); return }
            guard probeWindowMatches(pid:pid,marker:marker) else { print("BLOCKED: probe window changed during warmup"); return }
            let metadata=insertionMetadata(target)
            print("Probe role=\(metadata.role ?? "unknown") secure=\(metadata.secure) editable=\(metadata.editable)")
            guard metadata.editable else { print("BLOCKED: unsupported probe target"); return }
            var value:CFTypeRef?
            AXUIElementCopyAttributeValue(target,kAXValueAttribute as CFString,&value)
            guard (value as? String) == "" else { print("BLOCKED: probe field not verified empty; no overwrite"); return }
            let synthetic="LemonVox cross-app insertion probe."
            let validateProbe = {
                guard probeWindowMatches(pid:pid,marker:marker),let current=focusedElement() else { return false }
                return CFEqual(current,target) && insertionMetadata(current).editable
            }
            guard validateProbe() else { print("BLOCKED: probe target changed before dispatch"); return }
            if CommandLine.arguments.contains("--probe-ax") {
                let result=AXUIElementSetAttributeValue(target,kAXSelectedTextAttribute as CFString,synthetic as CFString)
                print("AX selected-text write result=\(result.rawValue)")
                try? await Task.sleep(nanoseconds:300_000_000)
            } else {
                let result=await PasteTransaction(clipboard:SystemClipboard()).insert(synthetic,validate:validateProbe,dispatch:{ postPaste(to:pid) })
                print("Paste probe dispatched=\(result == .dispatched)")
            }
            var after:CFTypeRef?
            AXUIElementCopyAttributeValue(target,kAXValueAttribute as CFString,&after)
            print("Disposable target matches synthetic text: \((after as? String) == synthetic)")
        }
    }
    NSApp.run()
}

let app = NSApplication.shared
if CommandLine.arguments.contains("--insertion-probe") {
    app.setActivationPolicy(.prohibited)
    MainActor.assumeIsolated { runInsertionProbe() }
} else if CommandLine.arguments.contains("--diagnose") {
    let panel = IndicatorPanel()
    precondition(!panel.canBecomeKey && !panel.canBecomeMain && !panel.ignoresMouseEvents)
    precondition(panel.styleMask.contains(.nonactivatingPanel))
    print("PASS: panel nonactivating, non-key, non-main, clickable controls")
    var diagnosticHotKey: EventHotKeyRef?
    let registration = RegisterEventHotKey(UInt32(kVK_ANSI_V), UInt32(cmdKey | optionKey),
        EventHotKeyID(signature: 0x4c565844, id: 1), GetApplicationEventTarget(), 0, &diagnosticHotKey)
    print("Hotkey registration status: \(registration) (0 means registered; key delivery still needs manual test)")
    if let diagnosticHotKey { UnregisterEventHotKey(diagnosticHotKey) }
    print("Accessibility trusted: \(AXIsProcessTrusted()); permission prompts: disabled")
    print("Event-post access: \(CGPreflightPostEventAccess())")
    print("Microphone authorization: \(AVCaptureDevice.authorizationStatus(for: .audio).rawValue); no request made")
} else if CommandLine.arguments.contains("--smoke-overlay") {
    app.setActivationPolicy(.prohibited)
    let panel = IndicatorPanel()
    let dark = CommandLine.arguments.contains("--dark")
    panel.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
    let before = NSWorkspace.shared.frontmostApplication?.processIdentifier
    panel.present(.recording)
    Timer.scheduledTimer(withTimeInterval: 1, repeats: false) { _ in
        let after = NSWorkspace.shared.frontmostApplication?.processIdentifier
        print("\(dark ? "Dark" : "Light") overlay frontmost PID before=\(before ?? -1) after=\(after ?? -1) unchanged=\(before == after)")
        panel.orderOut(nil); app.terminate(nil)
    }
    app.run()
} else {
    app.setActivationPolicy(.prohibited)
    let controller = MainActor.assumeIsolated { Controller() }
    app.delegate = controller
    withExtendedLifetime(controller) { app.run() }
}
