#include "trigger/shortcut_recorder.h"

#include <windows.h>

namespace lemonvox::win {

namespace {

bool is_modifier_vk(std::uint32_t vk) {
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
        return true;
    default:
        return false;
    }
}

bool round_trips(const TriggerBinding &binding) {
    TriggerBinding parsed;
    return parse_trigger(serialize_trigger(binding), parsed) && parsed == binding;
}

} // namespace

bool recorder_key_from_message(std::uint32_t message, std::uintptr_t wparam, std::intptr_t lparam,
    std::uint32_t time_ms, RecorderKey *key) {
    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    if (!down && message != WM_KEYUP && message != WM_SYSKEYUP) return false;
    const auto bits = static_cast<std::uint32_t>(lparam);
    key->vk = static_cast<std::uint32_t>(wparam);
    key->scan = (bits >> 16) & 0xFF;
    key->extended = (bits & (1u << 24)) != 0;
    key->down = down;
    key->repeat = down && (bits & (1u << 30)) != 0;
    key->time_ms = time_ms;
    return true;
}

void ShortcutRecorder::begin(bool allow_double_tap) {
    active_ = true;
    allow_double_tap_ = allow_double_tap;
    normalizer_.reset();
    lv_double_tap_init(&detector_, LV_KEY_UNKNOWN, LV_KEY_SIDE_ANY);
    binding_ = TriggerBinding{};
    message_.clear();
}

void ShortcutRecorder::cancel() {
    active_ = false;
    normalizer_.reset();
}

RecorderStatus ShortcutRecorder::feed(const RecorderKey &key) {
    if (!active_) return RecorderStatus::idle;
    if (key.down && key.vk == VK_ESCAPE) {
        cancel();
        return RecorderStatus::cancelled;
    }
    const std::uint32_t held = normalizer_.held_modifiers();
    RawKeyEvent raw;
    raw.vk_code = key.vk;
    raw.scan_code = key.scan;
    raw.flags = (key.extended ? LLKHF_EXTENDED : 0u) | (key.down ? 0u : LLKHF_UP);
    raw.message = key.down ? WM_KEYDOWN : WM_KEYUP;
    raw.time_ms = key.time_ms;
    LVNormalizedKeyEvent event;
    const bool has_event = normalizer_.key(raw, &event);

    if (is_modifier_vk(key.vk)) {
        if (!has_event || !allow_double_tap_) return RecorderStatus::listening;
        if (event.action == LV_KEY_PRESS && event.key != detector_.target_key) {
            lv_double_tap_init(&detector_, event.key, LV_KEY_SIDE_ANY);
        }
        if (!lv_double_tap_event(&detector_, event)) return RecorderStatus::listening;
        TriggerBinding tap;
        tap.kind = TriggerKind::double_tap;
        tap.tap_key = event.key;
        tap.tap_side = LV_KEY_SIDE_ANY;
        binding_ = tap;
        cancel();
        return RecorderStatus::recorded;
    }

    if (has_event) lv_double_tap_event(&detector_, event);
    if (!key.down || key.repeat) return RecorderStatus::listening;
    if (key.vk == VK_PROCESSKEY || key.vk == VK_PACKET) return RecorderStatus::listening;

    TriggerBinding combo;
    combo.kind = TriggerKind::combination;
    combo.modifiers = held;
    combo.virtual_key = key.vk;
    if ((held & (MOD_CONTROL | MOD_ALT | MOD_WIN)) == 0) {
        message_ = L"Include Ctrl, Alt or the Windows key";
        return RecorderStatus::rejected;
    }
    if (!round_trips(combo)) {
        message_ = L"That key can't be used in a shortcut";
        return RecorderStatus::rejected;
    }
    binding_ = combo;
    message_.clear();
    cancel();
    return RecorderStatus::recorded;
}

} // namespace lemonvox::win
