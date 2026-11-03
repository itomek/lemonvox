import AppKit
import Carbon

@MainActor extension TriggerChoice {
    var display:String {
        switch self {
        case .doubleTap(let key): return (TriggerControl.modifiers.first { $0.1 == key }?.0 ?? "Modifier") + " twice"
        case .combo(let key,let modifiers):
            let prefix=[(UInt32(controlKey),"⌃"),(UInt32(optionKey),"⌥"),(UInt32(shiftKey),"⇧"),(UInt32(cmdKey),"⌘")].filter { modifiers & $0.0 != 0 }.map(\.1).joined()
            let names:[UInt16:String]=[0:"A",1:"S",2:"D",3:"F",4:"H",5:"G",6:"Z",7:"X",8:"C",9:"V",11:"B",12:"Q",13:"W",14:"E",15:"R",16:"Y",17:"T",31:"O",32:"U",34:"I",35:"P",37:"L",38:"J",40:"K",45:"N",46:"M",49:"Space",36:"Return",48:"Tab",51:"Delete",53:"Escape",123:"←",124:"→",125:"↓",126:"↑"]
            return prefix + (names[key] ?? "Key \(key)")
        }
    }
}
@MainActor final class TriggerControl:NSView {
    static let modifiers:[(String,UInt16)]=[("Command",0),("Left Command",55),("Right Command",54),("Left Option",58),("Right Option",61),("Left Control",59),("Right Control",62),("Left Shift",56),("Right Shift",60)]
    let mode=NSPopUpButton(), modifier=NSPopUpButton()
    let record=NSButton(title:"Record Shortcut…",target:nil,action:nil)
    let summary=NSTextField(labelWithString:"")
    private(set) var choice=TriggerChoice.defaultMac
    private(set) var recording=false
    private var monitor:Any?
    var onChange: (() -> Void)?
    private let allowsDoubleTap: Bool
    private var displayedChoice: String {
        !allowsDoubleTap && choice == .defaultMacCancellation ? "Right Command–Escape" : choice.display
    }
    init(frame:NSRect, allowsDoubleTap: Bool = true, initialChoice: TriggerChoice = .defaultMac) {
        self.allowsDoubleTap = allowsDoubleTap
        super.init(frame:frame)
        mode.addItems(withTitles:["Double tap modifier","Key combination"]); mode.target=self; mode.action=#selector(modeChanged)
        modifier.addItems(withTitles:Self.modifiers.map(\.0)); modifier.target=self; modifier.action=#selector(modifierChanged)
        record.target=self; record.action=#selector(beginRecording)
        mode.frame=NSRect(x:0,y:32,width:190,height:26); modifier.frame=NSRect(x:198,y:32,width:180,height:26)
        record.frame=NSRect(x:allowsDoubleTap ? 198 : 0,y:32,width:190,height:26); summary.frame=NSRect(x:0,y:4,width:592,height:22)
        mode.setAccessibilityLabel("Trigger type"); modifier.setAccessibilityLabel("Double-tap modifier")
        record.setAccessibilityLabel(allowsDoubleTap ? "Record dictation shortcut" : "Record cancellation shortcut")
        for v in [mode,modifier,record,summary] { addSubview(v) }; update(initialChoice)
    }
    required init?(coder:NSCoder) { fatalError() }
    func update(_ choice:TriggerChoice) {
        cancelRecording()
        let supported: TriggerChoice
        if allowsDoubleTap { supported=choice } else if case .doubleTap = choice { supported = .defaultMacCancellation } else { supported=choice }
        self.choice=supported
        switch supported {
        case .doubleTap(let key): mode.selectItem(at:0); modifier.selectItem(at:Self.modifiers.firstIndex { $0.1 == key } ?? 0)
        case .combo: mode.selectItem(at:1)
        }
        mode.isHidden = !allowsDoubleTap
        modifier.isHidden = !allowsDoubleTap || mode.indexOfSelectedItem != 0
        record.isHidden = allowsDoubleTap && !modifier.isHidden
        summary.stringValue=displayedChoice
        onChange?()
    }
    @objc func modeChanged() {
        guard allowsDoubleTap else { return }
        if mode.indexOfSelectedItem == 0 { update(.doubleTap(key:Self.modifiers[modifier.indexOfSelectedItem].1)) }
        else { update(.combo(key:9,modifiers:UInt32(cmdKey | optionKey))) }
    }
    @objc func modifierChanged() {
        guard allowsDoubleTap else { return }
        update(.doubleTap(key:Self.modifiers[modifier.indexOfSelectedItem].1))
    }
    @objc func beginRecording() {
        cancelRecording(); recording=true; summary.stringValue="Press a key combination · Escape cancels"
        monitor=NSEvent.addLocalMonitorForEvents(matching:.keyDown) { [weak self] event in
            let consumed=MainActor.assumeIsolated {
                self?.capture(key:event.keyCode,flags:event.modifierFlags,repeating:event.isARepeat) ?? false
            }
            return consumed ? nil : event
        }
    }
    @discardableResult func capture(key:UInt16,flags:NSEvent.ModifierFlags,repeating:Bool) -> Bool {
        guard recording else { return false }
        var bits:UInt32=0
        if flags.contains(.command) { bits |= UInt32(cmdKey) }; if flags.contains(.option) { bits |= UInt32(optionKey) }
        if flags.contains(.control) { bits |= UInt32(controlKey) }; if flags.contains(.shift) { bits |= UInt32(shiftKey) }
        if key == 53 && bits == 0 { cancelRecording(); return true }
        guard !repeating else { return true }
        guard bits & UInt32(cmdKey | optionKey | controlKey) != 0 else { summary.stringValue="Include Command, Option or Control"; return true }
        update(.combo(key:key,modifiers:bits)); return true
    }
    func cancelRecording() {
        if let monitor { NSEvent.removeMonitor(monitor) }; monitor=nil; recording=false; summary.stringValue=displayedChoice
    }
}
