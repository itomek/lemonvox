#include "trigger/key_normalizer.h"

#include <windows.h>

namespace lemonvox::win {

namespace {

enum Slot : std::uint32_t {
    kLeftAlt = 1u << 0,
    kRightAlt = 1u << 1,
    kLeftCtrl = 1u << 2,
    kRightCtrl = 1u << 3,
    kLeftShift = 1u << 4,
    kRightShift = 1u << 5,
    kLeftWin = 1u << 6,
    kRightWin = 1u << 7,
};

// The synthetic LCtrl that Windows sends ahead of RAlt on AltGr layouts carries
// this bit in KBDLLHOOKSTRUCT::scanCode (0x21D instead of 0x1D).
constexpr std::uint32_t kFakeScanBit = 0x200;
constexpr std::uint32_t kRightShiftScan = 0x36;

struct Modifier {
    std::uint32_t slot;
    LVKeyIdentity key;
    LVKeySide side;
};

bool classify(const RawKeyEvent &event, Modifier *modifier) {
    const bool extended = (event.flags & LLKHF_EXTENDED) != 0;
    switch (event.vk_code) {
    case VK_LMENU: *modifier = {kLeftAlt, LV_KEY_OPTION, LV_KEY_SIDE_LEFT}; return true;
    case VK_RMENU: *modifier = {kRightAlt, LV_KEY_OPTION, LV_KEY_SIDE_RIGHT}; return true;
    case VK_MENU:
        *modifier = extended ? Modifier{kRightAlt, LV_KEY_OPTION, LV_KEY_SIDE_RIGHT}
                             : Modifier{kLeftAlt, LV_KEY_OPTION, LV_KEY_SIDE_LEFT};
        return true;
    case VK_LCONTROL: *modifier = {kLeftCtrl, LV_KEY_CONTROL, LV_KEY_SIDE_LEFT}; return true;
    case VK_RCONTROL: *modifier = {kRightCtrl, LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT}; return true;
    case VK_CONTROL:
        *modifier = extended ? Modifier{kRightCtrl, LV_KEY_CONTROL, LV_KEY_SIDE_RIGHT}
                             : Modifier{kLeftCtrl, LV_KEY_CONTROL, LV_KEY_SIDE_LEFT};
        return true;
    case VK_LSHIFT: *modifier = {kLeftShift, LV_KEY_SHIFT, LV_KEY_SIDE_LEFT}; return true;
    case VK_RSHIFT: *modifier = {kRightShift, LV_KEY_SHIFT, LV_KEY_SIDE_RIGHT}; return true;
    case VK_SHIFT:
        *modifier = (event.scan_code & 0xFF) == kRightShiftScan
            ? Modifier{kRightShift, LV_KEY_SHIFT, LV_KEY_SIDE_RIGHT}
            : Modifier{kLeftShift, LV_KEY_SHIFT, LV_KEY_SIDE_LEFT};
        return true;
    case VK_LWIN: *modifier = {kLeftWin, LV_KEY_COMMAND, LV_KEY_SIDE_LEFT}; return true;
    case VK_RWIN: *modifier = {kRightWin, LV_KEY_COMMAND, LV_KEY_SIDE_RIGHT}; return true;
    default: return false;
    }
}

LVNormalizedKeyEvent make(LVKeyIdentity key, LVKeySide side, LVKeyAction action, double timestamp) {
    return LVNormalizedKeyEvent{key, side, action, timestamp};
}

LVNormalizedKeyEvent intervening(double timestamp) {
    return make(LV_KEY_UNKNOWN, LV_KEY_SIDE_NONE, LV_KEY_INTERVENING, timestamp);
}

} // namespace

double KeyNormalizer::seconds(std::uint32_t time_ms) {
    if (!has_time_) {
        has_time_ = true;
        elapsed_ms_ = 0;
    } else {
        // Signed difference survives the 49.7-day GetTickCount wrap.
        elapsed_ms_ += static_cast<std::int32_t>(time_ms - last_time_);
    }
    last_time_ = time_ms;
    return static_cast<double>(elapsed_ms_) / 1000.0;
}

bool KeyNormalizer::key(const RawKeyEvent &event, LVNormalizedKeyEvent *out) {
    if (event.extra_info == kLemonVoxInjectedTag) return false;
    const double now = seconds(event.time_ms);
    const bool up = (event.flags & LLKHF_UP) != 0 || event.message == WM_KEYUP || event.message == WM_SYSKEYUP;

    Modifier modifier{};
    if (!classify(event, &modifier)) {
        if (up) return false;
        *out = intervening(now);
        return true;
    }

    // AltGr, path 1: the low-level hook marks the synthetic LCtrl explicitly.
    if (modifier.slot == kLeftCtrl && (event.scan_code & kFakeScanBit) != 0) {
        fake_ctrl_down_ = !up;
        *out = intervening(now);
        return true;
    }
    // AltGr, path 2: window messages lose that bit, but the synthetic LCtrl
    // carries exactly the same timestamp as the RAlt that follows it.
    if (modifier.slot == kRightAlt && !up && !altgr_down_ && (down_ & kLeftCtrl) != 0
        && ctrl_down_time_ == event.time_ms) {
        down_ &= ~kLeftCtrl;
        fake_ctrl_down_ = true;
    }
    if (modifier.slot == kRightAlt && (altgr_down_ || fake_ctrl_down_)) {
        altgr_down_ = !up;
        if (up) down_ &= ~kRightAlt;
        *out = intervening(now);
        return true;
    }
    if (modifier.slot == kLeftCtrl && up && fake_ctrl_down_ && (down_ & kLeftCtrl) == 0) {
        fake_ctrl_down_ = false;
        *out = intervening(now);
        return true;
    }

    if (!up) {
        // Auto-repeat of a held modifier: still the same press.
        if ((down_ & modifier.slot) != 0) return false;
        const bool chord = (down_ & ~modifier.slot) != 0 || fake_ctrl_down_ || altgr_down_;
        down_ |= modifier.slot;
        if (modifier.slot == kLeftCtrl) ctrl_down_time_ = event.time_ms;
        *out = chord ? intervening(now) : make(modifier.key, modifier.side, LV_KEY_PRESS, now);
        return true;
    }
    down_ &= ~modifier.slot;
    const bool chord = down_ != 0 || fake_ctrl_down_ || altgr_down_;
    *out = chord ? intervening(now) : make(modifier.key, modifier.side, LV_KEY_RELEASE, now);
    return true;
}

LVNormalizedKeyEvent KeyNormalizer::mouse_button(std::uint32_t time_ms) {
    return intervening(seconds(time_ms));
}

std::uint32_t KeyNormalizer::held_modifiers() const {
    std::uint32_t modifiers = 0;
    // AltGr is one synthetic Ctrl+right-Alt pair. Windows can report either
    // release first, so keep both modifier bits present until both halves are
    // clear; otherwise Escape between releases could impersonate Alt+Esc or
    // Ctrl+Esc cancellation. Genuine Ctrl/Alt state stays in down_.
    const bool altgr_pair = altgr_down_ || fake_ctrl_down_;
    if ((down_ & (kLeftAlt | kRightAlt)) != 0 || altgr_pair) modifiers |= MOD_ALT;
    if ((down_ & (kLeftCtrl | kRightCtrl)) != 0 || altgr_pair) modifiers |= MOD_CONTROL;
    if ((down_ & (kLeftShift | kRightShift)) != 0) modifiers |= MOD_SHIFT;
    if ((down_ & (kLeftWin | kRightWin)) != 0) modifiers |= MOD_WIN;
    return modifiers;
}

void KeyNormalizer::reset() {
    down_ = 0;
    fake_ctrl_down_ = false;
    altgr_down_ = false;
    ctrl_down_time_ = 0;
}

} // namespace lemonvox::win
