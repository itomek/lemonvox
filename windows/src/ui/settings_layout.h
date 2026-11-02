#pragma once

// Pure Settings logic, kept free of window handles so it can be unit tested:
// choice lists, status text, endpoint validation, the shortcut recorder and the
// DPI-scaled layout.

#include "ui/settings_window.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lemonvox::win::settings {

enum class ModelRole { transcription, cleanup };

struct ModelChoices {
    std::vector<std::string> ids; // UTF-8 model IDs, parallel to labels
    std::vector<std::wstring> labels;
    int selected = -1; // -1: nothing selected
};

// "<id>" or "<id> (remote via Lemonade)".
std::wstring model_label(const SettingsModelOption &option);
// Eligible models sorted by ID. A non-empty `selected` that is missing is kept
// as "(unavailable) <id>". Before the first load (`loaded` false) only the saved
// selection is listed, under its plain ID.
ModelChoices build_model_choices(const std::vector<SettingsModelOption> &models, ModelRole role,
    const std::string &selected, bool loaded);

struct DeviceChoices {
    std::vector<std::wstring> ids; // ids[0] is L"" (system default)
    std::vector<std::wstring> labels;
    int selected = 0;
};
DeviceChoices build_device_choices(const std::vector<SettingsDevice> &devices, const std::wstring &selected);

struct TriggerChoice {
    std::wstring label;
    bool custom = false; // "Custom shortcut…": starts the recorder
    TriggerBinding binding;
};
struct TriggerChoices {
    std::vector<TriggerChoice> items;
    int selected = 0;
};
// Alt/Ctrl/Shift twice, `current` when it is something else, then "Custom shortcut…".
TriggerChoices build_trigger_choices(const TriggerBinding &current);

// True when the binding survives its storage form unchanged.
bool trigger_round_trips(const TriggerBinding &binding);

struct RecordStep {
    enum class Outcome { pending, captured, canceled, rejected };
    Outcome outcome = Outcome::pending;
    TriggerBinding binding;  // valid when captured
    std::wstring display;    // text for the recorder (pending) or the reason (rejected)
};
// One key press seen by the recorder. `modifiers` are the MOD_* keys held,
// including `virtual_key` itself when it is a modifier.
RecordStep record_key(std::uint32_t virtual_key, std::uint32_t modifiers);
extern const wchar_t kRecorderPrompt[];

std::wstring trim(const std::wstring &text);
// Empty when valid; otherwise a user-facing reason. Mirrors the core rules:
// http(s), loopback host, no credentials/query/fragment, path ending in /v1.
std::wstring validate_endpoint(const std::wstring &endpoint);
std::wstring validate_prompt(const std::wstring &prompt);

// Converts between stored LF prompts and the edit control's CRLF text.
std::wstring to_edit_newlines(const std::wstring &text);
std::wstring from_edit_newlines(const std::wstring &text);

struct StatusText {
    std::wstring text;
    bool error = false;
};
StatusText loading_status();
StatusText endpoint_changed_status();
StatusText catalog_status(const SettingsCatalogResult &result, const std::wstring &endpoint);

// Layout in physical pixels for `dpi`. `measure(control, width)` returns the
// wrapped text height for notes (width > 0) or the single-line text width for
// buttons (width == 0) in the window font.
struct Layout {
    RECT rects[static_cast<int>(SettingsControl::count)] = {};
    SIZE client = {};
    int row_height = 0;
};
using MeasureText = std::function<int(SettingsControl control, int width)>;
Layout compute_layout(int dpi, int line_height, const MeasureText &measure);

} // namespace lemonvox::win::settings
