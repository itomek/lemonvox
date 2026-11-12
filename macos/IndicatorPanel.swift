import AppKit

enum OverlayPhase { case recording, processing, message(String), permission(String) }

private enum SpeechBarMetrics {
    // The extra top/right space belongs to the attached cancel control, not the tile.
    static let tileSize = NSSize(width: 120, height: 32)
    static let size = NSSize(width: 128, height: 40)
    static let cornerRadius: CGFloat = 10
    static let waveformWidth: CGFloat = 26
}

final class OverlayButton: NSButton {
    override var acceptsFirstResponder: Bool { false }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
}

final class ActivityView: NSView {
    var level = 0.0
    var sample = false
    var tick = 0.0
    override func draw(_ dirtyRect: NSRect) {
        effectiveAppearance.performAsCurrentDrawingAppearance {
            NSColor.controlAccentColor.setFill()
            for i in 0..<5 {
                let amplitude = sample ? abs(sin(Double(i) * 0.8 + tick)) : level * (0.45 + 0.55 * abs(sin(Double(i) * 0.8)))
                let height = 3 + 11 * amplitude
                NSBezierPath(roundedRect: NSRect(x: Double(i) * 5 + 1, y: bounds.midY - height / 2,
                    width: 3, height: height), xRadius: 1.5, yRadius: 1.5).fill()
            }
        }
    }
}

final class OverlayView: NSView {
    let glass = NSGlassEffectView()
    let content = NSView()
    let statusLabel = NSTextField(labelWithString: "Listening…")
    let spinner = NSProgressIndicator()
    let activity = ActivityView()
    let microphone = NSImageView()
    let divider = NSBox()
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
        glass.cornerRadius = SpeechBarMetrics.cornerRadius
        glass.contentView = content
        addSubview(glass)
        statusLabel.font = .systemFont(ofSize: 13, weight: .medium)
        statusLabel.textColor = .labelColor
        statusLabel.lineBreakMode = .byTruncatingTail
        statusLabel.isHidden = true
        spinner.style = .spinning; spinner.controlSize = .small
        spinner.isDisplayedWhenStopped = false
        content.addSubview(spinner); content.addSubview(activity)
        microphone.image = NSImage(systemSymbolName: "mic.fill", accessibilityDescription: nil)
        microphone.symbolConfiguration = NSImage.SymbolConfiguration(pointSize: 13, weight: .regular)
        microphone.contentTintColor = .labelColor
        microphone.setAccessibilityElement(false)
        content.addSubview(microphone)
        divider.boxType = .separator
        divider.setAccessibilityElement(false)
        content.addSubview(divider)
        cancelButton.image = NSImage(systemSymbolName: "xmark", accessibilityDescription: "Cancel dictation")
        cancelButton.symbolConfiguration = NSImage.SymbolConfiguration(pointSize: 9, weight: .medium)
        cancelButton.bezelStyle = .circular; cancelButton.isBordered = true
        cancelButton.target = self; cancelButton.action = #selector(cancel)
        cancelButton.refusesFirstResponder = true
        cancelButton.toolTip = "Cancel dictation"; cancelButton.setAccessibilityLabel("Cancel dictation")
        cancelButton.contentTintColor = .labelColor
        // Keep the full attached control inside the panel so every visible pixel is clickable.
        addSubview(cancelButton)
        stopButton.image = NSImage(systemSymbolName: "stop.fill", accessibilityDescription: "Finish recording")
        stopButton.symbolConfiguration = NSImage.SymbolConfiguration(pointSize: 10, weight: .regular)
        stopButton.bezelStyle = .circular
        stopButton.isBordered = false
        stopButton.contentTintColor = .labelColor
        stopButton.refusesFirstResponder = true
        stopButton.target = self
        stopButton.action = #selector(stop)
        stopButton.toolTip = "Finish recording"
        stopButton.setAccessibilityLabel("Finish recording")
        content.addSubview(stopButton)
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
        let tile = NSRect(origin: .zero, size: SpeechBarMetrics.tileSize)
        glass.frame = tile
        content.frame = tile
        cancelButton.frame = NSRect(x: 108, y: 20, width: 20, height: 20)
        stopButton.frame = NSRect(x: 88, y: 2, width: 28, height: 28)
        microphone.frame = NSRect(x: 12, y: 8, width: 14, height: 16)
        activity.frame = NSRect(x: 36, y: 7,
            width: SpeechBarMetrics.waveformWidth, height: 18)
        spinner.frame = NSRect(x: 42, y: 8, width: 16, height: 16)
        divider.frame = NSRect(x: 81, y: 8, width: 1, height: 16)
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
        content.wantsLayer = true
        content.layer?.cornerRadius = SpeechBarMetrics.cornerRadius
        // Resolve semantic colors in this view's appearance, including explicit preview appearances.
        effectiveAppearance.performAsCurrentDrawingAppearance {
            content.layer?.backgroundColor = solid ? NSColor.windowBackgroundColor.cgColor : NSColor.clear.cgColor
        }
        microphone.contentTintColor = .labelColor
        cancelButton.contentTintColor = .labelColor
        stopButton.contentTintColor = .labelColor
        activity.needsDisplay = true
        addSubview(cancelButton, positioned: .above, relativeTo: nil)
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
        stopButton.isHidden = true
        stopButton.isEnabled = false
        divider.isHidden = true
        switch phase {
        case .recording:
            divider.isHidden = false
            stopButton.isHidden = false
            stopButton.isEnabled = true
            statusLabel.stringValue = isSample ? "Sample recording" : "Listening…"
            activity.isHidden = false; spinner.isHidden = true
            cancelButton.isHidden = false; cancelButton.isEnabled = true
        case .processing:
            switch progressLabel.uppercased() {
            case "TRANSCRIBING": statusLabel.stringValue = "Transcribing…"
            case "CHECKING LOCAL MODELS": statusLabel.stringValue = "Checking models…"
            case "WAITING FOR MICROPHONE": statusLabel.stringValue = "Allow microphone…"
            case "CLEANING UP": statusLabel.stringValue = "Cleaning up…"
            default: statusLabel.stringValue = isSample ? "Sample cleanup…" : "Processing…"
            }
            activity.isHidden = true; spinner.isHidden = false
            cancelButton.isHidden = false; cancelButton.isEnabled = true
        case .message(let message), .permission(let message):
            statusLabel.stringValue = message
            activity.isHidden = true; spinner.isHidden = true
            // X remains the one immediate dismissal/cancellation affordance.
            // In an idle message it simply closes the nonactivating panel.
            cancelButton.isHidden = false; cancelButton.isEnabled = true
        }
        statusLabel.toolTip = statusLabel.stringValue
        activity.setAccessibilityLabel(isSample ? "Sample recording" : "Recording level")
        needsLayout = true
        if window?.isVisible == true { startAnimation() }
    }
    @objc private func openSettings() { if case .permission = phase { onOpenSettings?() } }
    @objc private func retryPermission() { if case .permission = phase { onRetryPermission?() } }
    @objc private func stop() { if case .recording = phase { onStop?() } }
    @objc private func cancel() { onCancel?() }
}

final class IndicatorPanel: NSPanel {
    static let speechBarSize = SpeechBarMetrics.size
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
    override var appearance: NSAppearance? {
        didSet { messagePanel.appearance = appearance }
    }
    let overlay = OverlayView(frame: NSRect(origin: .zero, size: SpeechBarMetrics.size))
    let messagePanel = MessagePanel(contentRect: NSRect(x: 0, y: 0, width: 400, height: 72),
        styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
    let messageLabel = NSTextField(wrappingLabelWithString: "")
    private let messageGlass = NSGlassEffectView()
    private let messageContent = NSView()
    private var followsScreenAnchor = true
    private var clamping = false
    private var noticeVisible = false
    private var observers: [NSObjectProtocol] = []
    var onStop: (() -> Void)? { didSet { overlay.onStop = onStop } }
    var onCancel: (() -> Void)? { didSet { overlay.onCancel = onCancel } }
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
            guard let self, !self.clamping else { return }
            self.followsScreenAnchor = false
            self.clampToScreen(); self.positionMessage()
        })
        observers.append(center.addObserver(forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main) { [weak self] _ in
            guard let self else { return }
            if self.followsScreenAnchor, let screen = self.screenForFrame() {
                self.anchor(to: screen)
            } else { self.clampToScreen() }
            self.positionMessage()
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
            noticeVisible = false
            messageLabel.stringValue = text
            layoutMessage()
            if isVisible { messagePanel.orderFrontRegardless() }
        default:
            if !noticeVisible { messagePanel.orderOut(nil) }
        }
    }
    static func anchoredOrigin(screenFrame: NSRect, visibleFrame: NSRect,
                               dockIsHidden: Bool, dockOrientation: String) -> NSPoint {
        // A side Dock must not shift the tile away from the display's center.
        let bottom = dockIsHidden || dockOrientation != "bottom" ? screenFrame.minY : visibleFrame.minY
        return NSPoint(x: screenFrame.midX - SpeechBarMetrics.tileSize.width / 2, y: bottom + 8)
    }
    private func anchor(to screen: NSScreen) {
        let dock = UserDefaults(suiteName: "com.apple.dock")
        let origin = Self.anchoredOrigin(screenFrame: screen.frame, visibleFrame: screen.visibleFrame,
            dockIsHidden: dock?.bool(forKey: "autohide") ?? false,
            dockOrientation: dock?.string(forKey: "orientation") ?? "bottom")
        clamping = true
        setFrameOrigin(origin)
        clamping = false
        followsScreenAnchor = true
    }
    func present(_ phase: OverlayPhase) {
        let startsPresentation = !isVisible
        setPhase(phase)
        if startsPresentation,
           let screen = NSScreen.screens.first(where: { $0.frame.contains(NSEvent.mouseLocation) }) ?? NSScreen.main {
            anchor(to: screen)
        } else if !followsScreenAnchor { clampToScreen() }
        positionMessage()
        orderFrontRegardless(); overlay.startAnimation()
        switch phase { case .message, .permission: messagePanel.orderFrontRegardless(); default: break }
    }
    func showPermission(_ text: String) { present(.permission(text)) }
    func show(_ text: String) { present(.message(text)) }
    func showNotice(_ text: String) {
        guard !text.isEmpty else { return }
        noticeVisible = true
        messageLabel.stringValue = text
        layoutMessage()
        if isVisible { messagePanel.orderFrontRegardless() }
    }
    func clearNotice() {
        guard noticeVisible else { return }
        noticeVisible = false
        switch overlay.phase {
        case .recording, .processing: messagePanel.orderOut(nil)
        case .message, .permission: break
        }
    }
    override func orderOut(_ sender: Any?) {
        noticeVisible = false
        overlay.stopAnimation(); messagePanel.orderOut(sender); super.orderOut(sender)
    }
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
