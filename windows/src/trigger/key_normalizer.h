#pragma once

#include "session.h"

#include <cstdint>

namespace lemonvox::win {

// Every input LemonVox injects itself (text insertion, paste chords) must set
// INPUT.ki.dwExtraInfo / INPUT.mi.dwExtraInfo to this value. The trigger
// ignores tagged events entirely; all other injected input (LLKHF_INJECTED,
// e.g. remote desktop, accessibility tools, tests) is treated like hardware.
inline constexpr std::uintptr_t kLemonVoxInjectedTag = 0x4C564F58; // "LVOX"

// The fields of KBDLLHOOKSTRUCT plus the hook's wParam, in portable types.
struct RawKeyEvent {
    std::uint32_t vk_code = 0;
    std::uint32_t scan_code = 0;
    std::uint32_t flags = 0;   // LLKHF_EXTENDED | LLKHF_UP | LLKHF_INJECTED | ...
    std::uint32_t message = 0; // WM_KEYDOWN, WM_SYSKEYDOWN, WM_KEYUP, WM_SYSKEYUP
    std::uint32_t time_ms = 0; // GetTickCount-based event time
    std::uintptr_t extra_info = 0;
};

// Translates Windows keyboard input into the shared LVNormalizedKeyEvent stream.
// Stateful: it tracks which modifiers are held so auto-repeat keydowns are
// swallowed (a held key is one press), chords with other modifiers become
// intervening, and the AltGr pair (synthetic LCtrl + RAlt) never counts as Alt.
// Not thread-safe; the owner serializes calls.
class KeyNormalizer {
public:
    // Returns true and fills *out when the event must be fed to the detector;
    // false when it carries no information (repeat, key-up of an ordinary key,
    // LemonVox's own injected input).
    bool key(const RawKeyEvent &event, LVNormalizedKeyEvent *out);
    // A mouse button press always interrupts a pending double tap.
    LVNormalizedKeyEvent mouse_button(std::uint32_t time_ms);
    // MOD_* flags for modifiers currently held (AltGr, including either
    // release-order handoff, reports Ctrl+Alt as Windows does).
    std::uint32_t held_modifiers() const;
    void reset();

private:
    double seconds(std::uint32_t time_ms);

    std::uint32_t down_ = 0;
    bool fake_ctrl_down_ = false;
    bool altgr_down_ = false;
    std::uint32_t ctrl_down_time_ = 0;
    bool has_time_ = false;
    std::uint32_t last_time_ = 0;
    std::int64_t elapsed_ms_ = 0;
};

} // namespace lemonvox::win
