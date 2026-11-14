#include "session.h"

#include <cassert>
#include <cstdio>

static LVNormalizedKeyEvent key_event(LVKeyIdentity key, LVKeySide side,
    LVKeyAction action, double timestamp) {
    return LVNormalizedKeyEvent{key, side, action, timestamp};
}

static void single_tap_tests() {
    LVSingleTapDetector detector;
    lv_single_tap_init(&detector, LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT);
    auto event = [&](LVKeyIdentity key, LVKeySide side, LVKeyAction action, double time) {
        return lv_single_tap_event(&detector, key_event(key, side, action, time));
    };
    assert(!event(LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 0));
    assert(event(LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, .1));
    assert(!event(LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, .11));
    assert(!event(LV_KEY_CONTROL, LV_KEY_SIDE_LEFT, LV_KEY_PRESS, 1));
    assert(!event(LV_KEY_CONTROL, LV_KEY_SIDE_LEFT, LV_KEY_RELEASE, 1.1));
    // Holds, repeats, chords, clock reversal and cancellation cannot fire.
    assert(!event(LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 2));
    assert(!event(LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 2.3));
    assert(!event(LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 3));
    assert(!event(LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 3.01));
    assert(!event(LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 3.02));
    assert(!event(LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 3.1));
    assert(!event(LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 4));
    assert(!event(LV_KEY_UNKNOWN, LV_KEY_SIDE_NONE, LV_KEY_INTERVENING, 4.01));
    assert(!event(LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 4.02));
    assert(!event(LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 4.1));
    assert(!event(LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 5));
    assert(!event(LV_KEY_SHIFT, LV_KEY_SIDE_LEFT, LV_KEY_PRESS, 5.01));
    assert(!event(LV_KEY_SHIFT, LV_KEY_SIDE_LEFT, LV_KEY_RELEASE, 5.02));
    assert(!event(LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 5.1));
    assert(!event(LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 6));
    assert(!event(LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 5.9));
    assert(!event(LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 7));
    lv_single_tap_cancel(&detector);
    assert(!event(LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 7.1));
    assert(!event(LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 8));
    assert(event(LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 8.1));
    lv_single_tap_init(&detector, LV_KEY_OPTION, LV_KEY_SIDE_ANY);
    assert(!event(LV_KEY_OPTION, LV_KEY_SIDE_LEFT, LV_KEY_PRESS, 9));
    assert(event(LV_KEY_OPTION, LV_KEY_SIDE_LEFT, LV_KEY_RELEASE, 9.1));
    assert(!event(LV_KEY_OPTION, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 10));
    assert(event(LV_KEY_OPTION, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 10.1));
    LVSession session;
    lv_init(&session);
    assert(!event(LV_KEY_OPTION, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 11));
    if (event(LV_KEY_OPTION, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 11.1)) lv_toggle(&session);
    assert(session.state == LV_RECORDING);
    assert(!event(LV_KEY_OPTION, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS, 12));
    if (event(LV_KEY_OPTION, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE, 12.1)) lv_toggle(&session);
    assert(session.state == LV_PROCESSING && session.generation == 1);
}

int main() {
    single_tap_tests();
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

    std::puts("PASS: normalized single/double taps, sides, chords, repeats, holds, cancellation and session toggles");
}
