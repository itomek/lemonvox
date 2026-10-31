import Foundation
@main struct TriggerTests {
 static func main() throws {
  var d=DoubleTapDetector(key:55)
  precondition(!d.event(key:55,down:true,time:0)); precondition(!d.event(key:55,down:false,time:0.1))
  precondition(!d.event(key:55,down:true,time:0.2)); precondition(d.event(key:55,down:false,time:0.3))
  precondition(!d.event(key:55,down:false,time:0.31))
  d.reset(); _=d.event(key:55,down:true,time:1); _=d.event(key:55,down:false,time:1.1)
  d.reset(); _=d.event(key:55,down:true,time:1.2); precondition(!d.event(key:55,down:false,time:1.3))
  d.reset(); _=d.event(key:55,down:true,time:2); _=d.event(key:55,down:false,time:2.4)
  _=d.event(key:55,down:true,time:2.5); precondition(!d.event(key:55,down:false,time:2.6))
  d.reset(); _=d.event(key:55,down:true,time:3); _=d.event(key:55,down:false,time:3.1)
  _=d.event(key:55,down:true,time:3.8); precondition(!d.event(key:55,down:false,time:3.9))
  d.reset(); _=d.event(key:55,down:true,time:4); _=d.event(key:55,down:true,time:4.1)
  _=d.event(key:55,down:false,time:4.2); _=d.event(key:55,down:true,time:4.3); precondition(!d.event(key:55,down:false,time:4.4))
  d.reset(); _=d.event(key:55,down:true,time:5); _=d.event(key:55,down:false,time:5.1)
  _=d.event(key:54,down:true,time:5.2); _=d.event(key:55,down:true,time:5.3); precondition(!d.event(key:55,down:false,time:5.4))
  precondition(TriggerChoice.migrate(nil) == .defaultMac && TriggerChoice.migrate(0) == .defaultMac)
  precondition(TriggerChoice.migrate(1) == .combo(key:49,modifiers:768))
  let restored=try JSONDecoder().decode(TriggerChoice.self,from:JSONEncoder().encode(TriggerChoice.defaultMac)); precondition(restored == .defaultMac)
  print("PASS: full double taps, chord reset, holds/repeats/gaps/side rejection, migration and persistence")
 }
}
