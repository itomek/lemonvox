#pragma once
#include <adwaita.h>
#include "protocol.h"
#include <functional>
#include <string>
#include <vector>
namespace lemonvox::ubuntu {
struct Settings {
    std::string endpoint = "http://127.0.0.1:13305/v1";
    std::string asr_model;
    std::string cleanup_model;
    std::string cleanup_prompt = lv_cleanup_default_prompt();
    std::string appearance = "auto";
    std::string toggle_shortcut = "<Control><Alt>space";
    std::string cancel_shortcut = "<Control><Super>Escape";
    bool debug = false;
    // New settings explicitly target the factory Lemonade server. Legacy files
    // without a backend marker are loaded as generic for compatibility.
    LVCleanupBackend cleanup_backend = LV_CLEANUP_BACKEND_LEMONADE;
    LVCleanupThinking cleanup_thinking = LV_CLEANUP_THINKING_DISABLED;
};
class SettingsStore {
public:
    explicit SettingsStore(std::string path = {});
    Settings load() const;
    bool save(const Settings &, std::string &error) const;
    const std::string &path() const { return path_; }
    static bool valid_shortcuts(const Settings &);
private: std::string path_;
};
void apply_appearance(const Settings &);
void apply_extension_shortcuts(const Settings &);
class SettingsWindow {
public:
    using Save = std::function<bool(const Settings &, std::string &)>;
    using Refresh = std::function<void(const std::string &)>;
    SettingsWindow(GtkApplication *, Settings, Save, Refresh, std::function<void()> open_log, std::function<void(const std::string &)> control);
    ~SettingsWindow();
    void present();
    void health(LVServerHealthStatus);
    void notice(const std::string &text) { status(text); }
    void state(const std::string &, bool recording, bool active, bool has_result);
    void models(std::vector<ProtocolModel>, const std::string &status);
private:
    static void save_clicked(GtkButton *, gpointer);
    static void refresh_clicked(GtkButton *, gpointer);
    Settings read() const;
    void status(const std::string &);
    GtkWindow *window_ = nullptr;
    AdwEntryRow *endpoint_ = nullptr, *toggle_ = nullptr, *cancel_ = nullptr;
    AdwComboRow *asr_ = nullptr, *cleanup_ = nullptr, *appearance_ = nullptr, *execution_ = nullptr;
    GtkTextView *prompt_ = nullptr;
    GtkSwitch *debug_ = nullptr;
    GtkLabel *state_ = nullptr;
    std::vector<GtkLabel *> status_labels_;
    std::vector<AdwActionRow *> health_rows_;
    std::vector<GtkImage *> health_icons_;
    GtkButton *start_ = nullptr, *stop_ = nullptr, *cancel_button_ = nullptr, *copy_ = nullptr;
    Settings settings_;
    Save save_;
    Refresh refresh_;
    std::vector<std::string> asr_ids_, cleanup_ids_;
};
}
