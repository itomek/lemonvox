import AppKit

enum OverlayPhase { case recording, processing, message(String), permission(String) }

final class OverlayButton: NSButton {
    override var acceptsFirstResponder: Bool { false }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
}

final class ActivityView: NSView {
    var level = 0.0
    var sample = false
    var tick = 0.0
    override func draw(_ dirtyRect: NSRect) {
        let dark = effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
        (dark ? NSColor.systemYellow : NSColor.systemOrange).setFill()
        for i in 0..<7 {
            let amplitude = sample ? abs(sin(Double(i) * 0.8 + tick)) : level * (0.45 + 0.55 * abs(sin(Double(i) * 0.8)))
            let height = 4 + 18 * amplitude
            NSBezierPath(roundedRect: NSRect(x: Double(i) * 6 + 1, y: bounds.midY - height / 2,
                width: 3, height: height), xRadius: 1.5, yRadius: 1.5).fill()
        }
    }
}

final class OverlayView: NSView {
    let glass = NSGlassEffectView()
    let content = NSView()
    let statusLabel = NSTextField(labelWithString: "Listening…")
    let spinner = NSProgressIndicator()
    let activity = ActivityView()
    let cancelButton = OverlayButton()
    let stopButton = OverlayButton()
    let openSettingsButton = OverlayButton(title:"Open Settings…",target:nil,action:nil)
    let retryPermissionButton = OverlayButton(title:"Check Again",target:nil,action:nil)
    var onOpenSettings: (() -> Void)?
    var onRetryPermission: (() -> Void)?
    var onCancel: (() -> Void)?
    var onStop: (() -> Void)?
    var phase = OverlayPhase.recording { didSet { updateState() } }
    var tick = 0.0 { didSet { activity.tick = tick; activity.needsDisplay = true } }
    var isSample = true { didSet { activity.sample = isSample; updateState() } }
    var level = 0.0 { didSet { activity.level = min(1, max(0, level)); activity.needsDisplay = true } }
    var progressLabel = "Processing sample…" { didSet { updateState() } }
    private var animation: Timer?
    private var accessibilityObserver: NSObjectProtocol?

    override init(frame: NSRect) {
        super.init(frame: frame)
        glass.style = .regular
        glass.cornerRadius = 28
        glass.contentView = content
        addSubview(glass)
        statusLabel.font = .systemFont(ofSize: 13, weight: .medium)
        statusLabel.textColor = .labelColor
        statusLabel.lineBreakMode = .byTruncatingTail
        content.addSubview(statusLabel)
        spinner.style = .spinning; spinner.controlSize = .small
        spinner.isDisplayedWhenStopped = false
        content.addSubview(spinner); content.addSubview(activity)
        for (button, symbol, description, action) in [
            (cancelButton, "xmark", "Cancel dictation", #selector(cancel)),
            (stopButton, "stop.fill", "Finish recording", #selector(stop))
        ] {
            button.image = NSImage(systemSymbolName: symbol, accessibilityDescription: description)
            button.symbolConfiguration = NSImage.SymbolConfiguration(pointSize: 12, weight: .semibold)
            button.bezelStyle = .circular; button.isBordered = true
            button.target = self; button.action = action
            button.refusesFirstResponder = true
            button.toolTip = description; button.setAccessibilityLabel(description)
            content.addSubview(button)
        }
        for (button, action) in [(openSettingsButton, #selector(openSettings)), (retryPermissionButton, #selector(retryPermission))] {
            button.bezelStyle = .rounded; button.target = self; button.action = action
            button.refusesFirstResponder = true; button.setAccessibilityLabel(button.title)
            content.addSubview(button)
        }
        accessibilityObserver = NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.accessibilityDisplayOptionsDidChangeNotification, object: nil, queue: .main
        ) { [weak self] _ in
            self?.updateAccessibilityAppearance()
            self?.startAnimation()
        }
        activity.sample = isSample
        updateAccessibilityAppearance(); updateState()
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    deinit {
        animation?.invalidate()
        if let accessibilityObserver { NSWorkspace.shared.notificationCenter.removeObserver(accessibilityObserver) }
    }
    override func layout() {
        super.layout()
        glass.frame = bounds
        content.frame = bounds
        cancelButton.frame = NSRect(x: 12, y: bounds.midY - 16, width: 32, height: 32)
        stopButton.frame = NSRect(x: bounds.width - 44, y: bounds.midY - 16, width: 32, height: 32)
        activity.frame = NSRect(x: 56, y: bounds.midY - 14, width: 43, height: 28)
        spinner.frame = NSRect(x: 66, y: bounds.midY - 8, width: 16, height: 16)
        let message: Bool
        switch phase { case .message, .permission: message = true; default: message = false }
        statusLabel.maximumNumberOfLines = message ? 2 : 1
        statusLabel.lineBreakMode = message ? .byWordWrapping : .byTruncatingTail
        statusLabel.frame = NSRect(x: message ? 56 : 110, y: bounds.midY - (message ? 20 : 9),
            width: bounds.width - (message ? 72 : 164), height: message ? 40 : 20)
        if case .permission = phase {
            statusLabel.frame = NSRect(x:56,y:bounds.height-54,width:bounds.width-72,height:40)
            cancelButton.frame.origin.y = bounds.height-46
            openSettingsButton.frame = NSRect(x:56,y:12,width:142,height:28)
            retryPermissionButton.frame = NSRect(x:208,y:12,width:120,height:28)
        }
    }
    func updateAccessibilityAppearance() {
        applyTransparencyPreference(NSWorkspace.shared.accessibilityDisplayShouldReduceTransparency)
    }
    func applyTransparencyPreference(_ solid: Bool) {
        // Keep the content outside hidden glass when transparency is reduced.
        glass.isHidden = solid
        if solid {
            if content.superview !== self { glass.contentView = nil; addSubview(content) }
        } else if glass.contentView !== content { glass.contentView = content }
        wantsLayer = true
        layer?.cornerRadius = 28
        layer?.backgroundColor = solid ? NSColor.windowBackgroundColor.cgColor : NSColor.clear.cgColor
        needsLayout = true
    }
    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        updateAccessibilityAppearance()
    }
    func stopAnimation() { animation?.invalidate(); animation = nil; spinner.stopAnimation(nil) }
    func startAnimation() {
        stopAnimation()
        guard window?.isVisible == true else { return }
        switch phase { case .message, .permission: return; default: break }
        guard !NSWorkspace.shared.accessibilityDisplayShouldReduceMotion else { return }
        if case .processing = phase { spinner.startAnimation(nil) }
        animation = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            self?.tick += 0.25
        }
        if let animation { RunLoop.main.add(animation, forMode: .common) }
    }
    private func updateState() {
        let permission: Bool
        if case .permission = phase { permission = true } else { permission = false }
        openSettingsButton.isHidden = !permission; retryPermissionButton.isHidden = !permission
        switch phase {
        case .recording:
            statusLabel.stringValue = isSample ? "Sample recording" : "Listening…"
            activity.isHidden = false; spinner.isHidden = true
            stopButton.isHidden = false; stopButton.isEnabled = true
        case .processing:
            switch progressLabel.uppercased() {
            case "TRANSCRIBING": statusLabel.stringValue = "Transcribing…"
            case "CHECKING LOCAL MODELS": statusLabel.stringValue = "Checking models…"
            case "WAITING FOR MICROPHONE": statusLabel.stringValue = "Allow microphone…"
            case "CLEANING UP": statusLabel.stringValue = "Cleaning up…"
            default: statusLabel.stringValue = isSample ? "Sample cleanup…" : "Processing…"
            }
            activity.isHidden = true; spinner.isHidden = false
            stopButton.isHidden = false; stopButton.isEnabled = false
        case .message(let message), .permission(let message):
            statusLabel.stringValue = message
            activity.isHidden = true; spinner.isHidden = true
            stopButton.isHidden = true; stopButton.isEnabled = false
        }
        statusLabel.toolTip = statusLabel.stringValue
        needsLayout = true
        if window?.isVisible == true { startAnimation() }
    }
    @objc private func openSettings() { if case .permission = phase { onOpenSettings?() } }
    @objc private func retryPermission() { if case .permission = phase { onRetryPermission?() } }
    @objc private func cancel() { onCancel?() }
    @objc private func stop() { if stopButton.isEnabled { onStop?() } }
}

final class IndicatorPanel: NSPanel {
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
    let overlay = OverlayView(frame: NSRect(x: 0, y: 0, width: 280, height: 56))
    var onCancel: (() -> Void)? { didSet { overlay.onCancel = onCancel } }
    var onStop: (() -> Void)? { didSet { overlay.onStop = onStop } }
    var onOpenSettings: (() -> Void)? { didSet { overlay.onOpenSettings = onOpenSettings } }
    var onRetryPermission: (() -> Void)? { didSet { overlay.onRetryPermission = onRetryPermission } }
    init() {
        super.init(contentRect: overlay.frame, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        level = .floating
        collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        isFloatingPanel = true; becomesKeyOnlyIfNeeded = true
        hidesOnDeactivate = false; ignoresMouseEvents = false
        isOpaque = false; backgroundColor = .clear; hasShadow = true
        contentView = overlay
    }
    func setPhase(_ phase: OverlayPhase) {
        overlay.phase = phase
        let size: NSSize
        switch phase {
        case .permission: size = NSSize(width:480,height:112)
        case .message: size = NSSize(width:430,height:72)
        default: size = NSSize(width:280,height:56)
        }
        setContentSize(size)
        overlay.layoutSubtreeIfNeeded()
    }
    func present(_ phase: OverlayPhase) {
        setPhase(phase)
        let screen = NSScreen.screens.first { $0.frame.contains(NSEvent.mouseLocation) } ?? NSScreen.main
        if let visible = screen?.visibleFrame {
            setFrameOrigin(NSPoint(x: visible.midX - frame.width / 2, y: visible.minY + 65))
        }
        orderFrontRegardless(); overlay.startAnimation()
    }
    func showPermission(_ text: String) { present(.permission(text)) }
    func show(_ text: String) { present(.message(text)) }
    override func orderOut(_ sender: Any?) { overlay.stopAnimation(); super.orderOut(sender) }
}
