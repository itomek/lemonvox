#pragma once

#include "adapters.h"
#include "common/trigger_binding.h"

#include <string>
#include <utility>

namespace lemonvox::win {

// Matches the Mac default prompt so cleanup semantics are identical.
extern const char kDefaultCleanupPrompt[];
extern const char kDefaultEndpoint[]; // "http://127.0.0.1:13305/v1"

// Every persisted Windows setting. Strings are UTF-8. Lemonade owns providers
// and API keys, so no credential is stored here.
struct WinPreferences {
    std::string endpoint = kDefaultEndpoint;
    std::string asr_model;
    std::string cleanup_model;
    std::string cleanup_prompt = kDefaultCleanupPrompt;
    // false: original field while it stays focused (default). true: the field
    // focused when processing finishes.
    bool insert_current_target = false;
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
