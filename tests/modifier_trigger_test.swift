import AppKit
@main struct ModifierTests {
 @MainActor static func main() {
  var enabled=true, count=0
  var active=false, cancels=0
  var cancellationChoice = TriggerChoice.defaultMacCancellation
  let trigger=ModifierTrigger(key:0,allowed:{enabled},fire:{count += 1},
                              canCancel:{ active && ModifierTrigger.canObserveDefaultCancellation(cancellationChoice, sessionActive: true) },
                              cancel:{cancels += 1})
  func event(_ key:UInt16,_ down:Bool,_ time:Double,_ extra:NSEvent.ModifierFlags=[]) {
   var flags=extra
   if down { flags.formUnion([.command,NSEvent.ModifierFlags(rawValue:key == 55 ? 8 : 16)]) }
   let event=NSEvent.keyEvent(with:.flagsChanged,location:.zero,modifierFlags:flags,timestamp:time,windowNumber:0,context:nil,characters:"",charactersIgnoringModifiers:"",isARepeat:false,keyCode:key)!
   trigger.receive(event)
  }
  event(55,true,0); event(55,false,0.1); event(55,true,0.2); event(55,false,0.3); precondition(count == 1)
  event(54,true,1); event(54,false,1.1); event(54,true,1.2); event(54,false,1.3); precondition(count == 2)
  event(55,true,2); event(55,false,2.1)
  let chord=NSEvent.keyEvent(with:.keyDown,location:.zero,modifierFlags:.command,timestamp:2.15,windowNumber:0,context:nil,characters:"c",charactersIgnoringModifiers:"c",isARepeat:false,keyCode:8)!
  trigger.receive(chord); event(55,true,2.2); event(55,false,2.3); precondition(count == 2)
  trigger.reset(); enabled=false; event(55,true,3); event(55,false,3.1); event(55,true,3.2); event(55,false,3.3); precondition(count == 2)
  enabled=true; event(55,true,4,.shift); event(55,false,4.1); event(55,true,4.2); event(55,false,4.3); precondition(count == 2)
  func escape(_ time:Double,_ flags:NSEvent.ModifierFlags=[]) {
   trigger.receive(NSEvent.keyEvent(with:.keyDown,location:.zero,modifierFlags:flags,timestamp:time,windowNumber:0,context:nil,characters:"",charactersIgnoringModifiers:"",isARepeat:false,keyCode:53)!)
  }
  func aggregate(_ key:UInt16,_ time:Double,_ flags:NSEvent.ModifierFlags) {
   trigger.receive(NSEvent.keyEvent(with:.flagsChanged,location:.zero,modifierFlags:flags,timestamp:time,windowNumber:0,context:nil,characters:"",charactersIgnoringModifiers:"",isARepeat:false,keyCode:key)!)
  }
  let bothCommandSnapshot: NSEvent.ModifierFlags = [.command, NSEvent.ModifierFlags(rawValue: 8), NSEvent.ModifierFlags(rawValue: 16)]
  let customCancellation = TriggerChoice.combo(key: 9, modifiers: 2304) // Command-Option-V
  precondition(ModifierTrigger.canObserveDefaultCancellation(.defaultMacCancellation, sessionActive: true))
  precondition(!ModifierTrigger.canObserveDefaultCancellation(.defaultMacCancellation, sessionActive: false))
  precondition(!ModifierTrigger.canObserveDefaultCancellation(customCancellation, sessionActive: true),
               "A saved custom cancellation chord must disable the default Right Command-Escape observer")
  var unknown = ModifierCommandSides()
  unknown.reset(commandHeld: true)
  unknown.observe(NSEvent.keyEvent(with:.flagsChanged,location:.zero,modifierFlags:.command,timestamp:4.5,windowNumber:0,context:nil,characters:"",charactersIgnoringModifiers:"",isARepeat:false,keyCode:54)!)
  precondition(!unknown.rightOnlyHeld, "A reset while Command is held must not guess a side transition")
  var tracked = ModifierCommandSides()
  tracked.reset(commandHeld: false)
  tracked.observe(NSEvent.keyEvent(with:.flagsChanged,location:.zero,modifierFlags:.command,timestamp:4.6,windowNumber:0,context:nil,characters:"",charactersIgnoringModifiers:"",isARepeat:false,keyCode:54)!)
  precondition(tracked.rightOnlyHeld, "An aggregate Right Command press after a known-up baseline must be tracked")
  tracked.observe(NSEvent.keyEvent(with:.flagsChanged,location:.zero,modifierFlags:.command,timestamp:4.7,windowNumber:0,context:nil,characters:"",charactersIgnoringModifiers:"",isARepeat:false,keyCode:55)!)
  precondition(!tracked.rightOnlyHeld, "Both Command keys must not satisfy Right Command only")
  var snapshot = ModifierCommandSides()
  snapshot.reset(commandHeld: false)
  snapshot.observe(NSEvent.keyEvent(with:.flagsChanged,location:.zero,modifierFlags:.command,timestamp:4.8,windowNumber:0,context:nil,characters:"",charactersIgnoringModifiers:"",isARepeat:false,keyCode:54)!)
  snapshot.observe(NSEvent.keyEvent(with:.keyDown,location:.zero,modifierFlags:bothCommandSnapshot,timestamp:4.9,windowNumber:0,context:nil,characters:"",charactersIgnoringModifiers:"",isARepeat:false,keyCode:53)!)
  precondition(!snapshot.rightOnlyHeld, "An authoritative Escape snapshot must reject an unobserved Left Command")
  snapshot.observe(NSEvent.keyEvent(with:.keyDown,location:.zero,modifierFlags:[],timestamp:4.95,windowNumber:0,context:nil,characters:"",charactersIgnoringModifiers:"",isARepeat:false,keyCode:53)!)
  precondition(!snapshot.rightOnlyHeld, "A no-Command Escape must clear tracked Command sides")
  trigger.reset(); active=true; enabled=false
  // Escape metadata alone cannot recover a side after reset.
  escape(5,.command)
  precondition(cancels == 0)
  // A flagsChanged press identifies Right Command when Escape has aggregate flags only.
  aggregate(54,6,.command); escape(7,.command)
  precondition(cancels == 1 && count == 2)
  trigger.reset(); aggregate(54,7.5,.command); escape(7.6,[.command,.shift]); aggregate(54,7.7,[])
  precondition(cancels == 1, "Extra modifiers must not cancel")
  // Left, both sides, and a released Right Command must not cancel.
  trigger.reset(); aggregate(55,8,.command); escape(8.1,.command); aggregate(55,8.2,[])
  trigger.reset(); aggregate(55,9,.command); aggregate(54,9.1,.command); escape(9.2,.command); aggregate(54,9.3,.command); aggregate(55,9.4,[])
  trigger.reset(); aggregate(54,10,.command); aggregate(54,10.1,[]); escape(10.2,.command)
  precondition(cancels == 1)
  // Idle and a saved custom chord keep the default observer inactive.
  active=false; trigger.reset(); aggregate(54,11,.command); escape(11.1,.command); aggregate(54,11.2,[])
  active=true; cancellationChoice=customCancellation; trigger.reset(); aggregate(54,12,.command); escape(12.1,.command); aggregate(54,12.2,[])
  precondition(cancels == 1)
  trigger.stop()
  print("PASS: tracked Command sides accept only held Right Command; custom/idle fallback stays inactive; no OS event dispatch")
 }
}
