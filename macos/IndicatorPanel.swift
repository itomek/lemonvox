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
    let openSettingsButton = OverlayButton(title:"Open Accessibility Settings…",target:nil,action:nil)
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
        glass.cornerRadius = 22
        glass.contentView = content
        addSubview(glass)
        statusLabel.font = .systemFont(ofSize: 13, weight: .medium)
        statusLabel.textColor = .labelColor
        statusLabel.lineBreakMode = .byTruncatingTail
        statusLabel.isHidden = true
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
        cancelButton.frame = NSRect(x: 4, y: bounds.midY - 16, width: 32, height: 32)
        stopButton.frame = NSRect(x: bounds.width - 36, y: bounds.midY - 16, width: 32, height: 32)
        activity.frame = NSRect(x: 42, y: bounds.midY - 14, width: 43, height: 28)
        spinner.frame = NSRect(x: bounds.midX - 8, y: bounds.midY - 8, width: 16, height: 16)
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
        layer?.cornerRadius = 22
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
        activity.setAccessibilityLabel(isSample ? "Sample recording" : "Recording level")
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
    override var appearance: NSAppearance? {
        didSet { messagePanel.appearance = appearance }
    }
    let overlay = OverlayView(frame: NSRect(x: 0, y: 0, width: 128, height: 44))
    let messagePanel = MessagePanel(contentRect: NSRect(x: 0, y: 0, width: 400, height: 72),
        styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
    let messageLabel = NSTextField(wrappingLabelWithString: "")
    private let messageGlass = NSGlassEffectView()
    private let messageContent = NSView()
    private var positioned = false
    private var clamping = false
    private var observers: [NSObjectProtocol] = []
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
        isMovableByWindowBackground = true
        contentView = overlay
        messagePanel.level = .floating
        messagePanel.collectionBehavior = collectionBehavior
        messagePanel.isFloatingPanel = true; messagePanel.becomesKeyOnlyIfNeeded = true
        messagePanel.hidesOnDeactivate = false; messagePanel.isOpaque = false
        messagePanel.backgroundColor = .clear; messagePanel.hasShadow = true
        messageGlass.style = .regular; messageGlass.cornerRadius = 18
        messagePanel.contentView = messageGlass
        messageGlass.contentView = messageContent
        messageLabel.font = .systemFont(ofSize: 13, weight: .medium)
        messageLabel.textColor = .labelColor
        messageContent.addSubview(messageLabel)
        messageContent.addSubview(overlay.openSettingsButton)
        messageContent.addSubview(overlay.retryPermissionButton)
        let center = NotificationCenter.default
        observers.append(center.addObserver(forName: NSWindow.didMoveNotification, object: self, queue: .main) { [weak self] _ in
            self?.clampToScreen(); self?.positionMessage()
        })
        observers.append(center.addObserver(forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main) { [weak self] _ in
            self?.clampToScreen(); self?.positionMessage()
        })
        observers.append(NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.accessibilityDisplayOptionsDidChangeNotification, object: nil, queue: .main) { [weak self] _ in
            self?.updateMessageAppearance()
        })
        updateMessageAppearance()
    }
    deinit {
        for observer in observers {
            NotificationCenter.default.removeObserver(observer)
            NSWorkspace.shared.notificationCenter.removeObserver(observer)
        }
    }
    private func updateMessageAppearance() {
        applyMessageTransparencyPreference(NSWorkspace.shared.accessibilityDisplayShouldReduceTransparency)
    }
    func applyMessageTransparencyPreference(_ solid: Bool) {
        messageGlass.isHidden = solid
        if solid {
            if messageContent.superview !== messagePanel.contentView {
                messageGlass.contentView = nil
                let view = MessageBackgroundView(frame: NSRect(origin: .zero, size: messagePanel.frame.size))
                view.addSubview(messageContent); messagePanel.contentView = view
            }
        } else if messagePanel.contentView !== messageGlass {
            messagePanel.contentView = messageGlass; messageGlass.contentView = messageContent
        }
        layoutMessage()
    }
    private func layoutMessage() {
        let permission: Bool
        if case .permission = overlay.phase { permission = true } else { permission = false }
        let size = NSSize(width: 400, height: permission ? 112 : 72)
        messagePanel.setContentSize(size)
        messageGlass.frame = NSRect(origin: .zero, size: size)
        messageContent.frame = NSRect(origin: .zero, size: size)
        messageLabel.frame = NSRect(x: 18, y: permission ? 55 : 16, width: 364, height: 40)
        overlay.openSettingsButton.frame = NSRect(x: 18, y: 12, width: 220, height: 28)
        overlay.retryPermissionButton.frame = NSRect(x: 250, y: 12, width: 132, height: 28)
        positionMessage()
    }
    private func positionMessage() {
        let visible = screenForFrame()?.visibleFrame
        var x = frame.midX - messagePanel.frame.width / 2
        var y = frame.maxY + 8
        if let visible {
            x = min(max(x, visible.minX), max(visible.minX, visible.maxX - messagePanel.frame.width))
            y = min(y, visible.maxY - messagePanel.frame.height)
        }
        messagePanel.setFrameOrigin(NSPoint(x: x, y: y))
    }
    private func screenForFrame() -> NSScreen? {
        NSScreen.screens.min { a, b in
            func distance(_ rect: NSRect) -> CGFloat {
                let x = max(rect.minX - frame.midX, 0, frame.midX - rect.maxX)
                let y = max(rect.minY - frame.midY, 0, frame.midY - rect.maxY)
                return x * x + y * y
            }
            return distance(a.visibleFrame) < distance(b.visibleFrame)
        } ?? NSScreen.main
    }
    static func clampedOrigin(_ frame: NSRect, in visible: NSRect) -> NSPoint {
        // Reserve space above for the tallest message so it never covers the controls.
        let top = max(visible.minY, visible.maxY - frame.height - 120)
        return NSPoint(x: min(max(frame.minX, visible.minX), max(visible.minX, visible.maxX - frame.width)),
                       y: min(max(frame.minY, visible.minY), top))
    }
    private func clampToScreen() {
        guard !clamping, let visible = screenForFrame()?.visibleFrame else { return }
        let origin = Self.clampedOrigin(frame, in: visible)
        guard origin != frame.origin else { return }
        clamping = true; setFrameOrigin(origin); clamping = false
    }
    func setPhase(_ phase: OverlayPhase) {
        overlay.phase = phase
        overlay.layoutSubtreeIfNeeded()
        switch phase {
        case .message(let text), .permission(let text):
            messageLabel.stringValue = text
            layoutMessage()
            if isVisible { messagePanel.orderFrontRegardless() }
        default: messagePanel.orderOut(nil)
        }
    }
    func present(_ phase: OverlayPhase) {
        setPhase(phase)
        if !positioned {
            let screen = NSScreen.screens.first { $0.frame.contains(NSEvent.mouseLocation) } ?? NSScreen.main
            if let visible = screen?.visibleFrame {
                setFrameOrigin(NSPoint(x: visible.midX - frame.width / 2, y: visible.minY + 65))
                positioned = true
            }
        }
        clampToScreen(); positionMessage()
        orderFrontRegardless(); overlay.startAnimation()
        switch phase { case .message, .permission: messagePanel.orderFrontRegardless(); default: break }
    }
    func showPermission(_ text: String) { present(.permission(text)) }
    func show(_ text: String) { present(.message(text)) }
    override func orderOut(_ sender: Any?) { overlay.stopAnimation(); messagePanel.orderOut(sender); super.orderOut(sender) }
}

final class MessagePanel: NSPanel {
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}

final class MessageBackgroundView: NSView {
    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.cornerRadius = 18
        updateColor()
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        updateColor()
    }
    private func updateColor() {
        effectiveAppearance.performAsCurrentDrawingAppearance {
            layer?.backgroundColor = NSColor.windowBackgroundColor.cgColor
        }
    }
}
