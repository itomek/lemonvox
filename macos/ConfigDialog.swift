import AppKit

enum RunningAppVersion {
    static func display(info: [String: Any]) -> String {
        func value(_ key: String) -> String {
            guard let text=info[key] as? String,
                  !text.trimmingCharacters(in:.whitespacesAndNewlines).isEmpty else { return "unavailable" }
            return text.trimmingCharacters(in:.whitespacesAndNewlines)
        }
        return "LemonVox \(value("CFBundleShortVersionString")) (build \(value("CFBundleVersion")))"
    }
}

@MainActor final class SettingsController: NSObject, NSWindowDelegate, NSTextFieldDelegate {
    let window: NSWindow
    let appVersion = NSTextField(labelWithString: RunningAppVersion.display(info: Bundle.main.infoDictionary ?? [:]))
    let scroll = NSScrollView()
    let trigger = TriggerControl(frame:.zero)
    let dark = NSButton(checkboxWithTitle:"Always use dark overlay",target:nil,action:nil)
    let automaticChecks=NSButton(checkboxWithTitle:"Check for updates automatically",target:nil,action:nil)
    let automaticInstallation=NSButton(checkboxWithTitle:"Download and install updates automatically",target:nil,action:nil)
    var updatePreferences:(() -> (Bool,Bool))?
    var saveUpdatePreferences:((Bool,Bool) -> Void)?
    let currentField = NSButton(checkboxWithTitle:"Insert into field selected at completion",target:nil,action:nil)
    var savePreferences: ((TriggerChoice,Bool,Bool) throws -> Void)?
    let endpoint = NSTextField()
    let transcription = ModelPicker()
    let cleanup = ModelPicker()
    let prompt = NSTextView()
    let status = NSTextField(wrappingLabelWithString: "")
    let version = NSTextField(labelWithString: "Tested with Lemonade 2026.44.1")
    let retry = NSButton(title: "Refresh", target: nil, action: nil)
    let save = NSButton(title: "Save", target: nil, action: nil)
    let logPath = NSTextField(wrappingLabelWithString: "")
    private let log: DiagnosticLog
    private let discover: (String) async throws -> DiscoveredModels
    private let readVersion: (String) async -> String?
    private var refreshTask: Task<Void, Never>?
    private var versionTask: Task<Void, Never>?
    private var generation = 0
    private var previousMenu: NSMenu?
    private var saveConfig: ((DictationConfig) throws -> Void)?
    private var didClose: (() -> Void)?
    var loading = false

    init(log: DiagnosticLog = .shared, discover: @escaping (String) async throws -> DiscoveredModels,
         readVersion: @escaping (String) async -> String? = { _ in nil }) {
        self.log = log; self.discover = discover; self.readVersion = readVersion
        window = NSWindow(contentRect: NSRect(x:0,y:0,width:640,height:690),
            styleMask:[.titled,.closable,.resizable],backing:.buffered,defer:false)
        super.init()
        window.title = "LemonVox Settings"; window.isReleasedWhenClosed = false; window.delegate = self
        window.minSize=NSSize(width:640,height:480)
        guard let root = window.contentView else { return }
        scroll.frame=NSRect(x:0,y:52,width:640,height:638); scroll.autoresizingMask=[.width,.height]
        scroll.hasVerticalScroller=true; scroll.drawsBackground=false
        let view=NSView(frame:NSRect(x:0,y:0,width:640,height:930)); scroll.documentView=view; root.addSubview(scroll)
        func label(_ text: String, _ y: CGFloat) {
            let label = NSTextField(labelWithString:text); label.frame = NSRect(x:24,y:y,width:592,height:20)
            label.font = .systemFont(ofSize:13,weight:.medium); view.addSubview(label)
        }
        label("OpenAI-compatible endpoint", 646)
        endpoint.frame = NSRect(x:24,y:614,width:444,height:26); endpoint.placeholderString = "http://127.0.0.1:13305/v1"
        endpoint.delegate = self; endpoint.target = self; endpoint.action = #selector(refresh)
        view.addSubview(endpoint)
        retry.frame = NSRect(x:476,y:612,width:140,height:30); retry.target = self; retry.action = #selector(refresh); view.addSubview(retry)
        status.frame = NSRect(x:24,y:558,width:592,height:48); status.font = .systemFont(ofSize:12); view.addSubview(status)
        version.frame = NSRect(x:24,y:536,width:592,height:18); version.textColor = .secondaryLabelColor; version.font = .systemFont(ofSize:11); view.addSubview(version)
        transcription.setAccessibilityLabel("Transcription model"); cleanup.setAccessibilityLabel("Cleanup model")
        label("Transcription model", 502); transcription.frame = NSRect(x:24,y:472,width:592,height:28); view.addSubview(transcription)
        label("Cleanup model", 438); cleanup.frame = NSRect(x:24,y:408,width:592,height:28); view.addSubview(cleanup)
        let routing = NSTextField(wrappingLabelWithString:"Models and provider credentials are managed in Lemonade. Remote cleanup choices send dictated text through Lemonade to that provider.")
        routing.frame = NSRect(x:24,y:362,width:592,height:38); routing.font = .systemFont(ofSize:11); routing.textColor = .secondaryLabelColor; view.addSubview(routing)
        label("Cleanup prompt", 334)
        let promptScroll = NSScrollView(frame:NSRect(x:24,y:228,width:592,height:100)); promptScroll.hasVerticalScroller=true; promptScroll.borderType = .bezelBorder
        prompt.frame = promptScroll.bounds; prompt.isRichText=false; prompt.font = .systemFont(ofSize:12); prompt.isVerticallyResizable=true; prompt.autoresizingMask=[.width]
        prompt.textContainer?.widthTracksTextView=true; promptScroll.documentView=prompt; view.addSubview(promptScroll)
        label("Diagnostics", 198)
        let openLog=NSButton(title:"Open Log",target:self,action:#selector(openLog))
        openLog.frame=NSRect(x:24,y:150,width:110,height:30); view.addSubview(openLog)
        logPath.frame=NSRect(x:24,y:104,width:592,height:36); logPath.font = .systemFont(ofSize:11); logPath.textColor = .secondaryLabelColor; logPath.isSelectable=true; view.addSubview(logPath)
        automaticChecks.frame=NSRect(x:24,y:900,width:592,height:24)
        automaticInstallation.frame=NSRect(x:24,y:872,width:592,height:24)
        view.addSubview(automaticChecks);view.addSubview(automaticInstallation)
        label("Dictation trigger", 820)
        trigger.frame=NSRect(x:24,y:752,width:592,height:62); view.addSubview(trigger)
        dark.frame=NSRect(x:24,y:720,width:280,height:24); view.addSubview(dark)
        currentField.frame=NSRect(x:24,y:692,width:592,height:24); view.addSubview(currentField)
        appVersion.frame=NSRect(x:24,y:22,width:486,height:18)
        appVersion.font = .systemFont(ofSize:11); appVersion.textColor = .secondaryLabelColor
        appVersion.isSelectable=true; appVersion.lineBreakMode = .byTruncatingMiddle
        appVersion.autoresizingMask=[.width]; appVersion.toolTip=appVersion.stringValue
        appVersion.setAccessibilityLabel("Running LemonVox version and build")
        root.addSubview(appVersion)
        save.target=self; save.action=#selector(saveSettings); save.bezelStyle = .rounded; save.keyEquivalent="\r"; save.frame=NSRect(x:526,y:16,width:90,height:30); root.addSubview(save)
    }
    func show(config: DictationConfig, save: @escaping (DictationConfig) throws -> Void, close: @escaping () -> Void) {
        if window.isVisible { window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps:true); return }
        saveConfig=save; didClose=close
        let updates=updatePreferences?() ?? (true,false)
        automaticChecks.state=updates.0 ? .on : .off
        automaticInstallation.state=updates.1 ? .on : .off
        endpoint.stringValue=config.endpoint; prompt.string=config.cleanupPrompt
        populate(transcription, models:[], selected:config.asrModel)
        populate(cleanup, models:[], selected:config.cleanupModel)
        previousMenu=NSApp.mainMenu; installEditingMenu()
        NSApp.setActivationPolicy(.accessory)
        scroll.contentView.scroll(to:NSPoint(x:0,y:max(0,930-scroll.contentView.bounds.height))); scroll.reflectScrolledClipView(scroll.contentView)
        window.center(); window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps:true)
        log.record(.settingsOpened); refreshLogs(); refresh()
    }
    private func selected(_ picker:ModelPicker) -> String { picker.selectedID }
    private func populate(_ picker:ModelPicker, models:[LocalModel], selected:String) {
        picker.update(choices:models.map { ModelChoice(id:$0.id,title:$0.id + ($0.cloud_provider == nil ? " · local" : " · remote")) },selected:selected)
    }
    func controlTextDidChange(_ notification: Notification) {
        guard notification.object as? NSTextField === endpoint else { return }
        generation += 1; refreshTask?.cancel(); versionTask?.cancel(); loading=false
        status.stringValue="Endpoint changed. Refresh models to discover available choices."
        version.stringValue="Tested with Lemonade 2026.44.1 · connected version not checked"
    }
    @objc func refresh() {
        refreshTask?.cancel(); versionTask?.cancel(); generation += 1
        let current=generation; let base=endpoint.stringValue.trimmingCharacters(in:.whitespacesAndNewlines)
        loading=true; status.stringValue="Loading models from Lemonade…"; retry.title="Refresh"
        version.stringValue="Tested with Lemonade 2026.44.1 · checking connected version…"
        log.record(.discoveryStarted)
        refreshTask=Task { [weak self] in
            guard let self else { return }
            do {
                let models=try await self.discover(base)
                guard !Task.isCancelled, self.generation == current, self.window.isVisible else { return }
                self.loading=false
                let asr=self.selected(self.transcription), chat=self.selected(self.cleanup)
                self.populate(self.transcription,models:models.transcription,selected:asr)
                self.populate(self.cleanup,models:models.cleanup,selected:chat)
                self.status.stringValue=models.transcription.isEmpty || models.cleanup.isEmpty
                    ? (models.reason ?? "No compatible models for one or both tasks. Lemonade supplied no reason; existing choices are preserved.")
                    : "Models available. Choose transcription and cleanup independently."
                self.status.toolTip=self.status.stringValue
                self.log.record(.discoverySucceeded); self.refreshLogs()
            } catch {
                guard !Task.isCancelled, self.generation == current, self.window.isVisible else { return }
                self.loading=false; self.status.stringValue=SafeDisplay.error(error); self.status.toolTip=self.status.stringValue
                self.log.record(.discoveryFailed,error:error); self.refreshLogs()
            }
        }
        versionTask=Task { [weak self] in
            guard let self else { return }
            let actual=await self.readVersion(base)
            guard !Task.isCancelled, self.generation == current, self.window.isVisible else { return }
            self.version.stringValue="Tested with Lemonade 2026.44.1 · connected: " + (actual ?? "unavailable")
        }
    }
    func showPreferences(trigger choice:TriggerChoice,dark:Bool,current:Bool,save:@escaping (TriggerChoice,Bool,Bool) throws -> Void) {
        trigger.update(choice); self.dark.state=dark ? .on : .off; currentField.state=current ? .on : .off
        savePreferences=save
    }
    @objc func saveSettings() {
        let config=DictationConfig(endpoint:endpoint.stringValue.trimmingCharacters(in:.whitespacesAndNewlines),
            asrModel:selected(transcription),cleanupModel:selected(cleanup),cleanupPrompt:prompt.string)
        do { trigger.cancelRecording(); _ = try config.baseURL(); try savePreferences?(trigger.choice,dark.state == .on,currentField.state == .on); try saveConfig?(config); saveUpdatePreferences?(automaticChecks.state == .on,automaticInstallation.state == .on); log.record(.configSaved); window.close() }
        catch { status.stringValue=SafeDisplay.error(error); status.toolTip=status.stringValue }
    }
    @objc func refreshLogs() {
        logPath.stringValue=(log.problem.map { $0 + " · " } ?? "") + log.file.path
    }
    @objc func openLog() {
        if !NSWorkspace.shared.open(log.file) { status.stringValue="Could not open diagnostic log"; status.toolTip=status.stringValue }
    }
    func windowDidResignKey(_ notification:Notification) { trigger.cancelRecording() }
    func windowWillClose(_ notification: Notification) {
        trigger.cancelRecording(); savePreferences=nil
        transcription.popover.close(); cleanup.popover.close()
        generation += 1; refreshTask?.cancel(); versionTask?.cancel(); loading=false
        NSApp.mainMenu=previousMenu; NSApp.setActivationPolicy(.prohibited)
        didClose?(); didClose=nil; saveConfig=nil
    }
    private func installEditingMenu() {
        let menu=NSMenu(); let app=NSMenuItem(); app.submenu=NSMenu(title:"LemonVox")
        let close=NSMenuItem(title:"Close Settings",action:#selector(NSWindow.performClose(_:)),keyEquivalent:"w"); close.target=window
        app.submenu?.addItem(close); menu.addItem(app)
        let edit=NSMenuItem(title:"Edit",action:nil,keyEquivalent:""); let submenu=NSMenu(title:"Edit")
        for (title,selector,key) in [("Undo","undo:","z"),("Cut","cut:","x"),("Copy","copy:","c"),("Paste","paste:","v"),("Select All","selectAll:","a")] {
            submenu.addItem(NSMenuItem(title:title,action:Selector(selector),keyEquivalent:key))
        }
        edit.submenu=submenu; menu.addItem(edit); NSApp.mainMenu=menu
    }
}
