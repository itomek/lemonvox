#pragma once

#include "common/trigger_binding.h"
#include "prefs/preferences.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lemonvox::win {

// One entry of Lemonade's model inventory as the Settings window needs it.
struct SettingsModelOption {
    std::string id; // UTF-8 model ID
    bool asr = false;
    bool cleanup = false;
    bool remote = false; // served by a remote provider routed through Lemonade
};

struct SettingsCatalogResult {
    enum class Status { ok, configuration, network, http, response };
    Status status = Status::ok;
    std::vector<SettingsModelOption> models;
    // Optional, already display-safe detail (Lemonade's reason or an HTTP
    // summary). Never contains credentials or dictated text.
    std::wstring message;
};

struct SettingsDevice {
    std::wstring id; // WASAPI endpoint ID
    std::wstring name;
};

// Everything the Settings window needs from the rest of the app. Every
// callback is invoked on the UI thread that owns the window. Unset callbacks
// are treated as no-ops (refresh then never completes; save succeeds).
struct SettingsServices {
    // Asynchronous model inventory fetch for `endpoint` (UTF-8). `done` must be
    // called exactly once, on the UI thread. A newer request supersedes older
    // ones: the window ignores stale completions, and the service may cancel them.
    std::function<void(const std::string &endpoint, std::function<void(SettingsCatalogResult)> done)>
        refresh_models;
    // Capture devices, excluding the system default (the window adds it).
    std::function<std::vector<SettingsDevice>()> list_devices;
    // Called on Save before `save` when the shortcut changed. Returning false
    // keeps the previous binding active and aborts the save; `error` explains why.
    std::function<bool(const TriggerBinding &, std::wstring *error)> apply_trigger;
    // true while the shortcut recorder has focus, so the global trigger does not
    // fire (and registered hotkeys do not swallow keys) during recording.
    std::function<void(bool suspended)> set_trigger_suspended;
    // Persists and applies the preferences for the next operation. Returning
    // false keeps the window open; the window then restores the old shortcut.
    std::function<bool(const WinPreferences &)> save;
    // Opens ms-settings:privacy-microphone.
    std::function<void()> open_microphone_privacy;
};

// Controls in creation (= tab) order. Use settings_control_id() with
// GetDlgItem to reach a control.
enum class SettingsControl : int {
    endpoint_label,
    endpoint_edit,
    refresh_button,
    endpoint_hint,
    status,
    asr_label,
    asr_combo,
    cleanup_label,
    cleanup_combo,
    routing_note,
    prompt_label,
    prompt_edit,
    reset_prompt_button,
    shortcut_label,
    shortcut_combo,
    recorder_label,
    recorder_edit,
    insert_label,
    insert_combo,
    microphone_label,
    microphone_combo,
    privacy_button,
    save_button,
    cancel_button,
    count,
};

// IDOK / IDCANCEL for Save / Cancel; 1000 + index for the rest.
int settings_control_id(SettingsControl control);

// The native Settings window. Single instance per object; UI thread only.
class SettingsWindow {
public:
    // `default_cleanup_prompt` (UTF-8) backs "Reset to default"; pass kDefaultCleanupPrompt.
    SettingsWindow(HINSTANCE instance, SettingsServices services, std::string default_cleanup_prompt);
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow &) = delete;
    SettingsWindow &operator=(const SettingsWindow &) = delete;

    // Opens Settings populated from `current` and starts an asynchronous model
    // refresh. If already open, brings the existing window forward unchanged.
    void show(const WinPreferences &current);
    // Closes without saving (same as Cancel).
    void close();
    bool is_open() const;
    HWND hwnd() const;
    // Call from the app message loop before TranslateMessage/DispatchMessage;
    // returns true when the message was consumed (dialog keyboard navigation,
    // Enter = Save, Esc = Cancel, mnemonics).
    bool pre_translate(MSG *message);
    // Whether the dark palette is in use (system dark mode, not high contrast).
    bool dark_mode_active() const;

    class Impl;

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace lemonvox::win
