// LemonVox for Windows: tray utility that binds the global trigger, the
// nonactivating indicator, Settings and the shared dictation workflow.
#include "app/dictation_controller.h"
#include "app/file_capture.h"
#include "capture/device_list.h"
#include "capture/wasapi_capture.h"
#include "common/executor.h"
#include "common/utf.h"
#include "insertion/clipboard.h"
#include "insertion/integrity.h"
#include "insertion/win_insertion.h"
#include "net/endpoint_facts.h"
#include "model_setup_client.h"
#include <map>
#include "net/server_health.h"
#include "net/winhttp_transport.h"
#include "prefs/preferences.h"
#include "trigger/global_trigger.h"
#include "ui/indicator.h"
#include "ui/settings_window.h"
#include "ui/tray.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace lemonvox;
using namespace lemonvox::win;

namespace {

constexpr wchar_t kInstanceMutex[] = L"Local\\LemonVox.Windows.SingleInstance";
constexpr wchar_t kSignalClass[] = L"LemonVoxInstanceSignal";
constexpr UINT kOpenSettings = WM_APP + 40;


CaptureProblem capture_problem(CaptureFailureReason reason) {
    switch (reason) {
    case CaptureFailureReason::none: return CaptureProblem::none;
    case CaptureFailureReason::permission_denied: return CaptureProblem::permission_denied;
    case CaptureFailureReason::no_device: return CaptureProblem::no_device;
    case CaptureFailureReason::device_busy: return CaptureProblem::device_busy;
    case CaptureFailureReason::device_lost: return CaptureProblem::device_lost;
    case CaptureFailureReason::format:
    case CaptureFailureReason::internal: return CaptureProblem::other;
    }
    return CaptureProblem::other;
}

std::wstring g_registry_key = L"Software\\LemonVox";

std::optional<POINT> load_anchor() {
    DWORD x = 0, y = 0, size = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER, g_registry_key.c_str(), L"IndicatorX", RRF_RT_REG_DWORD, nullptr, &x, &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    size = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER, g_registry_key.c_str(), L"IndicatorY", RRF_RT_REG_DWORD, nullptr, &y, &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return POINT{static_cast<LONG>(x), static_cast<LONG>(y)};
}

void save_anchor(POINT point) {
    const DWORD x = static_cast<DWORD>(point.x);
    const DWORD y = static_cast<DWORD>(point.y);
    RegSetKeyValueW(HKEY_CURRENT_USER, g_registry_key.c_str(), L"IndicatorX", REG_DWORD, &x, sizeof(x));
    RegSetKeyValueW(HKEY_CURRENT_USER, g_registry_key.c_str(), L"IndicatorY", REG_DWORD, &y, sizeof(y));
}

void open_uri(const wchar_t *uri) {
    ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
}

LRESULT CALLBACK signal_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kOpenSettings) {
        auto *open = reinterpret_cast<std::function<void()> *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (open && *open) (*open)();
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

// UI-thread timers adapt the shared setup controller to the Win32 run loop.
class SetupTimers {
public:
    static void schedule(unsigned delay, std::function<void()> callback) {
        const auto id = SetTimer(nullptr, 0, delay, +[](HWND, UINT, UINT_PTR token, DWORD) {
            KillTimer(nullptr, token);
            auto found = callbacks().find(token);
            if (found == callbacks().end()) return;
            auto done = std::move(found->second); callbacks().erase(found); done();
        });
        if (id) callbacks().emplace(id, std::move(callback));
    }
    static void shutdown() {
        for (const auto &entry : callbacks()) KillTimer(nullptr, entry.first);
        callbacks().clear();
    }
private:
    static std::map<UINT_PTR, std::function<void()>> &callbacks() {
        static std::map<UINT_PTR, std::function<void()>> value; return value;
    }
};

class App {
public:
    App(HINSTANCE instance, std::wstring registry_key, std::vector<std::uint8_t> sample_wav)
        : instance_(instance), store_(std::move(registry_key)), sample_wav_(std::move(sample_wav)) {}

    bool run(bool open_settings_at_launch, bool smoke);

private:
    static void CALLBACK foreground_changed(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG child, DWORD, DWORD);
    void on_foreground(HWND window);
    void on_phase(WorkflowPhase phase);
    void show_failure(const UserMessage &message);
    void open_settings();
    void apply_trigger_at_launch();
    void refresh_tray();
    void copy_pending();
    void quit();

    HINSTANCE instance_;
    PreferencesStore store_;
    UiThreadExecutor executor_;
    std::unique_ptr<SnapshotPreferences> preferences_;
    std::vector<std::uint8_t> sample_wav_;
    std::unique_ptr<WasapiCapture> capture_;
    // Debug/test source that replays a WAV instead of recording the microphone.
    std::unique_ptr<FileCapture> sample_capture_;
    std::unique_ptr<WinHttpTransport> transport_;
    std::unique_ptr<WinHttpTransport> catalog_transport_;
    std::unique_ptr<lemonvox::ModelSetupClient> setup_;
    std::function<void(SettingsCatalogResult)> setup_catalog_done_;
    std::function<void(std::wstring, bool, bool)> setup_install_done_;
    std::unique_ptr<WinHttpTransport> health_transport_;
    std::unique_ptr<ServerHealthMonitor> health_;
    UINT_PTR health_timer_ = 0;
    std::unique_ptr<WinInsertion> insertion_;
    std::unique_ptr<DictationController> controller_;
    std::unique_ptr<GlobalTrigger> trigger_;
    std::unique_ptr<ui::IndicatorWindow> indicator_;
    std::unique_ptr<ui::TrayIcon> tray_;
    std::unique_ptr<SettingsWindow> settings_;
    std::function<void()> open_settings_signal_;
    HWND signal_window_ = nullptr;
    FailureAction pending_action_ = FailureAction::none;
    std::chrono::steady_clock::time_point recording_started_{};
    ui::TrayActivity activity_ = ui::TrayActivity::idle;
    std::wstring tray_detail_;
    bool quitting_ = false;
    HWINEVENTHOOK foreground_hook_ = nullptr;
    HWND elevated_noticed_ = nullptr;
    static App *instance_app_;
};

App *App::instance_app_ = nullptr;

void CALLBACK App::foreground_changed(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG child, DWORD, DWORD) {
    if (object == OBJID_WINDOW && child == CHILDID_SELF && instance_app_) instance_app_->on_foreground(window);
}

// Windows does not deliver keystrokes aimed at higher-integrity windows to a
// medium-integrity low-level hook (UIPI), so a modifier double tap cannot be
// seen there. Say so once per window instead of silently doing nothing.
// RegisterHotKey shortcuts and the tray command still work; insertion then
// falls back to Copy.
void App::on_foreground(HWND window) {
    if (!window || window == elevated_noticed_ || controller_->busy()) return;
    const auto binding = trigger_->active();
    if (!binding || binding->kind != TriggerKind::double_tap) return;
    if (indicator_->visible() && indicator_->state().mode != ui::IndicatorMode::notice) return;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process == 0 || process == GetCurrentProcessId()) return;
    // An unreadable token is typical for elevated processes of the same user.
    if (input_integrity(process) == IntegrityDecision::allowed) return;
    elevated_noticed_ = window;
    ui::IndicatorState state;
    state.mode = ui::IndicatorMode::notice;
    state.message = L"Administrator window: " + describe_trigger(*binding)
        + L" can't be detected here. Use Start dictation in the tray menu or a regular shortcut; "
          L"the text will be offered to copy.";
    indicator_->set_state(state);
}

bool App::run(bool open_settings_at_launch, bool smoke) {
    preferences_ = std::make_unique<SnapshotPreferences>(store_.load());
    const WinPreferences &current = preferences_->current();

    capture_ = std::make_unique<WasapiCapture>(executor_, current.capture_device_id);
    transport_ = std::make_unique<WinHttpTransport>(executor_);
    catalog_transport_ = std::make_unique<WinHttpTransport>(executor_);
    InsertionOptions insertion_options;
    insertion_ = std::make_unique<WinInsertion>(executor_, std::move(insertion_options));

    ui::IndicatorCallbacks indicator_callbacks;
    indicator_callbacks.cancel = [this] { controller_->cancel(); indicator_->set_state({}); };
    indicator_callbacks.stop = [this] { controller_->stop(); };
    indicator_callbacks.copy = [this] { copy_pending(); };
    indicator_callbacks.dismiss = [this] { controller_->discard_pending(); refresh_tray(); };
    indicator_callbacks.action = [this] {
        const FailureAction action = pending_action_;
        indicator_->set_state({});
        if (action == FailureAction::open_microphone_privacy) open_uri(L"ms-settings:privacy-microphone");
        else if (action == FailureAction::open_settings) open_settings();
    };
    indicator_callbacks.position_changed = [](POINT point) { save_anchor(point); };
    indicator_ = std::make_unique<ui::IndicatorWindow>(std::move(indicator_callbacks));
    if (!indicator_->create(instance_)) return false;
    indicator_->set_anchor(load_anchor());

    ui::TrayCallbacks tray_callbacks;
    tray_callbacks.toggle_dictation = [this] { controller_->toggle(); };
    tray_callbacks.cancel = [this] { controller_->cancel(); indicator_->set_state({}); };
    tray_callbacks.copy_last = [this] { copy_pending(); };
    tray_callbacks.open_settings = [this] { open_settings(); };
    tray_callbacks.open_diagnostics = [this] {
        const std::wstring log = controller_->diagnostics().format();
        MessageBoxW(nullptr, log.empty() ? L"No dictation events yet." : log.c_str(),
            L"LemonVox diagnostics (content-free)", MB_OK | MB_ICONINFORMATION);
    };
    tray_callbacks.quit = [this] { quit(); };
    tray_ = std::make_unique<ui::TrayIcon>(std::move(tray_callbacks));
    if (!tray_->create(instance_)) return false;

    ControllerEvents events;
    events.phase = [this](WorkflowPhase phase) { on_phase(phase); };
    events.failed = [this](const UserMessage &message) { show_failure(message); };
    events.inserted = [this] {
        ui::IndicatorState state;
        state.mode = ui::IndicatorMode::success;
        state.message = L"Inserted";
        indicator_->set_state(state);
        refresh_tray();
    };
    events.copy_fallback = [this](const std::wstring &reason) {
        ui::IndicatorState state;
        state.mode = ui::IndicatorMode::copy_fallback;
        state.message = reason;
        indicator_->set_state(state);
        refresh_tray();
    };
    events.finished = [this](OperationId operation) { insertion_->release(operation); };
    if (!sample_wav_.empty()) sample_capture_ = std::make_unique<FileCapture>(executor_, sample_wav_);
    CaptureAdapter &capture_source = sample_capture_ ? static_cast<CaptureAdapter &>(*sample_capture_) : *capture_;
    controller_ = std::make_unique<DictationController>(capture_source, *transport_, *insertion_, *preferences_,
        std::move(events), [this] { return capture_problem(capture_->last_failure_reason()); });
    capture_->set_level_callback([this](float level) {
        if (controller_->phase() != WorkflowPhase::recording) return;
        indicator_->set_level(level, std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - recording_started_));
    });
    capture_->set_failure_callback([this](OperationId, CaptureFailureReason) { controller_->capture_failed(); });

    trigger_ = std::make_unique<GlobalTrigger>(executor_, [this] { controller_->toggle(); });
    apply_trigger_at_launch();

    SettingsServices services;
    setup_ = std::make_unique<lemonvox::ModelSetupClient>(*catalog_transport_, SetupTimers::schedule,
        [this](lemonvox::ModelSetupClient::Snapshot view) {
            if (setup_catalog_done_) {
                SettingsCatalogResult result;
                result.status = view.available ? SettingsCatalogResult::Status::ok : SettingsCatalogResult::Status::network;
                result.installation_supported = view.management_supported;
                result.setup_active = view.active;
                result.message = utf8_to_wide(view.message);
                for (const auto &model : view.catalog.models)
                    result.models.push_back({model.id, model.asr, model.cleanup, model.remote, model.downloaded, model.loaded, model.loaded_known, model.size_gb});
                setup_catalog_done_(std::move(result));
            }
            if (setup_install_done_) {
                auto done = setup_install_done_;
                if (!view.active) setup_install_done_ = {};
                done(utf8_to_wide(view.message), !view.active, !view.installed_id.empty());
            }
        });
    services.refresh_models = [this](const std::string &endpoint, std::function<void(SettingsCatalogResult)> done) {
        setup_catalog_done_ = std::move(done);
        lemonvox::ProtocolConfiguration config; endpoint_facts(endpoint, config.endpoint);
        setup_->refresh(std::move(config));
    };
    services.install_model = [this](const std::string &endpoint, const std::string &id, bool asr, std::function<void(std::wstring, bool, bool)> done) {
        lemonvox::ProtocolEndpointFacts facts;
        if (!endpoint_is_valid(endpoint, &facts) || facts.canonical_base_url != setup_->snapshot().endpoint) {
            done(L"Server endpoint changed. Refresh models before setup.", true, false); return;
        }
        setup_install_done_ = std::move(done);
        setup_->install(id, asr ? LV_MODEL_ASR : LV_MODEL_CLEANUP, true);
    };
    services.cancel_install = [this] { setup_->cancel(); };
    services.list_devices = [] {
        std::vector<SettingsDevice> devices;
        for (const auto &device : list_capture_devices().devices) devices.push_back({device.id, device.name});
        return devices;
    };
    services.apply_trigger = [this](const TriggerBinding &binding, std::wstring *error) {
        return trigger_->apply(binding, error);
    };
    services.set_trigger_suspended = [this](bool suspended) { trigger_->set_suspended(suspended); };
    services.save = [this](const WinPreferences &updated) {
        if (!store_.save(updated)) return false;
        // Applies to the next operation; a running one keeps its snapshot.
        preferences_->replace(updated);
        capture_->set_device_id(updated.capture_device_id);
        health_->set_endpoint(updated.endpoint);
        return true;
    };
    services.open_microphone_privacy = [] { open_uri(L"ms-settings:privacy-microphone"); };
    settings_ = std::make_unique<SettingsWindow>(instance_, std::move(services), kDefaultCleanupPrompt);

    open_settings_signal_ = [this] { open_settings(); };
    instance_app_ = this;
    WinHttpTransport::Options health_options;
    health_options.default_request_timeout_ms = 3000;
    health_options.resource_timeout_ms = 3000;
    health_transport_ = std::make_unique<WinHttpTransport>(executor_, health_options);
    health_ = std::make_unique<ServerHealthMonitor>(*health_transport_, [] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }, [this](LVServerHealthStatus status) {
        if (settings_) settings_->set_server_health(status);
    });
    health_->set_endpoint(current.endpoint);
    health_timer_ = SetTimer(nullptr, 0, 1000, [](HWND, UINT, UINT_PTR, DWORD) {
        if (instance_app_ && instance_app_->health_) instance_app_->health_->tick();
    });
    foreground_hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
        &App::foreground_changed, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    WNDCLASSEXW signal_class{};
    signal_class.cbSize = sizeof(signal_class);
    signal_class.lpfnWndProc = signal_proc;
    signal_class.hInstance = instance_;
    signal_class.lpszClassName = kSignalClass;
    RegisterClassExW(&signal_class);
    signal_window_ = CreateWindowExW(0, kSignalClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance_, nullptr);
    if (signal_window_) SetWindowLongPtrW(signal_window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&open_settings_signal_));

    refresh_tray();
    const bool unconfigured = current.asr_model.empty() || current.cleanup_model.empty();
    if (open_settings_at_launch || (unconfigured && !smoke)) open_settings();

    int exit_code = 0;
    if (smoke) {
        // Startup proof: every component constructed, tray added, trigger live.
        const bool ok = tray_->hwnd() && indicator_->hwnd() && trigger_->active().has_value()
            && indicator_->activation_count() == 0;
        std::printf("smoke: tray=%d indicator=%d trigger=%d\n", tray_->hwnd() != nullptr,
            indicator_->hwnd() != nullptr, trigger_->active().has_value());
        exit_code = ok ? 0 : 1;
        executor_.post([this] { quit(); });
    }

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (settings_ && settings_->pre_translate(&message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    // Orderly shutdown: stop input sources, then the workflow (which shuts down
    // every adapter), then the executor, then windows.
    if (foreground_hook_) UnhookWinEvent(foreground_hook_);
    if (health_timer_) KillTimer(nullptr, health_timer_);
    health_->shutdown();
    health_transport_->shutdown();
    instance_app_ = nullptr;
    trigger_->shutdown();
    controller_->shutdown();
    SetupTimers::shutdown(); setup_.reset();
    catalog_transport_->shutdown();
    executor_.shutdown();
    settings_.reset();
    if (signal_window_) DestroyWindow(signal_window_);
    tray_->destroy();
    indicator_->destroy();
    return exit_code == 0;
}

void App::apply_trigger_at_launch() {
    std::wstring error;
    const TriggerBinding saved = preferences_->current().trigger;
    if (trigger_->apply(saved, &error)) return;
    std::wstring fallback_error;
    if (saved != default_trigger() && trigger_->apply(default_trigger(), &fallback_error)) {
        tray_detail_ = L"Shortcut unavailable; using Alt twice";
    } else {
        tray_detail_ = L"Shortcut unavailable";
    }
    tray_->notify(L"LemonVox shortcut unavailable", error.empty() ? tray_detail_ : error, true);
}

void App::on_phase(WorkflowPhase phase) {
    ui::IndicatorState state;
    state.mode = ui::indicator_mode_for_phase(phase);
    if (phase == WorkflowPhase::recording) recording_started_ = std::chrono::steady_clock::now();
    if (phase == WorkflowPhase::idle) {
        // Terminal callbacks (inserted/fallback/failed) set the final state.
        activity_ = ui::TrayActivity::idle;
        refresh_tray();
        if (indicator_->state().mode != ui::IndicatorMode::error
            && indicator_->state().mode != ui::IndicatorMode::copy_fallback) {
            indicator_->set_state({});
        }
        return;
    }
    activity_ = phase == WorkflowPhase::recording ? ui::TrayActivity::recording : ui::TrayActivity::processing;
    tray_detail_.clear();
    pending_action_ = FailureAction::none;
    indicator_->set_state(state);
    refresh_tray();
}

void App::show_failure(const UserMessage &message) {
    pending_action_ = message.action;
    ui::IndicatorState state;
    state.mode = ui::IndicatorMode::error;
    state.message = message.text;
    if (message.action == FailureAction::open_microphone_privacy) state.action_label = L"Open microphone settings";
    else if (message.action == FailureAction::open_settings) state.action_label = L"Open Settings";
    indicator_->set_state(state);
    activity_ = ui::TrayActivity::error;
    tray_detail_ = message.text;
    refresh_tray();
}

void App::open_settings() {
    if (settings_) settings_->show(preferences_->current());
}

void App::refresh_tray() {
    ui::TrayStatus status;
    status.activity = activity_;
    status.copy_available = controller_ && controller_->has_pending_text();
    status.detail = tray_detail_;
    tray_->set_status(status);
}

void App::copy_pending() {
    if (controller_->copy_pending([](const std::string &text) { return copy_text_to_clipboard(text); })) {
        ui::IndicatorState state;
        state.mode = ui::IndicatorMode::success;
        state.message = L"Copied \u00B7 paste with Ctrl+V";
        indicator_->set_state(state);
    }
    refresh_tray();
}

void App::quit() {
    if (quitting_) return;
    quitting_ = true;
    controller_->cancel();
    PostQuitMessage(0);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool smoke = false;
    bool settings = false;
    std::wstring registry_key = L"Software\\LemonVox";
    std::wstring sample_path;
    for (int index = 1; argv && index < argc; ++index) {
        const std::wstring arg = argv[index];
        if (arg == L"--smoke") smoke = true;
        else if (arg == L"--settings") settings = true;
        // Isolated HKCU profile for automated tests, e.g. Software\LemonVoxTest\e2e.
        else if (arg == L"--settings-key" && index + 1 < argc) registry_key = argv[++index];
        // Debug/test: replay a WAV instead of recording the microphone.
        else if (arg == L"--sample-wav" && index + 1 < argc) sample_path = argv[++index];
    }
    if (argv) LocalFree(argv);
    if (registry_key.rfind(L"Software\\", 0) != 0) return 2;
    g_registry_key = registry_key;
    std::vector<std::uint8_t> sample_wav;
    if (!sample_path.empty() && !FileCapture::load(sample_path, sample_wav)) return 2;
    if (smoke) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        FILE *stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
    }

    HANDLE instance_mutex = CreateMutexW(nullptr, FALSE, kInstanceMutex);
    if (instance_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        // A second launch opens Settings in the running instance.
        if (HWND existing = FindWindowExW(HWND_MESSAGE, nullptr, kSignalClass, nullptr)) {
            DWORD process = 0;
            GetWindowThreadProcessId(existing, &process);
            AllowSetForegroundWindow(process);
            PostMessageW(existing, kOpenSettings, 0, 0);
        }
        CloseHandle(instance_mutex);
        return smoke ? 1 : 0;
    }

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    {
        App app(instance, registry_key, std::move(sample_wav));
        ok = app.run(settings, smoke);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    if (instance_mutex) CloseHandle(instance_mutex);
    return ok ? 0 : 1;
}
