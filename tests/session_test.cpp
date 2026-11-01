#include "session.h"
#include <cassert>
#include <cstdio>
int main() {
    assert(lv_abi_version() == 1);
    LVSession s;
    lv_init(&s);
    assert(s.state == LV_IDLE);
    auto first = lv_toggle(&s);
    assert(s.state == LV_RECORDING);
    assert(!lv_finish(&s, first));
    assert(lv_toggle(&s) == first && s.state == LV_PROCESSING);
    assert(lv_toggle(&s) == first && s.state == LV_PROCESSING);
    lv_cancel(&s);
    assert(s.state == LV_IDLE && !lv_finish(&s, first));
    auto second = lv_toggle(&s);
    assert(second != first);
    lv_toggle(&s);
    assert(!lv_finish(&s, first) && s.state == LV_PROCESSING);
    assert(lv_finish(&s, second) && s.state == LV_IDLE);
    assert(!lv_finish(&s, second));
    std::puts("PASS: transitions, busy toggle, cancel, stale response, single completion");
}
