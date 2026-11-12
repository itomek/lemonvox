#include "settings.h"
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <utility>

namespace lemonvox::ubuntu {
SettingsStore::SettingsStore(std::string path) : path_(std::move(path)) {
    if (path_.empty()) path_ = std::string(g_get_user_config_dir()) + "/lemonvox/settings.ini";
}
Settings SettingsStore::load() const {
    Settings result;
    auto *key = g_key_file_new();
    if (g_key_file_load_from_file(key, path_.c_str(), G_KEY_FILE_NONE, nullptr)) {
        auto get = [&](const char *name, std::string &value) {
            auto *text = g_key_file_get_string(key, "Settings", name, nullptr);
            if (text) { value = text; g_free(text); }
        };
        get("endpoint", result.endpoint); get("asr_model", result.asr_model);
        get("cleanup_model", result.cleanup_model); get("cleanup_prompt", result.cleanup_prompt);
        get("appearance", result.appearance); get("toggle_shortcut", result.toggle_shortcut);
        get("cancel_shortcut", result.cancel_shortcut);
        result.debug = g_key_file_get_boolean(key, "Settings", "debug", nullptr);
    }
    g_key_file_unref(key);
    if (result.appearance != "auto" && result.appearance != "light" && result.appearance != "dark") result.appearance = "auto";
    if (!valid_shortcuts(result)) {
        result.toggle_shortcut = Settings{}.toggle_shortcut;
        result.cancel_shortcut = Settings{}.cancel_shortcut;
    }
    return result;
}
bool SettingsStore::valid_shortcuts(const Settings &s) {
    guint a = 0, b = 0;
    GdkModifierType am {}, bm {};
    return gtk_accelerator_parse(s.toggle_shortcut.c_str(), &a, &am) &&
        gtk_accelerator_parse(s.cancel_shortcut.c_str(), &b, &bm) && a && b &&
        (am & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) &&
        (bm & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) &&
        (a != b || am != bm);
}
bool SettingsStore::save(const Settings &s, std::string &error) const {
    if (!valid_shortcuts(s)) { error = "Choose distinct shortcuts with Control, Alt or Super and a regular key."; return false; }
    auto *dir = g_path_get_dirname(path_.c_str());
    const bool made = g_mkdir_with_parents(dir, 0700) == 0;
    g_free(dir);
    if (!made) { error = "Could not create the private settings directory."; return false; }
    auto *key = g_key_file_new();
    auto put = [&](const char *name, const std::string &v) { g_key_file_set_string(key, "Settings", name, v.c_str()); };
    put("endpoint", s.endpoint); put("asr_model", s.asr_model); put("cleanup_model", s.cleanup_model);
    put("cleanup_prompt", s.cleanup_prompt); put("appearance", s.appearance);
    put("toggle_shortcut", s.toggle_shortcut); put("cancel_shortcut", s.cancel_shortcut);
    g_key_file_set_boolean(key, "Settings", "debug", s.debug);
    gsize size = 0;
    auto *bytes = g_key_file_to_data(key, &size, nullptr);
    GError *failure = nullptr;
    bool ok = g_file_set_contents_full(path_.c_str(), bytes, static_cast<gssize>(size),
        static_cast<GFileSetContentsFlags>(G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE), 0600, &failure);
    if (ok) ok = g_chmod(path_.c_str(), 0600) == 0;
    if (!ok) error = "Could not save settings.";
    g_clear_error(&failure); g_free(bytes); g_key_file_unref(key);
    return ok;
}
void apply_appearance(const Settings &s) {
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(),
        s.appearance == "dark" ? ADW_COLOR_SCHEME_FORCE_DARK :
        s.appearance == "light" ? ADW_COLOR_SCHEME_FORCE_LIGHT : ADW_COLOR_SCHEME_DEFAULT);
}
void apply_extension_shortcuts(const Settings &s) {
    const char *schema_name = "org.gnome.shell.extensions.lemonvox";
    auto *source = g_settings_schema_source_get_default();
    auto *schema = source ? g_settings_schema_source_lookup(source, schema_name, TRUE) : nullptr;
    if (!schema) {
        const char *roots[] = {g_get_user_data_dir(), "/usr/share", "/usr/local/share"};
        for (auto *root : roots) {
            const auto dir = std::string(root) + "/gnome-shell/extensions/lemonvox@itomek.com/schemas";
            auto *local = g_settings_schema_source_new_from_directory(dir.c_str(), source, FALSE, nullptr);
            if (local) { schema = g_settings_schema_source_lookup(local, schema_name, FALSE); g_settings_schema_source_unref(local); }
            if (schema) break;
        }
    }
    if (!schema) return;
    auto *settings = g_settings_new_full(schema, nullptr, nullptr);
    const char *toggle[] = {s.toggle_shortcut.c_str(), nullptr};
    const char *cancel[] = {s.cancel_shortcut.c_str(), nullptr};
    g_settings_delay(settings);
    g_settings_set_strv(settings, "toggle-shortcut", toggle);
    g_settings_set_strv(settings, "cancel-shortcut", cancel);
    // The extension reads this semantic appearance setting for its native bar.
    if (g_settings_schema_has_key(schema, "appearance")) g_settings_set_string(settings, "appearance", s.appearance.c_str());
    g_settings_apply(settings);
    g_object_unref(settings); g_settings_schema_unref(schema);
}
namespace {
AdwPreferencesGroup *group(AdwPreferencesPage *page, const char *title, const char *description = nullptr) {
    auto *value = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(value, title);
    if (description) adw_preferences_group_set_description(value, description);
    adw_preferences_page_add(page, value); return value;
}
AdwEntryRow *entry(AdwPreferencesGroup *parent, const char *title, const std::string &value) {
    auto *row = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    gtk_editable_set_text(GTK_EDITABLE(row), value.c_str());
    adw_preferences_group_add(parent, GTK_WIDGET(row)); return row;
}
AdwPreferencesPage *page(AdwPreferencesWindow *window, const char *title, const char *icon) {
    auto *p = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(p, title); adw_preferences_page_set_name(p, title); adw_preferences_page_set_icon_name(p, icon);
    // Keep the separate server prerequisite visible on every Settings page.
    group(p, "Server required",
        "Install and run Lemonade or another OpenAI-compatible server separately on this computer. "
        "It must provide audio transcription, chat completions and model-discovery metadata supported by LemonVox. "
        "Local and remote HTTP/HTTPS endpoints are supported. Generic OpenAI API compatibility alone is insufficient. "
        "Lemonade 2026.44.1 is the tested baseline; other servers are unverified.");
    adw_preferences_window_add(window, p); return p;
}
AdwComboRow *combo(AdwPreferencesGroup *parent, const char *title) {
    auto *row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_preferences_group_add(parent, GTK_WIDGET(row)); return row;
}
}
SettingsWindow::SettingsWindow(GtkApplication *app, Settings settings, Save save, Refresh refresh, std::function<void()> open_log, std::function<void(const std::string &)> control)
    : settings_(std::move(settings)), save_(std::move(save)), refresh_(std::move(refresh)) {
    window_ = GTK_WINDOW(adw_preferences_window_new());
    g_object_ref_sink(window_);
    gtk_window_set_application(window_, app);
    gtk_window_set_title(window_, "LemonVox Settings");
    gtk_window_set_default_size(window_, 680, 680);
    gtk_window_set_hide_on_close(window_, TRUE);
    auto *preferences = ADW_PREFERENCES_WINDOW(window_);
    auto *general = page(preferences, "General", "preferences-system-symbolic");
    auto *interaction = group(general, "Dictation", "Stop finishes the recording. Cancel discards it. Use a key combination; Alt twice is unavailable.");
    state_ = GTK_LABEL(gtk_label_new("Ready. Start here or use the LemonVox desktop extension."));
    gtk_label_set_wrap(state_, TRUE);
    gtk_widget_set_margin_start(GTK_WIDGET(state_), 12); gtk_widget_set_margin_end(GTK_WIDGET(state_), 12);
    gtk_widget_set_margin_top(GTK_WIDGET(state_), 12); gtk_widget_set_margin_bottom(GTK_WIDGET(state_), 6);
    adw_preferences_group_add(interaction, GTK_WIDGET(state_));
    auto *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(controls, 12); gtk_widget_set_margin_end(controls, 12);
    gtk_widget_set_margin_bottom(controls, 12);
    struct Action { std::function<void(const std::string &)> fn; std::string name; };
    auto button = [&](const char *label, const char *name) {
        auto *widget = GTK_BUTTON(gtk_button_new_with_label(label));
        auto *action = new Action{control, name};
        g_signal_connect_data(widget, "clicked", G_CALLBACK(+[](GtkButton *, gpointer value) {
            auto &a = *static_cast<Action *>(value); a.fn(a.name);
        }), action, +[](gpointer value, GClosure *) { delete static_cast<Action *>(value); }, G_CONNECT_DEFAULT);
        gtk_box_append(GTK_BOX(controls), GTK_WIDGET(widget)); return widget;
    };
    start_ = button("Start", "Toggle"); stop_ = button("Stop", "Stop");
    cancel_button_ = button("Cancel", "Cancel"); copy_ = button("Copy result", "Copy");
    button("Quit", "Quit");
    adw_preferences_group_add(interaction, controls);
    gtk_widget_set_sensitive(GTK_WIDGET(stop_), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(cancel_button_), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(copy_), FALSE);
    toggle_ = entry(interaction, "Dictation shortcut", settings_.toggle_shortcut);
    cancel_ = entry(interaction, "Cancellation shortcut", settings_.cancel_shortcut);
    auto *target = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(target), "Text destination");
    adw_action_row_set_subtitle(target, "The GNOME extension pastes once at current keyboard focus when processing finishes. Copy is available if paste cannot be dispatched.");
    adw_preferences_group_add(interaction, GTK_WIDGET(target));
    auto *appearance = group(general, "Appearance");
    appearance_ = combo(appearance, "Theme");
    const char *themes[] = {"Auto", "Light", "Dark", nullptr};
    auto *theme_model = gtk_string_list_new(themes);
    adw_combo_row_set_model(appearance_, G_LIST_MODEL(theme_model)); g_object_unref(theme_model);
    adw_combo_row_set_selected(appearance_, settings_.appearance == "dark" ? 2 : settings_.appearance == "light" ? 1 : 0);
    auto *version = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(version), "LemonVox");
#ifdef LEMONVOX_VERSION
    adw_action_row_set_subtitle(version, "Version " LEMONVOX_VERSION " · Build " LEMONVOX_BUILD " · Ubuntu GNOME Wayland");
#else
    adw_action_row_set_subtitle(version, "Development build · Ubuntu GNOME Wayland");
#endif
    adw_preferences_group_add(appearance, GTK_WIDGET(version));
    auto *models_page = page(preferences, "Models", "audio-input-microphone-symbolic");
    auto *connection = group(models_page, "Server connection", "Local and remote HTTP/HTTPS endpoints are supported. The separate server manages models, providers and credentials; models marked remote provider send text through that configured provider.");
    endpoint_ = entry(connection, "API endpoint", settings_.endpoint);
    auto *refresh_button = gtk_button_new_with_label("Refresh models");
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(refresh_clicked), this);
    adw_preferences_group_add(connection, refresh_button);
    asr_ = combo(connection, "Transcription model"); cleanup_ = combo(connection, "Cleanup model");
    auto *prompt_group = group(models_page, "Cleanup prompt");
    auto *scroller = gtk_scrolled_window_new();
    gtk_widget_set_size_request(scroller, -1, 150);
    prompt_ = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_wrap_mode(prompt_, GTK_WRAP_WORD_CHAR);
    gtk_accessible_update_property(GTK_ACCESSIBLE(prompt_), GTK_ACCESSIBLE_PROPERTY_LABEL, "Cleanup prompt", -1);
    gtk_text_view_set_left_margin(prompt_, 12); gtk_text_view_set_right_margin(prompt_, 12);
    gtk_text_view_set_top_margin(prompt_, 12); gtk_text_view_set_bottom_margin(prompt_, 12);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(prompt_), settings_.cleanup_prompt.c_str(), -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), GTK_WIDGET(prompt_));
    adw_preferences_group_add(prompt_group, scroller);
    auto *diagnostics_page = page(preferences, "Diagnostics", "dialog-information-symbolic");
    auto *logging = group(diagnostics_page, "Private diagnostics", "Logs contain fixed event categories and HTTP status only. Audio, transcripts and model responses are never logged.");
    auto *debug_row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(debug_row), "Debug traces");
    debug_ = GTK_SWITCH(gtk_switch_new());
    gtk_switch_set_active(debug_, settings_.debug);
    gtk_widget_set_valign(GTK_WIDGET(debug_), GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(debug_row, GTK_WIDGET(debug_));
    adw_action_row_set_activatable_widget(debug_row, GTK_WIDGET(debug_));
    adw_preferences_group_add(logging, GTK_WIDGET(debug_row));
    auto *log = gtk_button_new_with_label("Open log");
    auto *callback = new std::function<void()>(std::move(open_log));
    g_signal_connect_data(log, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) { (*static_cast<std::function<void()> *>(data))(); }), callback,
        +[](gpointer data, GClosure *) { delete static_cast<std::function<void()> *>(data); }, G_CONNECT_DEFAULT);
    adw_preferences_group_add(logging, log);
    for (auto *p : {general, models_page, diagnostics_page}) {
        auto *save_group = group(p, "");
        auto *actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_set_margin_top(actions, 12); gtk_widget_set_margin_bottom(actions, 12);
        auto *status_label = GTK_LABEL(gtk_label_new("Settings apply to the next dictation session."));
        gtk_label_set_wrap(status_label, TRUE); status_labels_.push_back(status_label);
        gtk_box_append(GTK_BOX(actions), GTK_WIDGET(status_label));
        auto *save_button = gtk_button_new_with_label("Save settings");
        gtk_widget_set_halign(save_button, GTK_ALIGN_END);
        gtk_widget_add_css_class(save_button, "suggested-action");
        g_signal_connect(save_button, "clicked", G_CALLBACK(save_clicked), this);
        gtk_box_append(GTK_BOX(actions), save_button);
        adw_preferences_group_add(save_group, actions);
    }
    models({}, "Refresh models to check the configured selections.");
}
SettingsWindow::~SettingsWindow() { gtk_window_destroy(window_); g_object_unref(window_); }
void SettingsWindow::present() { gtk_window_present(window_); }
void SettingsWindow::state(const std::string &text, bool recording, bool active, bool has_result) {
    gtk_label_set_text(state_, text.c_str());
    gtk_widget_set_sensitive(GTK_WIDGET(start_), !active);
    gtk_widget_set_sensitive(GTK_WIDGET(stop_), recording);
    gtk_widget_set_sensitive(GTK_WIDGET(cancel_button_), active || has_result);
    gtk_widget_set_sensitive(GTK_WIDGET(copy_), has_result);
}
void SettingsWindow::models(std::vector<ProtocolModel> inventory, const std::string &status) {
    auto fill = [&](AdwComboRow *row, bool asr, std::vector<std::string> &ids, const std::string &selected) {
        // Preserve an unsaved selection across an asynchronous refresh.
        const auto old_index = adw_combo_row_get_selected(row);
        const auto current = old_index < ids.size() ? ids[old_index] : selected;
        ids.clear(); ids.push_back("");
        auto *model = gtk_string_list_new(nullptr); gtk_string_list_append(model, "Choose a model");
        for (const auto &m : inventory) if (asr ? m.asr : m.cleanup) {
            ids.push_back(m.id);
            const auto label = (m.remote ? std::string("Remote provider · ") : std::string{}) + m.id;
            gtk_string_list_append(model, label.c_str());
        }
        guint index = 0;
        for (std::size_t i = 1; i < ids.size(); ++i) if (ids[i] == current) index = static_cast<guint>(i);
        if (index == 0 && !current.empty()) {
            index = static_cast<guint>(ids.size()); ids.push_back(current);
            gtk_string_list_append(model, (current + " (not verified)").c_str());
        }
        adw_combo_row_set_model(row, G_LIST_MODEL(model)); adw_combo_row_set_selected(row, index); g_object_unref(model);
    };
    fill(asr_, true, asr_ids_, settings_.asr_model); fill(cleanup_, false, cleanup_ids_, settings_.cleanup_model);
    this->status(status);
}
Settings SettingsWindow::read() const {
    auto result = settings_;
    result.endpoint = gtk_editable_get_text(GTK_EDITABLE(endpoint_));
    result.toggle_shortcut = gtk_editable_get_text(GTK_EDITABLE(toggle_));
    result.cancel_shortcut = gtk_editable_get_text(GTK_EDITABLE(cancel_));
    const auto asr = adw_combo_row_get_selected(asr_), cleanup = adw_combo_row_get_selected(cleanup_);
    result.asr_model = asr < asr_ids_.size() ? asr_ids_[asr] : "";
    result.cleanup_model = cleanup < cleanup_ids_.size() ? cleanup_ids_[cleanup] : "";
    result.appearance = adw_combo_row_get_selected(appearance_) == 2 ? "dark" : adw_combo_row_get_selected(appearance_) == 1 ? "light" : "auto";
    result.debug = gtk_switch_get_active(debug_);
    GtkTextIter start, end; auto *buffer = gtk_text_view_get_buffer(prompt_);
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    auto *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    result.cleanup_prompt = text; g_free(text); return result;
}
void SettingsWindow::save_clicked(GtkButton *, gpointer data) {
    auto &self = *static_cast<SettingsWindow *>(data); auto settings = self.read();
    std::string error;
    if (self.save_(settings, error)) {
        self.settings_ = std::move(settings);
        self.status("Saved. New settings apply to the next session.");
    } else self.status(error);
}
void SettingsWindow::status(const std::string &text) {
    for (auto *label : status_labels_) gtk_label_set_text(label, text.c_str());
}
void SettingsWindow::refresh_clicked(GtkButton *, gpointer data) {
    auto &self = *static_cast<SettingsWindow *>(data);
    self.status("Loading models…");
    self.refresh_(gtk_editable_get_text(GTK_EDITABLE(self.endpoint_)));
}
}
