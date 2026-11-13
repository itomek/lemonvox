#pragma once

#include "adapters.h"
#include "protocol.h"
#include "common/trigger_binding.h"

#include <string>
#include <utility>

namespace lemonvox::win {

// Shared core policy owns the default prompt for every native shell.
extern const char *const kDefaultCleanupPrompt;
extern const char kDefaultEndpoint[]; // "http://127.0.0.1:13305/v1"

// Every persisted Windows setting. Strings are UTF-8. Lemonade owns providers
// and API keys, so no credential is stored here.
struct WinPreferences {
    std::string endpoint = kDefaultEndpoint;
    std::string asr_model;
    std::string cleanup_model;
    std::string cleanup_prompt = lv_cleanup_default_prompt();
    // New settings explicitly target the factory Lemonade server. Legacy files
    // without a backend marker are loaded as generic for compatibility.
    LVCleanupBackend cleanup_backend = LV_CLEANUP_BACKEND_LEMONADE;
    LVCleanupThinking cleanup_thinking = LV_CLEANUP_THINKING_DISABLED;
    TriggerBinding trigger = default_trigger();
    // WASAPI endpoint ID; empty means the system default capture device.
    std::wstring capture_device_id;

    PreferencesSnapshot snapshot() const;
};

// Per-user persistent store (HKCU\Software\LemonVox by default). load() never
// fails: missing or malformed values fall back to defaults individually.
class PreferencesStore {
public:
    // `registry_subkey` is relative to HKEY_CURRENT_USER; tests pass a scratch key.
    explicit PreferencesStore(std::wstring registry_subkey = L"Software\\LemonVox");
    WinPreferences load() const;
    // Returns false if any value could not be written; earlier values may persist.
    bool save(const WinPreferences &preferences) const;
    // Removes the whole subkey; used by tests and uninstall.
    bool erase() const;
    const std::wstring &subkey() const { return subkey_; }

private:
    std::wstring subkey_;
};

// PreferencesAdapter over an in-memory value the shell replaces on save.
class SnapshotPreferences final : public PreferencesAdapter {
public:
    explicit SnapshotPreferences(WinPreferences preferences) : preferences_(std::move(preferences)) {}
    void replace(WinPreferences preferences) { preferences_ = std::move(preferences); }
    const WinPreferences &current() const { return preferences_; }
    PreferencesSnapshot snapshot() const override { return preferences_.snapshot(); }

private:
    WinPreferences preferences_;
};

} // namespace lemonvox::win
