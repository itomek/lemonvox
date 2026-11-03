import AppKit
@main struct ModifierTests {
 @MainActor static func main() {
  var enabled=true, count=0
  var active=false, cancels=0
  let trigger=ModifierTrigger(key:0,allowed:{enabled},fire:{count += 1},canCancel:{active},cancel:{cancels += 1})
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
  trigger.reset(); active=true; enabled=false
  event(55,true,5); event(55,false,5.1); escape(5.2)
  precondition(cancels == 1 && count == 2)
  event(54,true,6); escape(7,.command)
  precondition(cancels == 2)
  event(55,true,8); event(55,false,8.1); escape(8.6)
  event(55,true,9); event(55,false,9.1); trigger.receive(chord); escape(9.2)
  escape(10,[.command,.shift])
  precondition(cancels == 2)
  active=false; event(55,true,11); event(55,false,11.1); escape(11.2); escape(11.3,.command)
  precondition(cancels == 2)
  trigger.stop()
  print("PASS: passive adapter metadata maps both Command sides; chord/other modifier/suspension resets; no OS event dispatch")
 }
}
