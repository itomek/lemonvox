import Foundation

@main struct CoreBridgeTests {
    static func main() {
        precondition(lv_abi_version() == 1)
        var session = LVSession()
        lv_init(&session)
        precondition(session.state == LV_IDLE && session.generation == 0)

        let generation = lv_toggle(&session)
        precondition(session.state == LV_RECORDING && generation == 1)
        precondition(lv_toggle(&session) == generation && session.state == LV_PROCESSING)
        precondition(lv_finish(&session, generation) != 0 && session.state == LV_IDLE)

        let canceled = lv_toggle(&session)
        lv_cancel(&session)
        precondition(lv_finish(&session, canceled) == 0 && session.state == LV_IDLE)
        print("PASS: Swift bridge matches C++ session ABI version, transitions and stale-result rejection")
    }
}
