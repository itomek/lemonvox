#include "common/trigger_binding.h"
#include "trigger/key_normalizer.h"
#include "trigger/shortcut_recorder.h"

#include <windows.h>

#include <cassert>
#include <cstdio>
#include <vector>

using namespace lemonvox::win;

namespace {

constexpr std::uint32_t kFakeCtrlScan = 0x21D;

std::uint32_t scan_for(std::uint32_t vk) {
    switch (vk) {
    case VK_LMENU: case VK_RMENU: case VK_MENU: return 0x38;
    case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL: return 0x1D;
    case VK_LSHIFT: case VK_SHIFT: return 0x2A;
    case VK_RSHIFT: return 0x36;
    case VK_LWIN: return 0x5B;
    case VK_RWIN: return 0x5C;
    case VK_TAB: return 0x0F;
    case VK_F4: return 0x3E;
    case VK_SPACE: return 0x39;
    default: return MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    }
}

bool extended_vk(std::uint32_t vk) {
    return vk == VK_RMENU || vk == VK_RCONTROL || vk == VK_LWIN || vk == VK_RWIN;
}

RawKeyEvent raw(std::uint32_t vk, bool down, std::uint32_t time_ms, std::uint32_t extra_flags = 0) {
    RawKeyEvent event;
    event.vk_code = vk;
    event.scan_code = scan_for(vk);
    const bool alt = vk == VK_LMENU || vk == VK_RMENU || vk == VK_MENU;
    event.flags = (extended_vk(vk) ? LLKHF_EXTENDED : 0u) | (down ? 0u : LLKHF_UP) | extra_flags;
    event.message = down ? (alt ? WM_SYSKEYDOWN : WM_KEYDOWN) : WM_KEYUP;
    event.time_ms = time_ms;
    return event;
}

bool same(const LVNormalizedKeyEvent &event, LVKeyIdentity key, LVKeySide side, LVKeyAction action) {
    return event.key == key && event.side == side && event.action == action;
}

bool is_intervening(const LVNormalizedKeyEvent &event) {
    return event.action == LV_KEY_INTERVENING && event.key == LV_KEY_UNKNOWN && event.side == LV_KEY_SIDE_NONE;
}

// Normalizer + shared detector, exactly as the hook thread wires them.
struct Pipeline {
    KeyNormalizer normalizer;
    LVDoubleTapDetector detector{};
    int fired = 0;

    explicit Pipeline(LVKeyIdentity key = LV_KEY_OPTION, LVKeySide side = LV_KEY_SIDE_ANY) {
        lv_double_tap_init(&detector, key, side);
    }
    void feed(const RawKeyEvent &event) {
        LVNormalizedKeyEvent normalized;
        if (normalizer.key(event, &normalized) && lv_double_tap_event(&detector, normalized)) ++fired;
    }
    void key(std::uint32_t vk, bool down, std::uint32_t time_ms) { feed(raw(vk, down, time_ms)); }
    void tap(std::uint32_t vk, std::uint32_t time_ms, std::uint32_t hold_ms = 60) {
        key(vk, true, time_ms);
        key(vk, false, time_ms + hold_ms);
    }
    void click(std::uint32_t time_ms) {
        if (lv_double_tap_event(&detector, normalizer.mouse_button(time_ms))) ++fired;
    }
    void altgr(bool down, std::uint32_t time_ms) {
        RawKeyEvent fake = raw(VK_LCONTROL, down, time_ms);
        fake.scan_code = kFakeCtrlScan;
        if (down) {
            feed(fake);
            feed(raw(VK_RMENU, true, time_ms));
        } else {
            feed(fake);
            feed(raw(VK_RMENU, false, time_ms));
        }
    }
};

void normalizer_table() {
    struct Row {
        std::uint32_t vk;
        LVKeyIdentity key;
        LVKeySide side;
    };
    const Row rows[] = {
        {VK_LMENU, LV_KEY_OPTION, LV_KEY_SIDE_LEFT},
        {VK_RMENU, LV_KEY_OPTION, LV_KEY_SIDE_RIGHT},
        {VK_LCONTROL, LV_KEY_CONTROL, LV_KEY_SIDE_LEFT},
        {VK_RCONTROL, LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT},
        {VK_LSHIFT, LV_KEY_SHIFT, LV_KEY_SIDE_LEFT},
        {VK_RSHIFT, LV_KEY_SHIFT, LV_KEY_SIDE_RIGHT},
        {VK_LWIN, LV_KEY_COMMAND, LV_KEY_SIDE_LEFT},
        {VK_RWIN, LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT},
    };
    for (const Row &row : rows) {
        KeyNormalizer normalizer;
        LVNormalizedKeyEvent event;
        assert(normalizer.key(raw(row.vk, true, 10), &event));
        assert(same(event, row.key, row.side, LV_KEY_PRESS));
        assert(event.timestamp == 0.0);
        // Auto-repeat of a held modifier is the same press, not a new one.
        assert(!normalizer.key(raw(row.vk, true, 40), &event));
        assert(!normalizer.key(raw(row.vk, true, 70), &event));
        assert(normalizer.key(raw(row.vk, false, 110), &event));
        assert(same(event, row.key, row.side, LV_KEY_RELEASE));
        assert(event.timestamp == 0.1);
    }

    KeyNormalizer normalizer;
    LVNormalizedKeyEvent event;
    // Generic virtual keys (window messages, some injectors) resolve sides from flags/scan codes.
    RawKeyEvent generic = raw(VK_MENU, true, 0);
    generic.flags |= LLKHF_EXTENDED;
    assert(normalizer.key(generic, &event) && same(event, LV_KEY_OPTION, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS));
    generic.flags |= LLKHF_UP;
    assert(normalizer.key(generic, &event) && same(event, LV_KEY_OPTION, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE));
    RawKeyEvent shift = raw(VK_SHIFT, true, 0);
    shift.scan_code = 0x36;
    assert(normalizer.key(shift, &event) && same(event, LV_KEY_SHIFT, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS));
    shift.flags |= LLKHF_UP;
    assert(normalizer.key(shift, &event) && same(event, LV_KEY_SHIFT, LV_KEY_SIDE_RIGHT, LV_KEY_RELEASE));
    RawKeyEvent control = raw(VK_CONTROL, true, 0);
    assert(normalizer.key(control, &event) && same(event, LV_KEY_CONTROL, LV_KEY_SIDE_LEFT, LV_KEY_PRESS));
    control.flags |= LLKHF_UP;
    assert(normalizer.key(control, &event));

    // Ordinary keys interrupt on key-down and carry nothing on key-up.
    for (std::uint32_t vk : {static_cast<std::uint32_t>('A'), static_cast<std::uint32_t>(VK_TAB),
             static_cast<std::uint32_t>(VK_F4), static_cast<std::uint32_t>(VK_CAPITAL),
             static_cast<std::uint32_t>(VK_APPS), static_cast<std::uint32_t>(VK_PACKET)}) {
        assert(normalizer.key(raw(vk, true, 0), &event) && is_intervening(event));
        assert(!normalizer.key(raw(vk, false, 0), &event));
    }

    // A modifier pressed or released while another modifier is held is a chord.
    assert(normalizer.key(raw(VK_LCONTROL, true, 0), &event) && event.action == LV_KEY_PRESS);
    assert(normalizer.key(raw(VK_LMENU, true, 0), &event) && is_intervening(event));
    assert(normalizer.held_modifiers() == (MOD_CONTROL | MOD_ALT));
    assert(normalizer.key(raw(VK_LMENU, false, 0), &event) && is_intervening(event));
    assert(normalizer.key(raw(VK_LCONTROL, false, 0), &event) && event.action == LV_KEY_RELEASE);
    assert(normalizer.held_modifiers() == 0);
}

void normalizer_injection_and_time() {
    KeyNormalizer normalizer;
    LVNormalizedKeyEvent event;
    // LemonVox's own injected input is invisible to the trigger...
    RawKeyEvent own = raw('V', true, 0, LLKHF_INJECTED);
    own.extra_info = kLemonVoxInjectedTag;
    assert(!normalizer.key(own, &event));
    RawKeyEvent own_ctrl = raw(VK_LCONTROL, true, 0, LLKHF_INJECTED);
    own_ctrl.extra_info = kLemonVoxInjectedTag;
    assert(!normalizer.key(own_ctrl, &event));
    assert(normalizer.held_modifiers() == 0);
    // ...while other injected input behaves like hardware.
    assert(normalizer.key(raw(VK_LMENU, true, 0, LLKHF_INJECTED), &event));
    assert(same(event, LV_KEY_OPTION, LV_KEY_SIDE_LEFT, LV_KEY_PRESS));
    assert(normalizer.key(raw(VK_LMENU, false, 0, LLKHF_INJECTED), &event));

    // Event times survive the GetTickCount wrap.
    KeyNormalizer wrapping;
    assert(wrapping.key(raw(VK_LMENU, true, 0xFFFFFF9Cu), &event) && event.timestamp == 0.0);
    assert(wrapping.key(raw(VK_LMENU, false, 0x00000064u), &event) && event.timestamp == 0.2);

    // A mouse button press is intervening.
    assert(is_intervening(wrapping.mouse_button(0x100)));
}

void normalizer_altgr() {
    // Low-level hook form: synthetic LCtrl (scan 0x21D) then RAlt, same time.
    KeyNormalizer normalizer;
    LVNormalizedKeyEvent event;
    RawKeyEvent fake = raw(VK_LCONTROL, true, 500);
    fake.scan_code = kFakeCtrlScan;
    assert(normalizer.key(fake, &event) && is_intervening(event));
    assert(normalizer.key(raw(VK_RMENU, true, 500), &event) && is_intervening(event));
    assert(normalizer.held_modifiers() == (MOD_CONTROL | MOD_ALT));
    // Repeats keep arriving as pairs and stay intervening.
    assert(normalizer.key(fake, &event) && is_intervening(event));
    assert(normalizer.key(raw(VK_RMENU, true, 530), &event) && is_intervening(event));
    fake.flags |= LLKHF_UP;
    assert(normalizer.key(fake, &event) && is_intervening(event));
    assert(normalizer.key(raw(VK_RMENU, false, 560), &event) && is_intervening(event));
    assert(normalizer.held_modifiers() == 0);
    // After AltGr is released a real right Alt is Alt again.
    assert(normalizer.key(raw(VK_RMENU, true, 900), &event) && same(event, LV_KEY_OPTION, LV_KEY_SIDE_RIGHT, LV_KEY_PRESS));
    assert(normalizer.key(raw(VK_RMENU, false, 950), &event) && event.action == LV_KEY_RELEASE);

    // Release order may be reversed.
    fake.flags &= ~static_cast<std::uint32_t>(LLKHF_UP);
    assert(normalizer.key(fake, &event));
    assert(normalizer.key(raw(VK_RMENU, true, 1000), &event) && is_intervening(event));
    assert(normalizer.key(raw(VK_RMENU, false, 1050), &event) && is_intervening(event));
    fake.flags |= LLKHF_UP;
    assert(normalizer.key(fake, &event) && is_intervening(event));
    assert(normalizer.held_modifiers() == 0);

    // Window-message form: no 0x200 marker, recognised by the identical timestamp.
    KeyNormalizer messages;
    RawKeyEvent ctrl = raw(VK_CONTROL, true, 700);
    assert(messages.key(ctrl, &event) && event.action == LV_KEY_PRESS);
    RawKeyEvent menu = raw(VK_MENU, true, 700);
    menu.flags |= LLKHF_EXTENDED;
    assert(messages.key(menu, &event) && is_intervening(event));
    assert(messages.held_modifiers() == (MOD_CONTROL | MOD_ALT));
    ctrl.flags |= LLKHF_UP;
    assert(messages.key(ctrl, &event) && is_intervening(event));
    menu.flags |= LLKHF_UP;
    assert(messages.key(menu, &event) && is_intervening(event));
    assert(messages.held_modifiers() == 0);
}

void double_tap_sequences() {
    {
        Pipeline alt;
        alt.tap(VK_LMENU, 0);
        alt.tap(VK_LMENU, 200);
        assert(alt.fired == 1);
        alt.tap(VK_RMENU, 2000);
        alt.tap(VK_RMENU, 2200);
        assert(alt.fired == 2);
        // A third tap starts a new pair rather than firing again.
        alt.tap(VK_RMENU, 2400);
        assert(alt.fired == 2);
    }
    {
        // Alt+Tab then a single Alt tap.
        Pipeline alt;
        alt.key(VK_LMENU, true, 0);
        alt.tap(VK_TAB, 30);
        alt.key(VK_LMENU, false, 120);
        alt.tap(VK_LMENU, 250);
        assert(alt.fired == 0);
    }
    {
        // Alt+F4 twice in quick succession.
        Pipeline alt;
        for (std::uint32_t t : {0u, 200u}) {
            alt.key(VK_LMENU, true, t);
            alt.tap(VK_F4, t + 20, 20);
            alt.key(VK_LMENU, false, t + 80);
        }
        assert(alt.fired == 0);
    }
    {
        // A held Alt (auto-repeat) is not a tap.
        Pipeline alt;
        alt.key(VK_LMENU, true, 0);
        for (std::uint32_t t = 30; t < 600; t += 33) alt.key(VK_LMENU, true, t);
        alt.key(VK_LMENU, false, 600);
        alt.tap(VK_LMENU, 700);
        assert(alt.fired == 0);
        // Short repeats inside a tap still count as one press.
        Pipeline repeat;
        repeat.key(VK_LMENU, true, 0);
        repeat.key(VK_LMENU, true, 30);
        repeat.key(VK_LMENU, false, 80);
        repeat.tap(VK_LMENU, 200);
        assert(repeat.fired == 1);
    }
    {
        // Alt, a letter, Alt.
        Pipeline alt;
        alt.tap(VK_LMENU, 0);
        alt.tap('K', 80, 20);
        alt.tap(VK_LMENU, 150);
        assert(alt.fired == 0);
    }
    {
        // Side policy: "either side" means both taps on the same side.
        Pipeline any;
        any.tap(VK_LMENU, 0);
        any.tap(VK_RMENU, 200);
        assert(any.fired == 0);
        Pipeline right(LV_KEY_OPTION, LV_KEY_SIDE_RIGHT);
        right.tap(VK_LMENU, 0);
        right.tap(VK_LMENU, 200);
        assert(right.fired == 0);
        right.tap(VK_RMENU, 1000);
        right.tap(VK_RMENU, 1200);
        assert(right.fired == 1);
    }
    {
        // AltGr pressed twice never counts as Alt.
        Pipeline alt;
        alt.altgr(true, 0);
        alt.altgr(false, 60);
        alt.altgr(true, 200);
        alt.altgr(false, 260);
        assert(alt.fired == 0);
        Pipeline right(LV_KEY_OPTION, LV_KEY_SIDE_RIGHT);
        right.altgr(true, 0);
        right.altgr(false, 60);
        right.altgr(true, 200);
        right.altgr(false, 260);
        assert(right.fired == 0);
    }
    {
        // Alt taps while Ctrl is held are a chord.
        Pipeline alt;
        alt.key(VK_LCONTROL, true, 0);
        alt.tap(VK_LMENU, 20);
        alt.tap(VK_LMENU, 200);
        alt.key(VK_LCONTROL, false, 400);
        assert(alt.fired == 0);
    }
    {
        // Mouse click between taps, slow taps, long holds.
        Pipeline alt;
        alt.tap(VK_LMENU, 0);
        alt.click(100);
        alt.tap(VK_LMENU, 200);
        assert(alt.fired == 0);
        alt.tap(VK_LMENU, 2000);
        alt.tap(VK_LMENU, 2500);
        assert(alt.fired == 0);
        alt.tap(VK_LMENU, 4000, 300);
        alt.tap(VK_LMENU, 4400);
        assert(alt.fired == 0);
    }
    {
        // LemonVox's own tagged paste between taps is ignored.
        Pipeline alt;
        alt.tap(VK_LMENU, 0);
        RawKeyEvent paste = raw('V', true, 100, LLKHF_INJECTED);
        paste.extra_info = kLemonVoxInjectedTag;
        alt.feed(paste);
        alt.tap(VK_LMENU, 200);
        assert(alt.fired == 1);
    }
    {
        // Other modifier targets.
        Pipeline ctrl(LV_KEY_CONTROL, LV_KEY_SIDE_ANY);
        ctrl.tap(VK_RCONTROL, 0);
        ctrl.tap(VK_RCONTROL, 200);
        assert(ctrl.fired == 1);
        Pipeline win(LV_KEY_COMMAND, LV_KEY_SIDE_LEFT);
        win.tap(VK_LWIN, 0);
        win.tap(VK_LWIN, 200);
        assert(win.fired == 1);
        Pipeline shift(LV_KEY_SHIFT, LV_KEY_SIDE_ANY);
        shift.tap(VK_LSHIFT, 0);
        shift.tap('A', 100);
        shift.tap(VK_LSHIFT, 200);
        assert(shift.fired == 0);
    }
}

RecorderKey message_key(std::uint32_t vk, bool down, std::uint32_t time_ms, bool extended = false, bool repeat = false) {
    RecorderKey key;
    key.vk = vk;
    key.scan = scan_for(vk) & 0xFF;
    key.extended = extended;
    key.down = down;
    key.repeat = repeat;
    key.time_ms = time_ms;
    return key;
}

void recorder() {
    RecorderKey decoded;
    const LPARAM lparam = static_cast<LPARAM>((1u << 30) | (1u << 24) | (0x38u << 16) | 1u);
    assert(recorder_key_from_message(WM_SYSKEYDOWN, VK_MENU, lparam, 42, &decoded));
    assert(decoded.vk == VK_MENU && decoded.scan == 0x38 && decoded.extended && decoded.down && decoded.repeat);
    assert(decoded.time_ms == 42);
    assert(recorder_key_from_message(WM_KEYUP, 'A', static_cast<LPARAM>(0xC01E0001u), 7, &decoded));
    assert(!decoded.down && !decoded.repeat && decoded.scan == 0x1E);
    assert(!recorder_key_from_message(WM_CHAR, 'a', 0, 0, &decoded));

    ShortcutRecorder recorder;
    assert(recorder.feed(message_key('A', true, 0)) == RecorderStatus::idle);

    // Ctrl+Shift+Space.
    recorder.begin();
    assert(recorder.active());
    assert(recorder.feed(message_key(VK_CONTROL, true, 0)) == RecorderStatus::listening);
    assert(recorder.feed(message_key(VK_CONTROL, true, 30, false, true)) == RecorderStatus::listening);
    assert(recorder.feed(message_key(VK_SHIFT, true, 40)) == RecorderStatus::listening);
    assert(recorder.feed(message_key(VK_SPACE, true, 80)) == RecorderStatus::recorded);
    assert(!recorder.active());
    assert(recorder.binding().kind == TriggerKind::combination);
    assert(serialize_trigger(recorder.binding()) == "combo:ctrl+shift:0x20");
    assert(describe_trigger(recorder.binding()) == L"Ctrl+Shift+Space");

    // Shift-only and bare keys are rejected without ending the recording; Esc cancels.
    recorder.begin();
    assert(recorder.feed(message_key(VK_SHIFT, true, 0)) == RecorderStatus::listening);
    assert(recorder.feed(message_key('A', true, 10)) == RecorderStatus::rejected);
    assert(!recorder.message().empty() && recorder.active());
    assert(recorder.feed(message_key('A', false, 20)) == RecorderStatus::listening);
    assert(recorder.feed(message_key(VK_SHIFT, false, 30)) == RecorderStatus::listening);
    assert(recorder.feed(message_key('B', true, 40)) == RecorderStatus::rejected);
    assert(recorder.feed(message_key(VK_ESCAPE, true, 50)) == RecorderStatus::cancelled);
    assert(!recorder.active());

    // Alt+K and Win+F11.
    recorder.begin();
    recorder.feed(message_key(VK_MENU, true, 0));
    assert(recorder.feed(message_key('K', true, 10)) == RecorderStatus::recorded);
    assert(serialize_trigger(recorder.binding()) == "combo:alt:0x4B");
    recorder.begin();
    recorder.feed(message_key(VK_LWIN, true, 0, true));
    assert(recorder.feed(message_key(VK_F11, true, 10)) == RecorderStatus::recorded);
    assert(serialize_trigger(recorder.binding()) == "combo:win:0x7A");

    // A modifier tapped twice records a double tap on either side.
    recorder.begin();
    assert(recorder.feed(message_key(VK_MENU, true, 0)) == RecorderStatus::listening);
    assert(recorder.feed(message_key(VK_MENU, false, 60)) == RecorderStatus::listening);
    assert(recorder.feed(message_key(VK_MENU, true, 200)) == RecorderStatus::listening);
    assert(recorder.feed(message_key(VK_MENU, false, 260)) == RecorderStatus::recorded);
    assert(serialize_trigger(recorder.binding()) == "double:alt:any");
    recorder.begin();
    recorder.feed(message_key(VK_CONTROL, true, 0, true));
    recorder.feed(message_key(VK_CONTROL, false, 60, true));
    recorder.feed(message_key(VK_CONTROL, true, 200, true));
    assert(recorder.feed(message_key(VK_CONTROL, false, 260, true)) == RecorderStatus::recorded);
    assert(serialize_trigger(recorder.binding()) == "double:ctrl:any");

    // Switching modifiers between taps, or double tap disabled, records nothing.
    recorder.begin();
    recorder.feed(message_key(VK_CONTROL, true, 0));
    recorder.feed(message_key(VK_CONTROL, false, 60));
    recorder.feed(message_key(VK_MENU, true, 200));
    assert(recorder.feed(message_key(VK_MENU, false, 260)) == RecorderStatus::listening);
    recorder.begin(false);
    for (std::uint32_t t : {0u, 200u}) {
        recorder.feed(message_key(VK_MENU, true, t));
        assert(recorder.feed(message_key(VK_MENU, false, t + 60)) == RecorderStatus::listening);
    }
    recorder.cancel();
    assert(!recorder.active());

    // AltGr (Ctrl + extended Alt, same message time) twice is not "Alt twice";
    // AltGr+Q records the equivalent Ctrl+Alt+Q hotkey.
    recorder.begin();
    for (std::uint32_t t : {0u, 200u}) {
        recorder.feed(message_key(VK_CONTROL, true, t));
        recorder.feed(message_key(VK_MENU, true, t, true));
        recorder.feed(message_key(VK_CONTROL, false, t + 60));
        assert(recorder.feed(message_key(VK_MENU, false, t + 60, true)) == RecorderStatus::listening);
    }
    recorder.feed(message_key(VK_CONTROL, true, 500));
    recorder.feed(message_key(VK_MENU, true, 500, true));
    assert(recorder.feed(message_key('Q', true, 520)) == RecorderStatus::recorded);
    assert(serialize_trigger(recorder.binding()) == "combo:ctrl+alt:0x51");
}

void cancellation_shortcut_requires_an_active_held_modifier() {
    TriggerBinding alt_cancel;
    alt_cancel.kind = TriggerKind::combination;
    alt_cancel.modifiers = MOD_ALT;
    alt_cancel.virtual_key = VK_ESCAPE;
    assert(is_cancellation_binding(alt_cancel));
    assert(cancellation_matches(alt_cancel, VK_ESCAPE, true, MOD_ALT));
    assert(!cancellation_matches(alt_cancel, VK_ESCAPE, false, MOD_ALT));
    assert(!cancellation_matches(alt_cancel, VK_ESCAPE, true, 0));
    assert(!cancellation_matches(alt_cancel, VK_ESCAPE, true, MOD_ALT | MOD_CONTROL));

    TriggerBinding ctrl_cancel = alt_cancel;
    ctrl_cancel.modifiers = MOD_CONTROL;
    assert(is_cancellation_binding(ctrl_cancel));
    assert(cancellation_matches(ctrl_cancel, VK_ESCAPE, true, MOD_CONTROL));

    TriggerBinding shift_cancel = alt_cancel;
    shift_cancel.modifiers = MOD_SHIFT;
    assert(is_cancellation_binding(shift_cancel));
    assert(serialize_cancellation_binding(shift_cancel) == "combo:shift:0x1B");
    TriggerBinding parsed;
    assert(parse_cancellation_binding("combo:shift:0x1B", parsed));
    assert(parsed == shift_cancel);

    TriggerBinding invalid = alt_cancel;
    invalid.kind = TriggerKind::double_tap;
    assert(!is_cancellation_binding(invalid));
    invalid = alt_cancel;
    invalid.modifiers = MOD_ALT | MOD_CONTROL;
    assert(!is_cancellation_binding(invalid));
    invalid = alt_cancel;
    invalid.modifiers = MOD_WIN;
    assert(!is_cancellation_binding(invalid));
    invalid = alt_cancel;
    invalid.virtual_key = 'X';
    assert(!is_cancellation_binding(invalid));
    assert(!parse_cancellation_binding("combo:ctrl+alt:0x1B", parsed));

    // AltGr is represented as synthetic Ctrl plus right Alt. It must never
    // satisfy the opt-in Alt+Esc cancellation chord.
    KeyNormalizer normalizer;
    LVNormalizedKeyEvent event;
    RawKeyEvent fake_ctrl = raw(VK_LCONTROL, true, 100);
    fake_ctrl.scan_code = kFakeCtrlScan;
    assert(normalizer.key(fake_ctrl, &event));
    assert(normalizer.key(raw(VK_RMENU, true, 100), &event));
    assert(normalizer.held_modifiers() == (MOD_CONTROL | MOD_ALT));
    assert(!cancellation_matches(alt_cancel, VK_ESCAPE, true, normalizer.held_modifiers()));
}

} // namespace

int main() {
    normalizer_table();
    normalizer_injection_and_time();
    normalizer_altgr();
    double_tap_sequences();
    recorder();
    cancellation_shortcut_requires_an_active_held_modifier();
    std::puts("trigger_unit_test: ok");
    return 0;
}
