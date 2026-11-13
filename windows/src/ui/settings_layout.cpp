#include "ui/settings_layout.h"

#include "common/utf.h"
#include "protocol.h"
#include "net/endpoint_facts.h"

#include <algorithm>
#include <cwctype>

namespace lemonvox::win::settings {

namespace {

constexpr wchar_t kUnavailablePrefix[] = L"(unavailable) ";
constexpr wchar_t kRemoteSuffix[] = L" (remote via server)";
constexpr wchar_t kExampleEndpoint[] = L"http://127.0.0.1:13305/v1";

bool eligible(const SettingsModelOption &option, ModelRole role) {
    return role == ModelRole::transcription ? option.asr : option.cleanup;
}

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

std::wstring modifier_prefix(std::uint32_t modifiers) {
    std::wstring text;
    if (modifiers & MOD_CONTROL) text += L"Ctrl+";
    if (modifiers & MOD_ALT) text += L"Alt+";
    if (modifiers & MOD_SHIFT) text += L"Shift+";
    if (modifiers & MOD_WIN) text += L"Win+";
    return text;
}

TriggerBinding double_tap(LVKeyIdentity key) {
    TriggerBinding binding = default_trigger();
    binding.kind = TriggerKind::double_tap;
    binding.tap_key = key;
    binding.tap_side = LV_KEY_SIDE_ANY;
    return binding;
}

} // namespace

const wchar_t kRecorderPrompt[] = L"Press a shortcut (Esc to cancel)";

std::wstring model_label(const SettingsModelOption &option) {
    std::wstring label = utf8_to_wide(option.id);
    if (option.remote) label += kRemoteSuffix;
    else if (!option.installed) label += L" (not installed)";
    else if (option.loaded) label += L" (loaded)";
    else if (option.loaded_known) label += L" (installed; not loaded)";
    else label += L" (installed)";
    return label;
}

ModelChoices build_model_choices(const std::vector<SettingsModelOption> &models, ModelRole role,
    const std::string &selected, bool loaded) {
    ModelChoices choices;
    if (loaded) {
        std::vector<const SettingsModelOption *> eligible_models;
        for (const auto &model : models) {
            if (!eligible(model, role) || model.id.empty() || utf8_to_wide(model.id).empty()) continue;
            const bool duplicate = std::any_of(eligible_models.begin(), eligible_models.end(),
                [&](const SettingsModelOption *seen) { return seen->id == model.id; });
            if (!duplicate) eligible_models.push_back(&model);
        }
        std::sort(eligible_models.begin(), eligible_models.end(),
            [](const SettingsModelOption *a, const SettingsModelOption *b) { return a->id < b->id; });
        for (const auto *model : eligible_models) {
            if (model->id == selected) choices.selected = static_cast<int>(choices.ids.size());
            choices.ids.push_back(model->id);
            choices.labels.push_back(model_label(*model));
        }
    }
    if (!selected.empty() && choices.selected < 0) {
        choices.selected = static_cast<int>(choices.ids.size());
        choices.ids.push_back(selected);
        choices.labels.push_back((loaded ? kUnavailablePrefix : L"") + utf8_to_wide(selected));
    }
    return choices;
}

DeviceChoices build_device_choices(const std::vector<SettingsDevice> &devices, const std::wstring &selected) {
    DeviceChoices choices;
    choices.ids.push_back(L"");
    choices.labels.push_back(L"System default");
    choices.selected = 0;
    for (const auto &device : devices) {
        if (device.id.empty()) continue;
        if (device.id == selected) choices.selected = static_cast<int>(choices.ids.size());
        choices.ids.push_back(device.id);
        choices.labels.push_back(device.name.empty() ? device.id : device.name);
    }
    if (!selected.empty() && choices.selected == 0) {
        choices.selected = static_cast<int>(choices.ids.size());
        choices.ids.push_back(selected);
        choices.labels.push_back(std::wstring(kUnavailablePrefix) + L"saved microphone");
    }
    return choices;
}

TriggerChoices build_trigger_choices(const TriggerBinding &current) {
    const TriggerBinding shown = trigger_round_trips(current) ? current : default_trigger();
    TriggerChoices choices;
    choices.selected = -1;
    for (const LVKeyIdentity key : {LV_KEY_OPTION, LV_KEY_CONTROL, LV_KEY_SHIFT}) {
        TriggerChoice choice;
        choice.binding = double_tap(key);
        choice.label = describe_trigger(choice.binding);
        if (choice.binding == shown) choices.selected = static_cast<int>(choices.items.size());
        choices.items.push_back(choice);
    }
    if (choices.selected < 0) {
        TriggerChoice choice;
        choice.binding = shown;
        choice.label = describe_trigger(shown);
        choices.selected = static_cast<int>(choices.items.size());
        choices.items.push_back(choice);
    }
    TriggerChoice custom;
    custom.label = L"Custom shortcut\u2026";
    custom.custom = true;
    choices.items.push_back(custom);
    return choices;
}

bool trigger_round_trips(const TriggerBinding &binding) {
    TriggerBinding parsed;
    return parse_trigger(serialize_trigger(binding), parsed) && parsed == binding;
}

RecordStep record_key(std::uint32_t virtual_key, std::uint32_t modifiers) {
    RecordStep step;
    if (virtual_key == VK_ESCAPE && modifiers == 0) {
        step.outcome = RecordStep::Outcome::canceled;
        return step;
    }
    if (virtual_key == 0 || is_modifier_key(virtual_key) || virtual_key == VK_PROCESSKEY || virtual_key == VK_PACKET) {
        step.outcome = RecordStep::Outcome::pending;
        step.display = modifiers ? modifier_prefix(modifiers) + L"\u2026" : kRecorderPrompt;
        return step;
    }
    TriggerBinding binding = default_trigger();
    binding.kind = TriggerKind::combination;
    binding.modifiers = modifiers;
    binding.virtual_key = virtual_key;
    if (!trigger_round_trips(binding)) {
        step.outcome = RecordStep::Outcome::rejected;
        step.display = (modifiers & (MOD_CONTROL | MOD_ALT | MOD_WIN)) == 0
            ? L"A shortcut needs Ctrl, Alt or Windows together with another key."
            : L"That key can't be used in a shortcut. Try another key.";
        return step;
    }
    step.outcome = RecordStep::Outcome::captured;
    step.binding = binding;
    step.display = describe_trigger(binding);
    return step;
}

std::wstring trim(const std::wstring &text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::iswspace(text[begin])) ++begin;
    while (end > begin && std::iswspace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

std::wstring validate_endpoint(const std::wstring &endpoint) {
    const std::wstring text = trim(endpoint);
    if (text.empty()) return std::wstring(L"Enter the server endpoint, for example ") + kExampleEndpoint + L".";
    const std::wstring malformed = std::wstring(L"Enter an endpoint like ") + kExampleEndpoint
        + L" (http or https, ending in /v1, without credentials or query).";
    if (!lemonvox::win::endpoint_is_valid(wide_to_utf8(text))) return malformed;
    return L"";
}

std::wstring validate_prompt(const std::wstring &prompt) {
    if (trim(prompt).empty()) return L"Enter a cleanup prompt, or choose Reset to default.";
    return L"";
}

std::wstring to_edit_newlines(const std::wstring &text) {
    std::wstring result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            result += L"\r\n";
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
        } else if (text[i] == L'\n') {
            result += L"\r\n";
        } else {
            result += text[i];
        }
    }
    return result;
}

std::wstring from_edit_newlines(const std::wstring &text) {
    std::wstring result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            result += L'\n';
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
        } else {
            result += text[i];
        }
    }
    return result;
}

std::wstring server_health_text(LVServerHealthStatus status) {
    if (status == LV_SERVER_HEALTH_CONNECTED) {
        return L"\u25CF Server connected\nConfigured server responded with a compatible model inventory.";
    }
    if (status == LV_SERVER_HEALTH_DISCONNECTED) {
        return L"\u25CF Could not connect\nRequires Lemonade or another compatible OpenAI server, running locally or remotely.";
    }
    return L"\u25CF Checking server\nChecking the saved server endpoint.";
}

StatusText loading_status() {
    return {L"Loading models from server\u2026", false};
}

StatusText endpoint_changed_status() {
    return {L"Endpoint changed. Press Refresh models to load models from it.", false};
}

StatusText catalog_status(const SettingsCatalogResult &result, const std::wstring &endpoint) {
    using Status = SettingsCatalogResult::Status;
    switch (result.status) {
    case Status::ok: {
        size_t asr = 0;
        size_t cleanup = 0;
        for (const auto &model : result.models) {
            asr += model.asr ? 1 : 0;
            cleanup += model.cleanup ? 1 : 0;
        }
        const std::wstring detail = result.message.empty() ? L"" : L" " + result.message;
        if (asr == 0 && cleanup == 0) {
            return {L"The server offered no compatible models. Configure compatible model metadata, then press Refresh models."
                + detail, true};
        }
        if (asr == 0) return {L"The server offered no transcription models." + detail, true};
        if (cleanup == 0) return {L"The server offered no cleanup models." + detail, true};
        return {std::to_wstring(asr) + (asr == 1 ? L" transcription model and " : L" transcription models and ")
            + std::to_wstring(cleanup) + (cleanup == 1 ? L" cleanup model available." : L" cleanup models available."),
            false};
    }
    case Status::configuration:
        return {std::wstring(L"Enter an HTTP/HTTPS server endpoint such as ") + kExampleEndpoint + L".", true};
    case Status::network:
        return {L"Server unavailable at " + endpoint + L" \u2014 start your server and press Refresh models.", true};
    case Status::http:
        return {L"Server at " + endpoint + L" returned an error"
            + (result.message.empty() ? L"" : L" (" + result.message + L")") + L". Press Refresh models to try again.",
            true};
    case Status::response:
        return {L"The server sent an incompatible model list. Check its model metadata or press Refresh models to try again.", true};
    }
    return {L"", false};
}

Layout compute_layout(int dpi, int line_height, const MeasureText &measure) {
    const auto s = [dpi](int value) { return MulDiv(value, dpi, 96); };
    const auto at = [](Layout &layout, SettingsControl control, int x, int y, int w, int h) {
        layout.rects[static_cast<int>(control)] = RECT{x, y, x + w, y + h};
    };
    const auto button_width = [&](SettingsControl control, int minimum) {
        return std::max(s(minimum), measure(control, 0) + s(24));
    };
    Layout layout;
    const int margin = s(16);
    const int inner = s(560) - 2 * margin;
    const int row = std::max(line_height + s(10), s(26));
    const int label_gap = s(4);
    const int small_gap = s(6);
    const int group_gap = s(14);
    const int column_gap = s(12);
    layout.row_height = row;
    int y = margin;

    at(layout, SettingsControl::endpoint_label, margin, y, inner, line_height);
    y += line_height + label_gap;
    const int refresh_w = button_width(SettingsControl::refresh_button, 120);
    at(layout, SettingsControl::endpoint_edit, margin, y, inner - refresh_w - column_gap, row);
    at(layout, SettingsControl::refresh_button, margin + inner - refresh_w, y, refresh_w, row);
    y += row + small_gap;
    const int hint_h = std::max(line_height, measure(SettingsControl::endpoint_hint, inner));
    at(layout, SettingsControl::endpoint_hint, margin, y, inner, hint_h);
    y += hint_h + small_gap;
    const int status_h = std::max(2 * line_height, measure(SettingsControl::status, inner));
    at(layout, SettingsControl::status, margin, y, inner, status_h);
    y += status_h + group_gap;

    at(layout, SettingsControl::asr_label, margin, y, inner, line_height);
    y += line_height + label_gap;
    at(layout, SettingsControl::asr_combo, margin, y, inner, row);
    y += row + group_gap;
    at(layout, SettingsControl::cleanup_label, margin, y, inner, line_height);
    y += line_height + label_gap;
    at(layout, SettingsControl::cleanup_combo, margin, y, inner, row);
    y += row + small_gap;
    const int routing_h = std::max(line_height, measure(SettingsControl::routing_note, inner));
    at(layout, SettingsControl::routing_note, margin, y, inner, routing_h);
    y += routing_h + group_gap;

    const int setup_w = (inner - 2 * small_gap) / 3;
    at(layout, SettingsControl::install_asr_button, margin, y, setup_w, row);
    at(layout, SettingsControl::install_cleanup_button, margin + setup_w + small_gap, y, setup_w, row);
    at(layout, SettingsControl::cancel_install_button, margin + 2 * (setup_w + small_gap), y, setup_w, row);
    y += row + group_gap;

    const int reset_w = button_width(SettingsControl::reset_prompt_button, 120);
    at(layout, SettingsControl::prompt_label, margin, y + (row - line_height) / 2, inner - reset_w - column_gap,
        line_height);
    at(layout, SettingsControl::reset_prompt_button, margin + inner - reset_w, y, reset_w, row);
    y += row + label_gap;
    const int prompt_h = 5 * line_height + s(10);
    at(layout, SettingsControl::prompt_edit, margin, y, inner, prompt_h);
    y += prompt_h + group_gap;

    const int half = (inner - column_gap) / 2;
    at(layout, SettingsControl::shortcut_label, margin, y, half, line_height);
    at(layout, SettingsControl::recorder_label, margin + half + column_gap, y, inner - half - column_gap, line_height);
    y += line_height + label_gap;
    at(layout, SettingsControl::shortcut_combo, margin, y, half, row);
    at(layout, SettingsControl::recorder_edit, margin + half + column_gap, y, inner - half - column_gap, row);
    y += row + group_gap;

    const int privacy_w = button_width(SettingsControl::privacy_button, 180);
    at(layout, SettingsControl::microphone_label, margin, y, inner, line_height);
    y += line_height + label_gap;
    at(layout, SettingsControl::microphone_combo, margin, y, inner - privacy_w - column_gap, row);
    at(layout, SettingsControl::privacy_button, margin + inner - privacy_w, y, privacy_w, row);
    y += row + group_gap + s(6);

    const int action_w = std::max(button_width(SettingsControl::save_button, 88),
        button_width(SettingsControl::cancel_button, 88));
    at(layout, SettingsControl::cancel_button, margin + inner - action_w, y, action_w, row);
    at(layout, SettingsControl::save_button, margin + inner - 2 * action_w - s(8), y, action_w, row);
    y += row + margin;

    layout.client = SIZE{inner + 2 * margin, y};
    return layout;
}

} // namespace lemonvox::win::settings
