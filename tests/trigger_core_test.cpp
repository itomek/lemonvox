#include "session.h"

#include <cassert>
#include <cstdio>

static LVNormalizedKeyEvent key_event(LVKeyIdentity key, LVKeySide side,
    LVKeyAction action, double timestamp) {
    return LVNormalizedKeyEvent{key, side, action, timestamp};
}

int main() {
    LVDoubleTapDetector detector;
    lv_double_tap_init(&detector, LV_KEY_COMMAND, LV_KEY_SIDE_ANY);

    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_LEFT, LV_KEY_PRESS, 0.0)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_LEFT, LV_KEY_RELEASE, 0.1)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_LEFT, LV_KEY_PRESS, 0.2)));
    assert(lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_LEFT, LV_KEY_RELEASE, 0.3)));

    // A side-agnostic modifier still requires both taps on the same side.
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_LEFT, LV_KEY_PRESS, 1.0)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_LEFT, LV_KEY_RELEASE, 1.1)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 1.2)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 1.3)));

    // Targeted sides, long holds, late second taps, repeats and backward time reset.
    lv_double_tap_init(&detector, LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT);
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_LEFT, LV_KEY_PRESS, 2.0)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_LEFT, LV_KEY_RELEASE, 2.1)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 3.0)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 3.3)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 4.0)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 4.1)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 4.5)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 4.6)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 5.0)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 5.1)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 5.2)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 5.3)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 5.4)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 6.0)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 6.1)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 6.05)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 6.15)));

    // Native adapters send an intervening chord as a content-free reset event.
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 7.0)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 7.1)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_UNKNOWN, LV_KEY_SIDE_NONE, LV_KEY_INTERVENING, 7.15)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 7.2)));
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 7.3)));

    lv_double_tap_cancel(&detector);
    assert(!lv_double_tap_event(&detector, key_event(LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 7.31)));

    std::puts("PASS: normalized double-tap timing, sides, resets, cancellation and monotonic time");
}
