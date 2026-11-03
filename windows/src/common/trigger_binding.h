#pragma once

#include "session.h"

#include <cstdint>
#include <string>

namespace lemonvox::win {

// A persisted Windows trigger. double_tap uses the shared LVDoubleTapDetector;
// combination uses a native global hotkey (MOD_* flags plus a virtual key).
// LV_KEY_OPTION is the portable identity for Windows Alt, LV_KEY_COMMAND for
// the Windows key.
enum class TriggerKind : std::uint8_t { double_tap, combination };

struct TriggerBinding {
    TriggerKind kind = TriggerKind::double_tap;
    // Combination bindings keep these at their defaults so equality stays canonical.
    LVKeyIdentity tap_key = LV_KEY_OPTION;
    LVKeySide tap_side = LV_KEY_SIDE_ANY;
    std::uint32_t modifiers = 0;   // MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN
    std::uint32_t virtual_key = 0; // VK_* code; 0 for double_tap

    bool operator==(const TriggerBinding &other) const;
    bool operator!=(const TriggerBinding &other) const { return !(*this == other); }
};

// The Windows default: Alt pressed twice on either side (AltGr never counts).
TriggerBinding default_trigger();

// Stable ASCII storage form, e.g. "double:alt:any" or "combo:ctrl+shift:0x20".
std::string serialize_trigger(const TriggerBinding &binding);
// Returns false and leaves `binding` unchanged for malformed or unsupported input.
bool parse_trigger(const std::string &text, TriggerBinding &binding);
// Human-readable label, e.g. "Alt twice" or "Ctrl+Shift+Space".
std::wstring describe_trigger(const TriggerBinding &binding);

} // namespace lemonvox::win
