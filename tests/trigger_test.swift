import Foundation

// Test-only parity oracle: the pre-extraction Swift timing policy. Production
// code must use the C++ detector through DoubleTapDetector instead.
private struct LegacyDoubleTapDetector {
  let key: UInt16
  private var pressed: (key:UInt16,time:TimeInterval)?
  private var first: (key:UInt16,time:TimeInterval)?
  init(key:UInt16) { self.key=key }
  mutating func reset() { pressed=nil; first=nil }
  mutating func event(key candidate:UInt16,down:Bool,time:TimeInterval) -> Bool {
    guard candidate == key || (key == 0 && [54,55].contains(candidate)) else { reset(); return false }
    if down {
      guard pressed == nil else { reset(); return false }
      if let first, first.key != candidate || time-first.time > 0.35 || time < first.time { self.first=nil }
      pressed=(candidate,time); return false
    }
    guard let press=pressed,press.key == candidate,time >= press.time,time-press.time <= 0.25 else { reset(); return false }
    pressed=nil
    if let first,first.key == candidate,time-first.time <= 0.60 {
      reset(); return true
    }
    first=(candidate,time); return false
  }
}

private struct TriggerTrace { let key:UInt16; let down:Bool; let time:TimeInterval }

@main struct TriggerTests {
 static func main() throws {
  let traces:[TriggerTrace] = [
    .init(key:55,down:true,time:0), .init(key:55,down:false,time:0.1),
    .init(key:55,down:true,time:0.2), .init(key:55,down:false,time:0.3),
    .init(key:54,down:true,time:1), .init(key:54,down:false,time:1.1),
    .init(key:55,down:true,time:1.2), .init(key:55,down:false,time:1.3),
    .init(key:55,down:true,time:2), .init(key:55,down:false,time:2.4),
    .init(key:55,down:true,time:2.5), .init(key:55,down:false,time:2.6),
    .init(key:55,down:true,time:3), .init(key:55,down:false,time:3.1),
    .init(key:55,down:true,time:3.8), .init(key:55,down:false,time:3.9),
    .init(key:55,down:true,time:4), .init(key:55,down:true,time:4.1),
    .init(key:55,down:false,time:4.2), .init(key:55,down:true,time:4.3),
    .init(key:55,down:false,time:4.4), .init(key:55,down:true,time:5),
    .init(key:55,down:false,time:5.1), .init(key:55,down:true,time:5.05),
    .init(key:55,down:false,time:5.15), .init(key:55,down:true,time:6),
    .init(key:55,down:false,time:6.1), .init(key:55,down:true,time:6.5),
    .init(key:55,down:false,time:6.6)
  ]
  var legacy=LegacyDoubleTapDetector(key:0), core=DoubleTapDetector(key:0)
  for trace in traces {
    precondition(legacy.event(key:trace.key,down:trace.down,time:trace.time) == core.event(key:trace.key,down:trace.down,time:trace.time))
  }
  legacy.reset(); core.reset()
  precondition(!legacy.event(key:55,down:false,time:6) && !core.event(key:55,down:false,time:6))
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
  print("PASS: Swift-to-core trace parity, full double taps, chord reset, holds/repeats/gaps/side rejection, migration and persistence")
 }
}
