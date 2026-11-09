#include "adapters.h"
#include "settings.h"
#include "diagnostics.h"
#include "workflow.h"
#include <gst/gst.h>
#include <memory>
#include <string>
#include <cstring>
#include <cmath>
#include <cstdio>

namespace lv = lemonvox;
namespace native = lemonvox::ubuntu;
namespace {
constexpr const char *bus_path = "/com/itomek/LemonVox";
constexpr const char *bus_interface = "com.itomek.LemonVox";
constexpr const char *bus_xml = R"XML(<node><interface name="com.itomek.LemonVox">
<method name="Toggle"/><method name="Stop"/><method name="Cancel"/><method name="ShowSettings"/>
<method name="ClearResult"/><method name="Quit"/>
<property name="State" type="s" access="read"/><property name="Message" type="s" access="read"/>
<property name="Result" type="s" access="read"/><property name="Palette" type="s" access="read"/><property name="Error" type="b" access="read"/></interface></node>)XML";
const char *phase_name(lv::WorkflowPhase phase) {
    switch (phase) {
        case lv::WorkflowPhase::idle: return "idle";
        case lv::WorkflowPhase::preparing: return "preparing";
        case lv::WorkflowPhase::recording: return "recording";
        case lv::WorkflowPhase::transcribing: return "transcribing";
        case lv::WorkflowPhase::cleaning: return "cleaning";
        case lv::WorkflowPhase::inserting: return "inserting";
    }
    return "idle";
}
class Application {
public:
    Application() : app_(ADW_APPLICATION(adw_application_new("com.itomek.LemonVox", G_APPLICATION_HANDLES_COMMAND_LINE))),
        executor_(g_main_context_default()), capture_(executor_), transport_(executor_), discovery_(executor_), insertion_(executor_) {
        g_signal_connect(app_, "startup", G_CALLBACK(+[](GApplication *, gpointer self) { static_cast<Application *>(self)->startup(); }), this);
        g_signal_connect(app_, "activate", G_CALLBACK(+[](GApplication *, gpointer self) { static_cast<Application *>(self)->show_settings(); }), this);
        g_signal_connect(app_, "command-line", G_CALLBACK(+[](GApplication *, GApplicationCommandLine *line, gpointer self) -> int {
            return static_cast<Application *>(self)->command_line(line);
        }), this);
        g_signal_connect(app_, "shutdown", G_CALLBACK(+[](GApplication *, gpointer self) { static_cast<Application *>(self)->shutdown(); }), this);
    }
    ~Application() { shutdown(); g_object_unref(app_); }
    int run(int argc, char **argv) { return g_application_run(G_APPLICATION(app_), argc, argv); }
private:
    void startup() {
        settings_ = store_.load();
        native::apply_appearance(settings_); native::apply_extension_shortcuts(settings_);
        diagnostics_.set_debug(settings_.debug);
        refresh_palette();
        style_signal_ = g_signal_connect(adw_style_manager_get_default(), "notify",
            G_CALLBACK(+[](GObject *, GParamSpec *, gpointer data) {
                auto &self = *static_cast<Application *>(data);
                if (!self.palette_source_) self.palette_source_ = g_idle_add(+[](gpointer value) -> gboolean {
                    auto &app = *static_cast<Application *>(value); app.palette_source_ = 0;
                    app.refresh_palette(); app.changed(); return G_SOURCE_REMOVE;
                }, &self);
            }), this);
        if (const auto *device = g_getenv("LEMONVOX_AUDIO_DEVICE")) capture_.set_source_device(device);
        lv::WorkflowCoreCallbacks callbacks;
        callbacks.phase_changed = [this](lv::OperationId, lv::WorkflowPhase phase) {
            state_ = phase_name(phase);
            if (capture_limit_) { g_source_remove(capture_limit_); capture_limit_ = 0; }
            if (phase == lv::WorkflowPhase::recording) capture_limit_ = g_timeout_add_seconds(120, +[](gpointer data) -> gboolean {
                auto &self = *static_cast<Application *>(data); self.capture_limit_ = 0;
                self.action("Stop"); return G_SOURCE_REMOVE;
            }, this);
            switch (phase) {
                case lv::WorkflowPhase::idle: break;
                case lv::WorkflowPhase::preparing: message_ = "Checking models…"; break;
                case lv::WorkflowPhase::recording: message_ = "Listening. Stop finishes; Cancel discards."; break;
                case lv::WorkflowPhase::transcribing: message_ = "Transcribing…"; break;
                case lv::WorkflowPhase::cleaning: message_ = "Cleaning up…"; break;
                case lv::WorkflowPhase::inserting: message_ = "Preparing result…"; break;
            }
            changed();
        };
        callbacks.text_ready = [this](lv::OperationId, std::string text) {
            result_ = std::move(text);
            if (result_expiry_) g_source_remove(result_expiry_);
            result_expiry_ = g_timeout_add_seconds(300, +[](gpointer data) -> gboolean {
                auto &self = *static_cast<Application *>(data); self.result_expiry_ = 0;
                self.result_.clear(); self.message_ = "Result expired. Start a new dictation."; self.changed(); return G_SOURCE_REMOVE;
            }, this);
        };
        callbacks.completed = [this](lv::OperationId, lv::WorkflowOutcome outcome) {
            error_ = false;
            message_ = outcome.kind == lv::WorkflowOutcomeKind::copy_fallback ?
                "Ready to copy. Choose Copy, then paste into your field." : "Done.";
            changed();
        };
        callbacks.failed = [this](lv::OperationId, lv::WorkflowFailure failure) {
            result_.clear(); error_ = true;
            if (failure.source == lv::WorkflowFailureSource::protocol) {
                switch (failure.protocol_status) {
                    case LV_PROTOCOL_CONFIGURATION: message_ = "Set a local Lemonade endpoint and both models in Settings."; break;
                    case LV_PROTOCOL_MODEL_UNAVAILABLE: message_ = "Selected models are unavailable. Refresh models in Settings."; break;
                    case LV_PROTOCOL_EMPTY_TRANSCRIPT: message_ = "No speech was recognized. Try another recording."; break;
                    case LV_PROTOCOL_EMPTY_CLEANUP: message_ = "Cleanup returned no text. Check the cleanup model and try again."; break;
                    default: message_ = "Lemonade could not process this recording. Check the server and models, then try again."; break;
                }
            } else if (failure.adapter_error == lv::AdapterError::unavailable || failure.adapter_error == lv::AdapterError::invalid_input) {
                message_ = "Audio capture is unavailable or empty. Check the input device and microphone access.";
            } else message_ = "Could not reach Lemonade. Check the local server and endpoint, then try again.";
            changed();
        };
        core_ = std::make_unique<lv::WorkflowCore>(capture_, transport_, insertion_, diagnostics_, std::move(callbacks));
        auto *bus = g_application_get_dbus_connection(G_APPLICATION(app_));
        GError *error = nullptr;
        node_ = g_dbus_node_info_new_for_xml(bus_xml, &error);
        if (node_ && bus) {
            static const GDBusInterfaceVTable table = {
                +[](GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *method, GVariant *, GDBusMethodInvocation *invocation, gpointer data) {
                    auto &self = *static_cast<Application *>(data);
                    const std::string command = method;
                    // Reply before Quit can tear down the exported object.
                    g_dbus_method_invocation_return_value(invocation, nullptr);
                    self.action(command);
                },
                +[](GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *name, GError **, gpointer data) -> GVariant * {
                    auto &self = *static_cast<Application *>(data);
                    if (std::strcmp(name, "Error") == 0) return g_variant_new_boolean(self.error_);
                    return g_variant_new_string(std::strcmp(name, "State") == 0 ? self.state_.c_str() :
                        std::strcmp(name, "Message") == 0 ? self.message_.c_str() :
                        std::strcmp(name, "Palette") == 0 ? self.palette_.c_str() : self.result_.c_str());
                }, nullptr, {nullptr}
            };
            registration_ = g_dbus_connection_register_object(bus, bus_path, node_->interfaces[0], &table, this, nullptr, &error);
        }
        if (error) { g_warning("LemonVox service registration failed"); g_clear_error(&error); }
        g_application_hold(G_APPLICATION(app_));
    }
    int command_line(GApplicationCommandLine *line) {
        gint count = 0; auto **args = g_application_command_line_get_arguments(line, &count);
        std::string command = "ShowSettings";
        for (int i = 1; i < count; ++i) {
            if (std::strcmp(args[i], "--service") == 0) command.clear();
            else if (std::strcmp(args[i], "--settings") == 0) command = "ShowSettings";
            else if (std::strcmp(args[i], "--toggle") == 0) command = "Toggle";
            else if (std::strcmp(args[i], "--stop") == 0) command = "Stop";
            else if (std::strcmp(args[i], "--cancel") == 0) command = "Cancel";
            else if (std::strcmp(args[i], "--quit") == 0) command = "Quit";
            else if (std::strcmp(args[i], "--version") == 0) {
#ifdef LEMONVOX_VERSION
                g_application_command_line_print(line, "LemonVox %s\n", LEMONVOX_VERSION);
#else
                g_application_command_line_print(line, "LemonVox development\n");
#endif
                if (!g_application_command_line_get_is_remote(line)) g_application_quit(G_APPLICATION(app_));
                g_strfreev(args); return 0;
            } else {
                g_application_command_line_printerr(line, "Unknown option: %s\n", args[i]);
                if (!g_application_command_line_get_is_remote(line)) g_application_quit(G_APPLICATION(app_));
                g_strfreev(args); return 2;
            }
        }
        g_strfreev(args); if (!command.empty()) action(command); return 0;
    }
    void action(const std::string &command) {
        if (!core_) return;
        if (command == "ShowSettings") { show_settings(); return; }
        if (command == "Quit") { core_->cancel(); g_application_quit(G_APPLICATION(app_)); return; }
        if (command == "ClearResult") { error_ = false; clear_result(); message_ = "Copied. Paste into your field."; changed(); return; }
        if (command == "Copy") {
            if (!result_.empty()) {
                auto *display = gdk_display_get_default();
                if (display) { gdk_clipboard_set_text(gdk_display_get_clipboard(display), result_.c_str()); clear_result(); message_ = "Copied. Paste into your field."; changed(); }
            }
            return;
        }
        if (command == "Cancel") {
            core_->cancel(); error_ = false; clear_result(); message_ = "Canceled. Ready for a new recording."; changed(); return;
        }
        if (command == "Stop" || (command == "Toggle" && core_->phase() == lv::WorkflowPhase::recording)) {
            auto *uuid = g_uuid_string_random(); core_->stop(uuid); g_free(uuid); return;
        }
        if (command == "Toggle" && core_->phase() == lv::WorkflowPhase::idle) {
            error_ = false; clear_result();
            lv::WorkflowConfig config;
            config.preferences = {settings_.endpoint, settings_.asr_model, settings_.cleanup_model, settings_.cleanup_prompt, false};
            native::endpoint_facts(settings_.endpoint, config.endpoint);
            core_->start(std::move(config));
        }
    }
    void clear_result() {
        if (result_expiry_) { g_source_remove(result_expiry_); result_expiry_ = 0; }
        result_.clear();
    }
    void refresh_palette() {
        // Read native libadwaita semantic colors; never invent a private palette.
        // These supported GTK4 APIs are deprecated in newer GTK, but are the
        // available named-color bridge on the Ubuntu 24.04 baseline.
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        auto *widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        g_object_ref_sink(widget);
        auto *context = gtk_widget_get_style_context(widget);
        std::string palette;
        for (const char *name : {"window_bg_color", "window_fg_color", "card_bg_color", "card_fg_color"}) {
            GdkRGBA color {};
            if (!gtk_style_context_lookup_color(context, name, &color)) { palette.clear(); break; }
            char hex[10];
            auto channel = [](float value) { return static_cast<unsigned>(std::lround(std::fmax(0.0, std::fmin(1.0, static_cast<double>(value))) * 255.0)); };
            std::snprintf(hex, sizeof(hex), "#%02x%02x%02x%02x", channel(color.red), channel(color.green), channel(color.blue), channel(color.alpha));
            if (!palette.empty()) palette += ' ';
            palette += hex;
        }
        palette_ = std::move(palette); g_object_unref(widget);
        G_GNUC_END_IGNORE_DEPRECATIONS
    }
    void changed() {
        auto *bus = g_application_get_dbus_connection(G_APPLICATION(app_));
        if (bus && registration_) {
            GVariantBuilder properties; g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&properties, "{sv}", "State", g_variant_new_string(state_.c_str()));
            g_variant_builder_add(&properties, "{sv}", "Message", g_variant_new_string(message_.c_str()));
            g_variant_builder_add(&properties, "{sv}", "Result", g_variant_new_string(result_.c_str()));
            g_variant_builder_add(&properties, "{sv}", "Palette", g_variant_new_string(palette_.c_str()));
            g_variant_builder_add(&properties, "{sv}", "Error", g_variant_new_boolean(error_));
            g_dbus_connection_emit_signal(bus, nullptr, bus_path, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                g_variant_new("(sa{sv}as)", bus_interface, &properties, nullptr), nullptr);
        }
        if (window_) window_->state(message_, state_ == "recording", state_ != "idle", !result_.empty());
    }
    void show_settings() {
        if (!window_) {
            window_ = std::make_unique<native::SettingsWindow>(GTK_APPLICATION(app_), settings_,
                [this](const native::Settings &value, std::string &error) {
                    if (!native::endpoint_is_valid(value.endpoint)) { error = "Use a loopback HTTP/HTTPS endpoint ending in /v1, without credentials, query or fragment."; return false; }
                    if (!store_.save(value, error)) return false;
                    settings_ = value; diagnostics_.set_debug(settings_.debug);
                    native::apply_appearance(settings_); native::apply_extension_shortcuts(settings_);
                    refresh_palette(); changed(); return true;
                }, [this](const std::string &endpoint) { refresh(endpoint); }, [this]() {
                    if (!g_file_test(diagnostics_.path().c_str(), G_FILE_TEST_EXISTS)) {
                        window_->notice("No log has been recorded yet. Errors will create a private log."); return;
                    }
                    auto *file = g_file_new_for_path(diagnostics_.path().c_str());
                    auto *uri = g_file_get_uri(file);
                    g_app_info_launch_default_for_uri_async(uri, nullptr, nullptr, nullptr, nullptr);
                    g_free(uri); g_object_unref(file);
                }, [this](const std::string &name) { action(name); });
            refresh(settings_.endpoint);
        }
        changed(); window_->present();
    }
    void refresh(const std::string &endpoint) {
        discovery_.cancel(discovery_operation_);
        const auto operation = ++discovery_operation_;
        lv::ProtocolConfiguration config; native::endpoint_facts(endpoint, config.endpoint);
        lv::TransportRequest request;
        if (lv::Protocol::make_models_request(config, request) != LV_PROTOCOL_OK) {
            window_->models({}, "Use a local endpoint ending in /v1."); return;
        }
        discovery_.send(operation, std::move(request), [this, operation](lv::OperationId id, lv::TransportResponse response) {
            if (id != operation || id != discovery_operation_ || !window_) return;
            lv::ProtocolInventory inventory;
            if (response.error != lv::AdapterError::none || response.status != 200 || lv::Protocol::parse_inventory(response.bytes, inventory) != LV_PROTOCOL_OK) {
                window_->models({}, "Could not load models. Check Lemonade, then Refresh."); return;
            }
            window_->models(std::move(inventory.models), "Models refreshed. Save settings to use your selections.");
        });
    }
    void shutdown() {
        if (shutdown_) return;
        shutdown_ = true;
        if (core_) core_->shutdown();
        discovery_.shutdown(); clear_result();
        if (capture_limit_) { g_source_remove(capture_limit_); capture_limit_ = 0; }
        if (palette_source_) { g_source_remove(palette_source_); palette_source_ = 0; }
        if (style_signal_) { g_signal_handler_disconnect(adw_style_manager_get_default(), style_signal_); style_signal_ = 0; }
        if (registration_) {
            auto *bus = g_application_get_dbus_connection(G_APPLICATION(app_));
            if (bus) g_dbus_connection_unregister_object(bus, registration_);
            registration_ = 0;
        }
        window_.reset(); core_.reset();
        if (node_) { g_dbus_node_info_unref(node_); node_ = nullptr; }
    }
    AdwApplication *app_;
    native::GlibExecutor executor_;
    native::PipeWireCaptureAdapter capture_;
    native::SoupByteTransport transport_, discovery_;
    native::ManualCopyInsertionAdapter insertion_;
    native::PrivateDiagnostics diagnostics_;
    native::SettingsStore store_;
    native::Settings settings_;
    std::unique_ptr<lv::WorkflowCore> core_;
    std::unique_ptr<native::SettingsWindow> window_;
    GDBusNodeInfo *node_ = nullptr;
    guint registration_ = 0, result_expiry_ = 0, palette_source_ = 0, capture_limit_ = 0;
    gulong style_signal_ = 0;
    lv::OperationId discovery_operation_ = 0;
    bool shutdown_ = false, error_ = false;
    std::string state_ = "idle", message_ = "Ready. Start dictation with the Shell extension or Settings.", result_, palette_;
};
}
int main(int argc, char **argv) {
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
#ifdef LEMONVOX_VERSION
        std::printf("LemonVox %s (build %s)\n", LEMONVOX_VERSION, LEMONVOX_BUILD);
#else
        std::printf("LemonVox development\n");
#endif
        return 0;
    }
    gst_init(nullptr, nullptr);
    Application app; return app.run(argc, argv);
}
