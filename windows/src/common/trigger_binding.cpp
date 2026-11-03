#include "trigger_binding.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace lemonvox::win {

namespace {

struct TapName {
    LVKeyIdentity key;
    const char *storage;
    const wchar_t *label;
};

constexpr TapName kTapNames[] = {
    {LV_KEY_OPTION, "alt", L"Alt"},
    {LV_KEY_CONTROL, "ctrl", L"Ctrl"},
    {LV_KEY_SHIFT, "shift", L"Shift"},
    {LV_KEY_COMMAND, "win", L"Windows"},
};

struct SideName {
    LVKeySide side;
    const char *storage;
    const wchar_t *label;
};

constexpr SideName kSideNames[] = {
    {LV_KEY_SIDE_ANY, "any", L""},
    {LV_KEY_SIDE_LEFT, "left", L"Left "},
    {LV_KEY_SIDE_RIGHT, "right", L"Right "},
};

struct ModifierName {
    std::uint32_t flag;
    const char *storage;
    const wchar_t *label;
};

constexpr ModifierName kModifierNames[] = {
    {MOD_CONTROL, "ctrl", L"Ctrl"},
    {MOD_ALT, "alt", L"Alt"},
    {MOD_SHIFT, "shift", L"Shift"},
    {MOD_WIN, "win", L"Win"},
};

constexpr std::uint32_t kAllModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN;

bool is_modifier_key(std::uint32_t vk) {
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

bool valid(const TriggerBinding &binding) {
    if (binding.kind == TriggerKind::double_tap) {
        bool known = false;
        for (const auto &name : kTapNames) known = known || name.key == binding.tap_key;
        bool side = false;
        for (const auto &name : kSideNames) side = side || name.side == binding.tap_side;
        return known && side && binding.modifiers == 0 && binding.virtual_key == 0;
    }
    if (binding.tap_key != LV_KEY_OPTION || binding.tap_side != LV_KEY_SIDE_ANY) return false;
    if ((binding.modifiers & ~kAllModifiers) != 0) return false;
    // Shift alone would capture ordinary typing, so require a non-Shift modifier.
    if ((binding.modifiers & (MOD_CONTROL | MOD_ALT | MOD_WIN)) == 0) return false;
    if (binding.virtual_key < 0x08 || binding.virtual_key > 0xFE) return false;
    return !is_modifier_key(binding.virtual_key);
}

bool valid_cancellation(const TriggerBinding &binding) {
    return binding.kind == TriggerKind::combination && binding.tap_key == LV_KEY_OPTION
        && binding.tap_side == LV_KEY_SIDE_ANY && binding.virtual_key == VK_ESCAPE
        && (binding.modifiers == MOD_ALT || binding.modifiers == MOD_CONTROL || binding.modifiers == MOD_SHIFT);
}

std::wstring key_label(std::uint32_t vk) {
    switch (vk) {
    case VK_SPACE: return L"Space";
    case VK_RETURN: return L"Enter";
    case VK_TAB: return L"Tab";
    case VK_ESCAPE: return L"Esc";
    case VK_BACK: return L"Backspace";
    case VK_DELETE: return L"Delete";
    case VK_INSERT: return L"Insert";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_PRIOR: return L"Page Up";
    case VK_NEXT: return L"Page Down";
    case VK_LEFT: return L"Left";
    case VK_RIGHT: return L"Right";
    case VK_UP: return L"Up";
    case VK_DOWN: return L"Down";
    default: break;
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    wchar_t name[64] = {};
    if (scan != 0 && GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64) > 0) return name;
    wchar_t fallback[16];
    swprintf(fallback, 16, L"Key 0x%02X", vk);
    return fallback;
}

} // namespace

bool TriggerBinding::operator==(const TriggerBinding &other) const {
    return kind == other.kind && tap_key == other.tap_key && tap_side == other.tap_side
        && modifiers == other.modifiers && virtual_key == other.virtual_key;
}

TriggerBinding default_trigger() {
    return TriggerBinding{};
}

bool is_cancellation_binding(const TriggerBinding &binding) {
    return valid_cancellation(binding);
}

std::string serialize_cancellation_binding(const TriggerBinding &binding) {
    if (!valid_cancellation(binding)) return {};
    std::string text = "combo:";
    for (const auto &name : kModifierNames) {
        if ((binding.modifiers & name.flag) != 0) text += name.storage;
    }
    char vk[8];
    std::snprintf(vk, sizeof(vk), ":0x%02X", binding.virtual_key);
    return text + vk;
}

bool parse_cancellation_binding(const std::string &text, TriggerBinding &binding) {
    constexpr std::string_view prefix = "combo:";
    const std::string_view view(text);
    if (view.rfind(prefix, 0) != 0) return false;
    const std::string_view rest = view.substr(prefix.size());
    const size_t colon = rest.rfind(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    const std::string_view modifier = rest.substr(0, colon);
    TriggerBinding parsed;
    for (const auto &name : kModifierNames) {
        if (modifier == name.storage) parsed.modifiers = name.flag;
    }
    const std::string key(rest.substr(colon + 1));
    if (key.size() != 4 || key[0] != '0' || (key[1] != 'x' && key[1] != 'X')) return false;
    char *end = nullptr;
    const unsigned long vk = std::strtoul(key.c_str() + 2, &end, 16);
    if (end != key.c_str() + key.size()) return false;
    parsed.kind = TriggerKind::combination;
    parsed.virtual_key = static_cast<std::uint32_t>(vk);
    if (!valid_cancellation(parsed)) return false;
    binding = parsed;
    return true;
}

bool cancellation_matches(const TriggerBinding &binding, std::uint32_t virtual_key,
    bool down, std::uint32_t held_modifiers) {
    return valid_cancellation(binding) && down && virtual_key == VK_ESCAPE && held_modifiers == binding.modifiers;
}

std::string serialize_trigger(const TriggerBinding &binding) {
    if (!valid(binding)) return serialize_trigger(default_trigger());
    if (binding.kind == TriggerKind::double_tap) {
        std::string text = "double:";
        for (const auto &name : kTapNames) if (name.key == binding.tap_key) text += name.storage;
        text += ':';
        for (const auto &name : kSideNames) if (name.side == binding.tap_side) text += name.storage;
        return text;
    }
    std::string text = "combo:";
    bool first = true;
    for (const auto &name : kModifierNames) {
        if ((binding.modifiers & name.flag) == 0) continue;
        if (!first) text += '+';
        text += name.storage;
        first = false;
    }
    char vk[8];
    std::snprintf(vk, sizeof(vk), ":0x%02X", binding.virtual_key);
    return text + vk;
}

bool parse_trigger(const std::string &text, TriggerBinding &binding) {
    const std::string_view view(text);
    TriggerBinding parsed;
    if (view.rfind("double:", 0) == 0) {
        const std::string_view rest = view.substr(7);
        const size_t colon = rest.find(':');
        if (colon == std::string_view::npos) return false;
        const std::string_view key = rest.substr(0, colon);
        const std::string_view side = rest.substr(colon + 1);
        bool key_found = false;
        bool side_found = false;
        for (const auto &name : kTapNames) {
            if (key == name.storage) { parsed.tap_key = name.key; key_found = true; }
        }
        for (const auto &name : kSideNames) {
            if (side == name.storage) { parsed.tap_side = name.side; side_found = true; }
        }
        if (!key_found || !side_found) return false;
        parsed.kind = TriggerKind::double_tap;
    } else if (view.rfind("combo:", 0) == 0) {
        const std::string_view rest = view.substr(6);
        const size_t colon = rest.rfind(':');
        if (colon == std::string_view::npos || colon == 0) return false;
        std::string_view modifiers = rest.substr(0, colon);
        const std::string key(rest.substr(colon + 1));
        while (!modifiers.empty()) {
            const size_t plus = modifiers.find('+');
            const std::string_view part = modifiers.substr(0, plus);
            bool found = false;
            for (const auto &name : kModifierNames) {
                if (part == name.storage && (parsed.modifiers & name.flag) == 0) {
                    parsed.modifiers |= name.flag;
                    found = true;
                }
            }
            if (!found) return false;
            if (plus == std::string_view::npos) break;
            modifiers = modifiers.substr(plus + 1);
            if (modifiers.empty()) return false;
        }
        if (key.size() != 4 || key[0] != '0' || (key[1] != 'x' && key[1] != 'X')) return false;
        char *end = nullptr;
        const unsigned long vk = std::strtoul(key.c_str() + 2, &end, 16);
        if (end != key.c_str() + key.size()) return false;
        parsed.kind = TriggerKind::combination;
        parsed.virtual_key = static_cast<std::uint32_t>(vk);
    } else {
        return false;
    }
    if (!valid(parsed)) return false;
    binding = parsed;
    return true;
}

std::wstring describe_trigger(const TriggerBinding &binding) {
    const TriggerBinding &shown = (valid(binding) || valid_cancellation(binding)) ? binding : default_trigger();
    if (shown.kind == TriggerKind::double_tap) {
        std::wstring label;
        for (const auto &name : kSideNames) if (name.side == shown.tap_side) label += name.label;
        for (const auto &name : kTapNames) if (name.key == shown.tap_key) label += name.label;
        return label + L" twice";
    }
    std::wstring label;
    for (const auto &name : kModifierNames) {
        if ((shown.modifiers & name.flag) == 0) continue;
        label += name.label;
        label += L'+';
    }
    return label + key_label(shown.virtual_key);
}

} // namespace lemonvox::win
