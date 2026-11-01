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
    // LVSession remains only for the explicitly synthetic sample/smoke paths.
    // Production dictation is owned by WorkflowBridge and WorkflowNativeAdapter.
    var sampleSession = LVSession()
    let panel = IndicatorPanel()
    var statusItem: NSStatusItem!
    let binding = HotKeyBinding()
    var handler: EventHandlerRef?
    var sampleTimer: Timer?
    var sampleOperation: Task<Void, Never>?
    var pendingText: String?
    var useCurrentField = UserDefaults.standard.bool(forKey: "currentField")
    var triggerChoice = UserDefaults.standard.data(forKey:"triggerChoice").flatMap { try? JSONDecoder().decode(TriggerChoice.self,from:$0) }
        ?? TriggerChoice.migrate(UserDefaults.standard.object(forKey:"shortcut") as? Int)
    var pendingPermissionTrigger:TriggerChoice?
    var modifierTrigger:ModifierTrigger?
    let syntheticRun = CommandLine.arguments.contains("--sample") || CommandLine.arguments.contains(where: { $0.hasPrefix("--smoke-") })
    var sampleMode = CommandLine.arguments.contains("--smoke-cancel") || CommandLine.arguments.contains("--sample")
    let capture = Recorder()
    let transport = LocalHTTP()
    var meter: Timer?
    var inserting = false
    lazy var paste = PasteTransaction(clipboard:SystemClipboard())
    lazy var workflowTargeting = SystemWorkflowTargeting(
        trusted: { AXIsProcessTrusted() },
        focused: { focusedElement() },
        metadata: { element in
            let metadata = insertionMetadata(element)
            return WorkflowNativeTargetMetadata(editable: metadata.editable, secure: metadata.secure)
        },
        frontmost: { NSWorkspace.shared.frontmostApplication?.processIdentifier },
        dispatch: { postPaste(to: $0) }
    )
    lazy var workflowAdapter = WorkflowNativeAdapter(capture: capture, transport: transport,
        paste: paste, targeting: workflowTargeting)
    lazy var workflow: WorkflowBridge = {
        var callbacks = WorkflowBridgeCallbacks()
        callbacks.phaseChanged = { [weak self] operation, phase in self?.workflowPhaseChanged(operation, phase) }
        callbacks.textReady = { [weak self] operation, text in self?.workflowTextReady(operation, text) }
        callbacks.failed = { [weak self] operation, failure in self?.workflowFailed(operation, failure) }
        callbacks.completed = { [weak self] operation, outcome in self?.workflowCompleted(operation, outcome) }
        callbacks.diagnostic = { [weak self] event, error, status in self?.workflowDiagnostic(event, error, status) }
        let bridge = WorkflowBridge(native: workflowAdapter, callbacks: callbacks)
        workflowAdapter.attach(bridge)
        workflowAdapter.onCaptureWillStart = { [weak self] in self?.workflowWaitingForMicrophone() }
        return bridge
    }()
    var workflowCreated = false
    var launchTracker = WorkflowLaunchTracker()
    var pendingLaunchEpoch: UInt64?
    var preflightTask: Task<Void, Never>?
    // Smoke-only staged facts enter the same production warmup path without
    // querying AX or starting a provider/capture request.
    var syntheticPreflight: (originalPID: pid_t?, frontmostPID: pid_t?)?
    var configuring = false
    lazy var updater=UpdateCoordinator(safety:{ [weak self] in
        guard let self else { return UpdateSafety(sessionBusy:true) }
        return UpdateSafety(sessionBusy:self.workflowBusy || self.sampleSession.state != LV_IDLE,
            pasteBusy:self.inserting || self.paste.active,pendingText:self.pendingText != nil,settingsOpen:self.configuring)
    },changed:{ [weak self] in self?.updateMenuIndicator() })
    let diagnostics = DiagnosticLog.shared
    lazy var settings: SettingsController = {
        let synthetic = syntheticRun
        return SettingsController(log: diagnostics,
            discover: { [transport] endpoint in
                if synthetic {
                    return DiscoveredModels(
                        transcription: [LocalModel(id: "synthetic-asr", downloaded: true, recipe: "whispercpp", labels: ["transcription"], cloud_provider: nil)],
                        cleanup: [LocalModel(id: "synthetic-cleanup", downloaded: true, recipe: "llamacpp", labels: ["chat"], cloud_provider: nil)],
                        reason: nil, version: "synthetic")
                }
                return try await ModelDiscovery(transport: transport).load(endpoint: endpoint)
            },
            readVersion: { [transport] endpoint in
                synthetic ? "synthetic" : await ModelDiscovery(transport: transport).version(endpoint: endpoint)
            })
    }()
    var config = UserDefaults.standard.data(forKey: "dictationConfig").flatMap { try? JSONDecoder().decode(DictationConfig.self, from: $0) } ?? .empty
    let sample = "LemonVox prototype: send Alex the 14 invoices on Friday."

    override init() {
        super.init(); lv_init(&sampleSession)
        panel.onCancel = { [weak self] in
            guard let self else { return }
            if !self.workflowBusy && self.sampleSession.state == LV_IDLE && !self.inserting { self.pendingPermissionTrigger=nil; self.panel.orderOut(nil) }
            else { self.cancel() }
        }
        panel.onStop = { [weak self] in
            guard let self, self.sampleSession.state == LV_RECORDING || self.launchTracker.phase == LV_WORKFLOW_PHASE_RECORDING else { return }
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
            _=lv_toggle(&sampleSession); menuNeedsUpdate(menu)
            precondition(menu.items.first?.title == "Stop Dictation" && menu.item(withTitle:"Cancel") != nil)
            _=lv_toggle(&sampleSession); menuNeedsUpdate(menu); precondition(menu.items.first?.isEnabled == false)
            cancel(); pendingText="synthetic pending text"; menuNeedsUpdate(menu)
            precondition(menu.item(withTitle:"Copy Text") != nil && menu.item(withTitle:"Cancel") == nil)
            pendingText=nil
            print("PASS: everyday menu, contextual Cancel/Copy, processing disabled, template accessible icon")
            Timer.scheduledTimer(withTimeInterval:0.1,repeats:false) { _ in NSApp.terminate(nil) }
        } else if CommandLine.arguments.contains("--smoke-focus-change") || CommandLine.arguments.contains("--smoke-focus-cancel") {
            syntheticPreflight = (originalPID: 101, frontmostPID: CommandLine.arguments.contains("--smoke-focus-change") ? 202 : 101)
            beginWorkflow()
            if CommandLine.arguments.contains("--smoke-focus-cancel") { cancel() }
            Timer.scheduledTimer(withTimeInterval:0.7,repeats:false) { [weak self] _ in
                MainActor.assumeIsolated {
                    guard let self else { return }
                    precondition(!self.workflowBusy && !self.workflowCreated && !self.capture.isRecording)
                    print("PASS: synthetic focus warmup cancellation leaves workflow idle with no capture")
                    self.cancel(); NSApp.terminate(nil)
                }
            }
        } else if CommandLine.arguments.contains("--smoke-canceled-completion") {
            let epoch = launchTracker.begin()
            precondition(launchTracker.acceptPhase(operation: 77, phase: LV_WORKFLOW_PHASE_PREPARING))
            precondition(launchTracker.finishStart(operation: 77, epoch: epoch))
            cancel()
            workflowTextReady(77, sample)
            workflowCompleted(77, WorkflowBridgeOutcome(kind: LV_WORKFLOW_OUTCOME_COPY_FALLBACK,
                fallbackReason: LV_WORKFLOW_FALLBACK_TARGET_UNAVAILABLE))
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
            let blocked = sampleSession.state == LV_IDLE && !workflowBusy
            cancel()
            print("\(blocked ? "PASS" : "FAIL"): configuration blocks recording hotkey")
            exit(blocked ? 0 : 1)
        } else if CommandLine.arguments.contains("--smoke-cancel") {
            toggle()
            panel.overlay.cancelButton.performClick(nil)
            precondition(sampleSession.state == LV_IDLE)
            toggle()
            panel.overlay.stopButton.performClick(nil)
            precondition(sampleSession.state == LV_PROCESSING)
            panel.overlay.cancelButton.performClick(nil)
            Timer.scheduledTimer(withTimeInterval: 1, repeats: false) { [weak self] _ in
                MainActor.assumeIsolated {
                    guard let self else { fatalError("Controller released early") }
                    precondition(self.sampleSession.state == LV_IDLE && self.pendingText == nil && !self.workflowBusy)
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
        let generation=sampleSession.generation
        pendingText="pending test result"
        showSamplePreview()
        precondition(!panel.isVisible, "Sample menu must not show listening overlay")
        precondition(sampleSession.state == LV_IDLE && sampleSession.generation == generation)
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
        guard !workflowBusy && sampleSession.state == LV_IDLE && !inserting, let choice=pendingPermissionTrigger else { return }
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
        let sampleActive = sampleSession.state != LV_IDLE
        let active = sampleActive || workflowBusy || inserting
        let recording = sampleSession.state == LV_RECORDING || launchTracker.phase == LV_WORKFLOW_PHASE_RECORDING
        let action=item(recording ? "Stop Dictation" : "Start Dictation",#selector(toggle))
        action.isEnabled = !configuring && !inserting && !workflowBusy && sampleSession.state != LV_PROCESSING
        if recording { action.isEnabled = !configuring && !inserting }
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
        if sampleMode {
            toggleSample()
            return
        }
        if launchTracker.phase == LV_WORKFLOW_PHASE_RECORDING, launchTracker.activeOperation != nil {
            _ = workflow.stop(boundary: "LemonVox-\(UUID().uuidString)")
            return
        }
        guard !workflowBusy else { return }
        beginWorkflow()
    }

    private var workflowBusy: Bool {
        preflightTask != nil || launchTracker.activeOperation != nil || inserting ||
            (workflowCreated && workflowAdapter.isRecording)
    }

    private func workflowConfiguration() -> WorkflowBridgeConfig {
        let components = URLComponents(string: config.endpoint)
        let url = components?.url
        let endpoint = WorkflowBridgeEndpointFacts(
            canonicalBaseURL: url?.absoluteString ?? config.endpoint,
            schemeAllowed: ["http", "https"].contains(components?.scheme ?? ""),
            hostLoopback: ["127.0.0.1", "localhost", "::1", "[::1]"].contains(components?.host ?? ""),
            noUserinfo: components?.user == nil && components?.password == nil,
            noQueryFragment: components?.query == nil && components?.fragment == nil,
            pathEndsV1: components?.path.split(separator: "/").last == "v1")
        return WorkflowBridgeConfig(endpoint: endpoint, preferenceEndpoint: config.endpoint,
            asrModel: config.asrModel, cleanupModel: config.cleanupModel,
            cleanupPrompt: config.cleanupPrompt, insertCurrentTarget: useCurrentField)
    }

    private func editableTargetForStart() -> WorkflowNativeTarget? {
        guard let target = workflowTargeting.focusedTarget() else { return nil }
        guard target.editable else {
            workflowTargeting.release(target)
            return nil
        }
        return target
    }

    private func beginWorkflow() {
        let stagedPreflight = syntheticPreflight
        if stagedPreflight == nil {
            if let element = focusedElement(), insertionMetadata(element).secure {
                panel.show("Secure field · dictation unavailable")
                return
            }
        }
        pendingText = nil
        // Snapshot before Chromium/Electron focus preparation so a changed
        // frontmost PID can only abort the warmup, never redefine its target.
        let originalPID = stagedPreflight?.originalPID ?? workflowTargeting.frontmostPID()
        if stagedPreflight == nil { FocusAccessibility.prepareIfNeeded() }
        let initialOriginal = stagedPreflight == nil ? editableTargetForStart() : nil
        let epoch = launchTracker.begin()
        pendingLaunchEpoch = epoch
        if let initialOriginal,
            originalPID != initialOriginal.pid || workflowTargeting.frontmostPID() != originalPID {
            workflowTargeting.release(initialOriginal)
            abortPreflight(epoch: epoch, message: "Target changed before recording · try again")
            return
        }
        panel.overlay.isSample = false
        panel.overlay.progressLabel = "CHECKING LOCAL MODELS"
        panel.present(.processing)
        preflightTask = Task { [weak self] in
            guard let self else { return }
            var original = initialOriginal
            var handedOff = false
            defer {
                if !handedOff, let original { self.workflowTargeting.release(original) }
                if self.pendingLaunchEpoch == epoch {
                    self.pendingLaunchEpoch = nil
                    self.preflightTask = nil
                }
            }
            guard !Task.isCancelled, self.pendingLaunchEpoch == epoch else { return }
            if original == nil {
                for _ in 0..<8 {
                    try? await Task.sleep(nanoseconds: 60_000_000)
                    guard !Task.isCancelled, self.pendingLaunchEpoch == epoch else { return }
                    let frontmostPID = stagedPreflight?.frontmostPID ?? self.workflowTargeting.frontmostPID()
                    guard frontmostPID == originalPID else {
                        self.abortPreflight(epoch: epoch, message: "Target changed before recording · try again")
                        return
                    }
                    let candidate = stagedPreflight == nil ? self.editableTargetForStart() : nil
                    if let candidate {
                        guard originalPID == candidate.pid,
                            self.workflowTargeting.frontmostPID() == originalPID else {
                            self.workflowTargeting.release(candidate)
                            self.abortPreflight(epoch: epoch, message: "Target changed before recording · try again")
                            return
                        }
                        original = candidate
                        break
                    }
                }
            }
            guard !Task.isCancelled, self.pendingLaunchEpoch == epoch else { return }
            self.workflowCreated = true
            self.workflowAdapter.prepareForStart(original: original, originalPID: originalPID, useCurrentField: self.useCurrentField)
            handedOff = true
            let operation = self.workflow.start(self.workflowConfiguration())
            let accepted = self.launchTracker.finishStart(operation: operation, epoch: epoch)
            if !accepted { self.workflowAdapter.finalizeOperation(operation) }
        }
    }

    private func abortPreflight(epoch: UInt64, message: String) {
        guard pendingLaunchEpoch == epoch else { return }
        launchTracker.cancel()
        pendingLaunchEpoch = nil
        preflightTask?.cancel()
        preflightTask = nil
        syntheticPreflight = nil
        diagnostics.record(.canceled)
        panel.show(message)
    }

    private func workflowWaitingForMicrophone() {
        guard launchTracker.activeOperation != nil else { return }
        panel.overlay.progressLabel = "WAITING FOR MICROPHONE"
        panel.overlay.needsDisplay = true
    }

    private func workflowPhaseChanged(_ operation: UInt64, _ phase: LVWorkflowPhase) {
        guard launchTracker.acceptPhase(operation: operation, phase: phase) else { return }
        switch phase {
        case LV_WORKFLOW_PHASE_PREPARING:
            panel.overlay.progressLabel = "CHECKING LOCAL MODELS"
            panel.present(.processing)
        case LV_WORKFLOW_PHASE_RECORDING:
            panel.present(.recording)
            startMeter()
        case LV_WORKFLOW_PHASE_TRANSCRIBING:
            stopMeter()
            panel.overlay.progressLabel = "TRANSCRIBING"
            panel.present(.processing)
        case LV_WORKFLOW_PHASE_CLEANING:
            panel.overlay.progressLabel = "CLEANING UP"
            panel.present(.processing)
        case LV_WORKFLOW_PHASE_INSERTING:
            inserting = true
            // Keep the existing cleaning/processing presentation through the
            // terminal outcome; insertion did not previously add a new label.
        case LV_WORKFLOW_PHASE_IDLE:
            stopMeter()
        default:
            break
        }
    }

    private func startMeter() {
        meter?.invalidate()
        meter = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, self.launchTracker.phase == LV_WORKFLOW_PHASE_RECORDING,
                    self.launchTracker.activeOperation != nil else { return }
                self.panel.overlay.level = self.workflowAdapter.level
                self.panel.overlay.needsDisplay = true
                if !self.workflowAdapter.isRecording {
                    _ = self.workflow.stop(boundary: "LemonVox-\(UUID().uuidString)")
                }
            }
        }
        if let meter { RunLoop.main.add(meter, forMode: .common) }
    }

    private func stopMeter() {
        meter?.invalidate()
        meter = nil
    }

    private func workflowTextReady(_ operation: UInt64, _ text: String) {
        guard launchTracker.acceptsSynchronousCallback(operation: operation) else { return }
        pendingText = text
    }

    private func workflowFailed(_ operation: UInt64, _ failure: WorkflowBridgeFailure) {
        guard launchTracker.acceptTerminal(operation: operation) else { return }
        stopMeter()
        inserting = false
        pendingText = nil
        workflowAdapter.finalizeOperation(operation)
        if failure.source == LV_WORKFLOW_FAILURE_ADAPTER,
            failure.adapterError == LV_WORKFLOW_ADAPTER_INSERTION_UNAVAILABLE {
            panel.show("Secure field · dictation unavailable")
        } else {
            panel.show(SafeDisplay.error(workflowError(failure)))
        }
    }

    private func workflowCompleted(_ operation: UInt64, _ outcome: WorkflowBridgeOutcome) {
        guard launchTracker.acceptTerminal(operation: operation) else { return }
        stopMeter()
        inserting = false
        workflowAdapter.finalizeOperation(operation)
        switch outcome.kind {
        case LV_WORKFLOW_OUTCOME_INSERTED:
            pendingText = nil
            panel.orderOut(nil)
        case LV_WORKFLOW_OUTCOME_COPY_FALLBACK:
            panel.show(fallbackMessage(outcome.fallbackReason))
        default:
            panel.show("Target unavailable · text ready to copy")
        }
    }

    private func workflowError(_ failure: WorkflowBridgeFailure) -> DictationError {
        if failure.source == LV_WORKFLOW_FAILURE_PROTOCOL {
            return CoreProtocolBridge.error(status: failure.protocolStatus, httpStatus: Int32(failure.httpStatus), detail: failure.safeDetail)
        }
        switch failure.adapterError {
        case LV_WORKFLOW_ADAPTER_UNAVAILABLE: return .microphone
        case LV_WORKFLOW_ADAPTER_INVALID_INPUT: return .recording
        case LV_WORKFLOW_ADAPTER_NETWORK: return .serverUnavailable
        case LV_WORKFLOW_ADAPTER_RESPONSE: return .response
        case LV_WORKFLOW_ADAPTER_INSERTION_UNAVAILABLE: return .response
        default: return .response
        }
    }

    private func fallbackMessage(_ reason: LVWorkflowFallbackReason) -> String {
        switch reason {
        case LV_WORKFLOW_FALLBACK_ACCESSIBILITY_UNAVAILABLE: return "Accessibility needed · text ready to copy"
        case LV_WORKFLOW_FALLBACK_TARGET_UNAVAILABLE: return "Target unavailable · text ready to copy"
        case LV_WORKFLOW_FALLBACK_TARGET_CHANGED: return "Target changed · text ready to copy"
        case LV_WORKFLOW_FALLBACK_SECURE_FIELD: return "Secure field · insertion stopped"
        case LV_WORKFLOW_FALLBACK_UNSUPPORTED_FIELD: return "Field unsupported · text ready to copy"
        case LV_WORKFLOW_FALLBACK_PASTE_UNAVAILABLE: return "Paste unavailable · text ready to copy"
        default: return "Target unavailable · text ready to copy"
        }
    }

    private func workflowDiagnostic(_ event: LVWorkflowDiagnosticEvent, _ error: LVWorkflowAdapterError, _ status: Int) {
        switch event {
        case LV_WORKFLOW_DIAGNOSTIC_RECORDING_STARTED: diagnostics.record(.recordingStarted)
        case LV_WORKFLOW_DIAGNOSTIC_TRANSCRIPTION_STARTED: diagnostics.record(.transcriptionStarted)
        case LV_WORKFLOW_DIAGNOSTIC_CLEANUP_STARTED: diagnostics.record(.cleanupStarted)
        case LV_WORKFLOW_DIAGNOSTIC_INSERTION_FALLBACK: diagnostics.record(.insertionFallback)
        case LV_WORKFLOW_DIAGNOSTIC_INSERTION_SUCCEEDED: diagnostics.record(.pasteDispatched)
        case LV_WORKFLOW_DIAGNOSTIC_CANCELED: diagnostics.record(.canceled)
        case LV_WORKFLOW_DIAGNOSTIC_TRANSPORT_FAILED, LV_WORKFLOW_DIAGNOSTIC_DICTATION_FAILED:
            diagnostics.record(.dictationFailed, error: diagnosticError(error, status: status))
        default:
            diagnostics.record(.dictationFailed, error: diagnosticError(error, status: status))
        }
    }

    private func diagnosticError(_ adapterError: LVWorkflowAdapterError, status: Int) -> Error? {
        if status != 0 { return DictationError.http(status) }
        switch adapterError {
        case LV_WORKFLOW_ADAPTER_NETWORK: return URLError(.cannotConnectToHost)
        case LV_WORKFLOW_ADAPTER_UNAVAILABLE: return DictationError.microphone
        case LV_WORKFLOW_ADAPTER_INVALID_INPUT: return DictationError.recording
        case LV_WORKFLOW_ADAPTER_RESPONSE, LV_WORKFLOW_ADAPTER_INSERTION_UNAVAILABLE: return DictationError.response
        case LV_WORKFLOW_ADAPTER_CANCELED: return CancellationError()
        default: return nil
        }
    }

    private func toggleSample() {
        if sampleSession.state == LV_PROCESSING { return }
        let previous = sampleSession.state
        let generation = lv_toggle(&sampleSession)
        panel.overlay.isSample = true
        if sampleSession.state == LV_RECORDING {
            panel.present(.recording)
        } else if previous == LV_RECORDING {
            panel.overlay.progressLabel = "PROCESSING SAMPLE"
            panel.present(.processing)
            sampleTimer = Timer.scheduledTimer(withTimeInterval: 0.7, repeats: false) { [weak self] _ in
                MainActor.assumeIsolated {
                    guard let self, lv_finish(&self.sampleSession, generation) != 0 else { return }
                    self.sampleOperation = Task { [weak self] in
                        await Task.yield()
                        guard let self, !Task.isCancelled,
                            self.sampleSession.generation == generation else { return }
                        self.pendingText = self.sample
                        self.panel.show("Sample text ready to copy")
                    }
                }
            }
        }
    }

    @objc func cancel() {
        let hadPreflight = preflightTask != nil || pendingLaunchEpoch != nil
        let hadSample = sampleSession.state != LV_IDLE
        let operation = launchTracker.activeOperation
        let activeCore = operation != nil && workflowCreated
        launchTracker.cancel()
        pendingLaunchEpoch = nil
        syntheticPreflight = nil
        preflightTask?.cancel(); preflightTask = nil
        if let operation, activeCore {
            workflow.cancel()
            workflowAdapter.finalizeOperation(operation)
        }
        if workflowCreated { workflowAdapter.discardPreparedStart() }
        stopMeter()
        sampleOperation?.cancel(); sampleOperation = nil
        sampleTimer?.invalidate(); sampleTimer = nil
        if hadSample { lv_cancel(&sampleSession) }
        // Core reports an active operation synchronously. A preflight or
        // synthetic sample has no core callback, so record only those here.
        if (hadPreflight && !activeCore) || hadSample { diagnostics.record(.canceled) }
        inserting = false; pendingText = nil; panel.orderOut(nil)
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
        cancel()
        guard workflowCreated && workflowAdapter.pasteActive else { return .terminateNow }
        Task { [weak self] in
            guard let self else { return }
            while self.workflowAdapter.pasteActive { try? await Task.sleep(nanoseconds:10_000_000) }
            NSApp.reply(toApplicationShouldTerminate:true)
        }
        return .terminateLater
    }
    func applicationWillTerminate(_ notification: Notification) {
        modifierTrigger?.stop(); cancel()
        if workflowCreated {
            workflow.close()
            workflowAdapter.prepareForShutdown()
        } else {
            // Settings discovery shares this transport without constructing a
            // workflow, so preserve the former unconditional shutdown path.
            transport.close()
        }
        binding.clear()
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
