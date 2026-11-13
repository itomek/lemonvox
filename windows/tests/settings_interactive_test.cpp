// Opens the real Settings window, drives it with window messages and injected
// keyboard input, and inspects it through UI Automation. Labeled interactive:
// it needs an unlocked desktop and holds the shared interactive lock.

#include "common/executor.h"
#include "support/interactive_lock.h"
#include "ui/settings_layout.h"
#include "ui/settings_window.h"

#include <windows.h>

#include <commctrl.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <uiautomation.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' "     \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' "   \
                        "language='*'\"")

using namespace lemonvox::win;

#define CHECK(condition)                                                                         \
    do {                                                                                         \
        if (!(condition)) {                                                                      \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);   \
            std::exit(1);                                                                        \
        }                                                                                        \
    } while (0)

namespace {

constexpr char kDefaultPrompt[] = "Default prompt line 1\nline 2";

struct Planned {
    DWORD delay_ms;
    SettingsCatalogResult result;
};

struct Fake {
    UiThreadExecutor executor;
    std::vector<std::thread> workers;
    std::deque<Planned> plan;
    std::vector<std::string> endpoints;
    bool suspended = false;
    int suspend_calls = 0;
    bool apply_fails = false;
    std::vector<TriggerBinding> applied;
    std::optional<WinPreferences> saved;
    int save_calls = 0;
    int privacy_calls = 0;

    SettingsServices services() {
        SettingsServices services;
        services.refresh_models = [this](const std::string &endpoint,
                                      std::function<void(SettingsCatalogResult)> done) {
            endpoints.push_back(endpoint);
            Planned next{10, {}};
            next.result.status = SettingsCatalogResult::Status::network;
            if (!plan.empty()) {
                next = plan.front();
                plan.pop_front();
            }
            workers.emplace_back([this, next, done = std::move(done)] {
                Sleep(next.delay_ms);
                executor.post([done, result = next.result] { done(result); });
            });
        };
        services.list_devices = [] {
            return std::vector<SettingsDevice>{{L"{dev1}", L"Desk mic"}, {L"{dev2}", L"Headset"}};
        };
        services.apply_trigger = [this](const TriggerBinding &binding, std::wstring *error) {
            applied.push_back(binding);
            if (apply_fails && error) *error = L"already registered by another app";
            return !apply_fails;
        };
        services.set_trigger_suspended = [this](bool value) {
            suspended = value;
            ++suspend_calls;
        };
        services.save = [this](const WinPreferences &preferences) {
            saved.emplace(preferences);
            ++save_calls;
            return true;
        };
        services.open_microphone_privacy = [this] { ++privacy_calls; };
        return services;
    }

    void join() {
        for (auto &worker : workers) worker.join();
        workers.clear();
    }
};

SettingsWindow *g_window = nullptr;

void pump_once() {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (g_window && g_window->pre_translate(&message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

template <typename Predicate>
bool pump_until(Predicate predicate, DWORD timeout_ms = 3000) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        pump_once();
        if (predicate()) return true;
        if (GetTickCount64() >= deadline) return false;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
}

void pump_for(DWORD ms) {
    pump_until([] { return false; }, ms);
}

HWND item(const SettingsWindow &window, SettingsControl control) {
    return GetDlgItem(window.hwnd(), settings_control_id(control));
}

std::wstring text_of(HWND hwnd) {
    wchar_t buffer[1024] = {};
    GetWindowTextW(hwnd, buffer, 1024);
    return buffer;
}

std::vector<std::wstring> combo_items(HWND combo) {
    std::vector<std::wstring> items;
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        wchar_t buffer[512] = {};
        SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(buffer));
        items.push_back(buffer);
    }
    return items;
}

std::wstring combo_selected(HWND combo) {
    const int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (index < 0) return L"";
    return combo_items(combo)[static_cast<size_t>(index)];
}

void select_combo(HWND combo, const std::wstring &label) {
    const auto items = combo_items(combo);
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i] == label) {
            SendMessageW(combo, CB_SETCURSEL, i, 0);
            return;
        }
    }
    CHECK(!"combo item not found");
}

void click(const SettingsWindow &window, SettingsControl control) {
    SendMessageW(window.hwnd(), WM_COMMAND, MAKEWPARAM(settings_control_id(control), BN_CLICKED),
        reinterpret_cast<LPARAM>(item(window, control)));
    pump_once();
}

bool bring_to_foreground(HWND hwnd) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (GetForegroundWindow() == hwnd) return true;
        const HWND foreground = GetForegroundWindow();
        const DWORD foreground_thread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
        const DWORD self = GetCurrentThreadId();
        const bool attached = foreground_thread && foreground_thread != self
            && AttachThreadInput(self, foreground_thread, TRUE);
        ShowWindow(hwnd, SW_SHOWNORMAL);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        if (attached) AttachThreadInput(self, foreground_thread, FALSE);
        pump_for(50);
    }
    return GetForegroundWindow() == hwnd;
}

// Injects only after proving the Settings window is foreground; anything else
// could type into another app on the shared desktop.
void send_keys(HWND window, std::initializer_list<std::pair<int, bool>> keys) {
    if (!bring_to_foreground(window)) {
        std::fprintf(stderr, "FAIL: Settings window is not foreground; refusing to inject input\n");
        std::exit(1);
    }
    std::vector<INPUT> inputs;
    for (const auto &[vk, up] : keys) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = static_cast<WORD>(vk);
        input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC));
        input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        inputs.push_back(input);
    }
    CHECK(SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size());
    pump_for(150);
}

void focus(const SettingsWindow &window, SettingsControl control) {
    CHECK(bring_to_foreground(window.hwnd()));
    SetFocus(item(window, control));
    pump_once();
    CHECK(GetFocus() == item(window, control));
}

TriggerBinding combination(std::uint32_t modifiers, std::uint32_t vk) {
    TriggerBinding binding = default_trigger();
    binding.kind = TriggerKind::combination;
    binding.modifiers = modifiers;
    binding.virtual_key = vk;
    return binding;
}

SettingsCatalogResult models(std::vector<SettingsModelOption> options) {
    SettingsCatalogResult result;
    result.status = SettingsCatalogResult::Status::ok;
    result.models = std::move(options);
    return result;
}

// UI Automation must not run on the thread that owns the window, so query from
// a worker while this thread keeps pumping.
std::vector<std::string> check_automation(const SettingsWindow &window) {
    struct Expect {
        HWND hwnd;
        const wchar_t *name;
    };
    const std::vector<Expect> expectations{
        {item(window, SettingsControl::endpoint_edit), L"Server endpoint"},
        {item(window, SettingsControl::asr_combo), L"Transcription model"},
        {item(window, SettingsControl::cleanup_combo), L"Cleanup model"},
        {item(window, SettingsControl::prompt_edit), L"Cleanup prompt"},
        {item(window, SettingsControl::shortcut_combo), L"Shortcut"},
        {item(window, SettingsControl::recorder_edit), L"Custom shortcut"},
        {item(window, SettingsControl::microphone_combo), L"Microphone"},
        {item(window, SettingsControl::refresh_button), L"Refresh models"},
        {item(window, SettingsControl::save_button), L"Save"},
        {item(window, SettingsControl::cancel_button), L"Cancel"},
    };
    const HWND status = item(window, SettingsControl::status);
    std::vector<std::string> failures;
    std::atomic<bool> done{false};
    std::thread worker([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IUIAutomation *automation = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                __uuidof(IUIAutomation), reinterpret_cast<void **>(&automation)))) {
            failures.push_back("CUIAutomation unavailable");
        } else {
            for (const auto &expect : expectations) {
                IUIAutomationElement *element = nullptr;
                BSTR name = nullptr;
                if (FAILED(automation->ElementFromHandle(expect.hwnd, &element)) || !element
                    || FAILED(element->get_CurrentName(&name))) {
                    failures.push_back("no UIA element/name");
                } else if (!name || std::wstring(name) != expect.name) {
                    char buffer[512];
                    std::snprintf(buffer, sizeof(buffer), "UIA name '%ls' != '%ls'", name ? name : L"(null)",
                        expect.name);
                    failures.push_back(buffer);
                }
                if (name) SysFreeString(name);
                if (element) element->Release();
            }
            IUIAutomationElement *element = nullptr;
            VARIANT live;
            VariantInit(&live);
            if (FAILED(automation->ElementFromHandle(status, &element)) || !element
                || FAILED(element->GetCurrentPropertyValue(UIA_LiveSettingPropertyId, &live)) || live.vt != VT_I4
                || live.lVal != Polite) {
                failures.push_back("status is not a polite live region");
            }
            VariantClear(&live);
            if (element) element->Release();
            automation->Release();
        }
        CoUninitialize();
        done = true;
    });
    pump_until([&] { return done.load(); }, 20000);
    worker.join();
    return failures;
}

bool system_dark() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    return value == 0 && (contrast.dwFlags & HCF_HIGHCONTRASTON) == 0;
}

void check_common_controls_v6() {
    const HMODULE comctl = GetModuleHandleW(L"comctl32.dll");
    CHECK(comctl);
    const auto get_version = reinterpret_cast<DLLGETVERSIONPROC>(GetProcAddress(comctl, "DllGetVersion"));
    CHECK(get_version);
    DLLVERSIONINFO info{};
    info.cbSize = sizeof(info);
    CHECK(SUCCEEDED(get_version(&info)));
    CHECK(info.dwMajorVersion >= 6);
}

} // namespace

int main() {
    test::InteractiveLock lock;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
    INITCOMMONCONTROLSEX init{sizeof(init), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&init);
    check_common_controls_v6();

    Fake fake;
    WinPreferences initial{"http://127.0.0.1:13305/v1", "whisper-old", "llama", "Clean up.\nKeep meaning.",
        default_trigger(), L""};
    {
        SettingsWindow window(GetModuleHandleW(nullptr), fake.services(), kDefaultPrompt);
        g_window = &window;

        // Opens immediately, before the slow model load completes.
        fake.plan.push_back({300, models({{"whisper-b", true, false, false}, {"whisper-a", true, false, false},
                                         {"llama", false, true, false}, {"gpt-x", false, true, true}})});
        const auto started = std::chrono::steady_clock::now();
        window.show(initial);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        CHECK(window.is_open() && IsWindowVisible(window.hwnd()));
        CHECK(elapsed < std::chrono::milliseconds(200));
        std::printf("Settings visible after %lld ms\n",
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
        const auto prerequisite = text_of(item(window, SettingsControl::endpoint_hint));
        CHECK(prerequisite.find(L"Checking server") != std::wstring::npos);
        window.set_server_health(LV_SERVER_HEALTH_CONNECTED);
        CHECK(text_of(item(window, SettingsControl::endpoint_hint)).find(L"Server connected") != std::wstring::npos);
        window.set_server_health(LV_SERVER_HEALTH_DISCONNECTED);
        const auto disconnected = text_of(item(window, SettingsControl::endpoint_hint));
        CHECK(disconnected.find(L"Could not connect") != std::wstring::npos);
        CHECK(disconnected.find(L"Lemonade or another compatible OpenAI server") != std::wstring::npos);
        const auto capabilities = text_of(item(window, SettingsControl::routing_note));
        CHECK(capabilities.find(L"transcription, chat and discoverable compatible model metadata") != std::wstring::npos);
        CHECK(text_of(item(window, SettingsControl::status)) == L"Loading models from server\u2026");
        CHECK(combo_items(item(window, SettingsControl::asr_combo)) == std::vector<std::wstring>{L"whisper-old"});
        CHECK(fake.endpoints == std::vector<std::string>{"http://127.0.0.1:13305/v1"});
        CHECK(text_of(item(window, SettingsControl::endpoint_edit)) == L"http://127.0.0.1:13305/v1");
        CHECK(text_of(item(window, SettingsControl::prompt_edit)) == L"Clean up.\r\nKeep meaning.");
        CHECK(combo_selected(item(window, SettingsControl::shortcut_combo)) == L"Alt twice");
        CHECK(combo_items(item(window, SettingsControl::microphone_combo))
            == (std::vector<std::wstring>{L"System default", L"Desk mic", L"Headset"}));
        CHECK(combo_selected(item(window, SettingsControl::microphone_combo)) == L"System default");

        // Models arrive; the missing saved selection is preserved and flagged.
        CHECK(pump_until([&] {
            return text_of(item(window, SettingsControl::status)) != L"Loading models from server\u2026";
        }));
        CHECK(combo_items(item(window, SettingsControl::asr_combo))
            == (std::vector<std::wstring>{L"whisper-a (installed)", L"whisper-b (installed)", L"(unavailable) whisper-old"}));
        CHECK(combo_selected(item(window, SettingsControl::asr_combo)) == L"(unavailable) whisper-old");
        CHECK(combo_items(item(window, SettingsControl::cleanup_combo))
            == (std::vector<std::wstring>{L"gpt-x (remote via server)", L"llama (installed)"}));
        CHECK(combo_selected(item(window, SettingsControl::cleanup_combo)) == L"llama (installed)");
        CHECK(text_of(item(window, SettingsControl::status)) == L"2 transcription models and 2 cleanup models available.");

        // Single instance.
        const HWND first = window.hwnd();
        window.show(initial);
        CHECK(window.hwnd() == first);
        CHECK(fake.endpoints.size() == 1);

        // A slow older refresh is superseded by a newer one.
        fake.plan.push_back({500, models({{"stale-asr", true, false, false}, {"stale-cleanup", false, true, false}})});
        fake.plan.push_back({30, models({{"fresh-asr", true, false, false}, {"llama", false, true, false}})});
        click(window, SettingsControl::refresh_button);
        CHECK(text_of(item(window, SettingsControl::status)) == L"Loading models from server\u2026");
        click(window, SettingsControl::refresh_button);
        pump_for(800);
        CHECK(combo_items(item(window, SettingsControl::asr_combo))
            == (std::vector<std::wstring>{L"fresh-asr (installed)", L"(unavailable) whisper-old"}));
        CHECK(combo_items(item(window, SettingsControl::cleanup_combo)) == std::vector<std::wstring>{L"llama (installed)"});

        // Error state keeps the last inventory.
        fake.plan.push_back({20, SettingsCatalogResult{SettingsCatalogResult::Status::network, {}, L""}});
        click(window, SettingsControl::refresh_button);
        CHECK(pump_until([&] {
            return text_of(item(window, SettingsControl::status)).find(L"Server unavailable at http://127.0.0.1:13305/v1")
                == 0;
        }));
        CHECK(combo_items(item(window, SettingsControl::asr_combo)).front() == L"fresh-asr (installed)");

        // Tab order follows native child creation order and wraps. All three
        // visible, enabled setup actions sit between model selection and the
        // cleanup prompt, including Cancel setup (an idle no-op).
        const std::vector<SettingsControl> order{SettingsControl::endpoint_edit, SettingsControl::refresh_button,
            SettingsControl::asr_combo, SettingsControl::cleanup_combo,
            SettingsControl::install_asr_button, SettingsControl::install_cleanup_button,
            SettingsControl::cancel_install_button, SettingsControl::prompt_edit,
            SettingsControl::reset_prompt_button, SettingsControl::shortcut_combo, SettingsControl::recorder_edit,
            SettingsControl::microphone_combo, SettingsControl::privacy_button,
            SettingsControl::save_button, SettingsControl::cancel_button, SettingsControl::endpoint_edit};
        for (size_t i = 0; i + 1 < order.size(); ++i) {
            CHECK(IsWindowVisible(item(window, order[i])));
            CHECK(IsWindowEnabled(item(window, order[i])));
            CHECK(GetNextDlgTabItem(window.hwnd(), item(window, order[i]), FALSE) == item(window, order[i + 1]));
            CHECK(GetNextDlgTabItem(window.hwnd(), item(window, order[i + 1]), TRUE) == item(window, order[i]));
        }

        const auto failures = check_automation(window);
        for (const auto &failure : failures) std::fprintf(stderr, "UIA: %s\n", failure.c_str());
        CHECK(failures.empty());

        // Theme follows the system.
        CHECK(window.dark_mode_active() == system_dark());
        if (window.dark_mode_active()) {
            BOOL dark = FALSE;
            if (SUCCEEDED(DwmGetWindowAttribute(window.hwnd(), 20, &dark, sizeof(dark)))) CHECK(dark);
        }
        std::printf("dark mode: %s\n", window.dark_mode_active() ? "on" : "off");

        // Validation keeps the window open and explains.
        SetWindowTextW(item(window, SettingsControl::endpoint_edit), L"");
        click(window, SettingsControl::save_button);
        CHECK(window.is_open() && fake.save_calls == 0);
        CHECK(text_of(item(window, SettingsControl::status)).find(L"Enter the server endpoint") == 0);
        SetWindowTextW(item(window, SettingsControl::endpoint_edit), L"http://localhost:13305/v1");
        pump_once();
        CHECK(text_of(item(window, SettingsControl::status)).find(L"Endpoint changed.") == 0);

        SetWindowTextW(item(window, SettingsControl::prompt_edit), L"x");
        click(window, SettingsControl::reset_prompt_button);
        CHECK(text_of(item(window, SettingsControl::prompt_edit)) == L"Default prompt line 1\r\nline 2");
        click(window, SettingsControl::privacy_button);
        CHECK(fake.privacy_calls == 1);

        // Recording: Alt+letter is recorded, not treated as a mnemonic (Alt+S would Save).
        focus(window, SettingsControl::recorder_edit);
        CHECK(fake.suspended);
        CHECK(text_of(item(window, SettingsControl::recorder_edit)) == settings::kRecorderPrompt);
        send_keys(window.hwnd(), {{VK_MENU, false}, {'S', false}, {'S', true}, {VK_MENU, true}});
        CHECK(window.is_open() && fake.save_calls == 0);
        CHECK(combo_selected(item(window, SettingsControl::shortcut_combo)) == L"Alt+S");
        CHECK(!fake.suspended);

        focus(window, SettingsControl::recorder_edit);
        CHECK(fake.suspended);
        send_keys(window.hwnd(), {{VK_CONTROL, false}, {VK_SHIFT, false}, {'K', false}, {'K', true},
                                     {VK_SHIFT, true}, {VK_CONTROL, true}});
        CHECK(pump_until([&] {
            return combo_selected(item(window, SettingsControl::shortcut_combo)) == L"Ctrl+Shift+K";
        }));
        CHECK(!fake.suspended);
        CHECK(GetFocus() == item(window, SettingsControl::shortcut_combo));
        CHECK(text_of(item(window, SettingsControl::recorder_edit)) == L"Ctrl+Shift+K");

        // Esc cancels recording only, leaving the window and binding alone.
        focus(window, SettingsControl::recorder_edit);
        send_keys(window.hwnd(), {{VK_ESCAPE, false}, {VK_ESCAPE, true}});
        CHECK(window.is_open());
        CHECK(GetFocus() == item(window, SettingsControl::shortcut_combo));
        CHECK(combo_selected(item(window, SettingsControl::shortcut_combo)) == L"Ctrl+Shift+K");
        CHECK(!fake.suspended);
        // Shift alone plus a key is rejected and recording continues.
        focus(window, SettingsControl::recorder_edit);
        send_keys(window.hwnd(), {{VK_SHIFT, false}, {'Q', false}, {'Q', true}, {VK_SHIFT, true}});
        CHECK(fake.suspended);
        CHECK(text_of(item(window, SettingsControl::status)).find(L"A shortcut needs Ctrl, Alt or Windows") == 0);
        focus(window, SettingsControl::endpoint_edit);
        CHECK(!fake.suspended);

        select_combo(item(window, SettingsControl::asr_combo), L"fresh-asr (installed)");
        select_combo(item(window, SettingsControl::microphone_combo), L"Headset");

        // Enter = Save. A shortcut the app cannot register aborts the save.
        fake.apply_fails = true;
        focus(window, SettingsControl::endpoint_edit);
        send_keys(window.hwnd(), {{VK_RETURN, false}, {VK_RETURN, true}});
        CHECK(window.is_open() && fake.save_calls == 0);
        CHECK(fake.applied.size() == 1 && fake.applied[0] == combination(MOD_CONTROL | MOD_SHIFT, 'K'));
        CHECK(text_of(item(window, SettingsControl::status)).find(L"already registered by another app")
            != std::wstring::npos);

        fake.apply_fails = false;
        focus(window, SettingsControl::endpoint_edit);
        send_keys(window.hwnd(), {{VK_RETURN, false}, {VK_RETURN, true}});
        CHECK(pump_until([&] { return !window.is_open(); }));
        CHECK(fake.save_calls == 1 && fake.saved);
        const WinPreferences &saved = *fake.saved;
        CHECK(saved.endpoint == "http://localhost:13305/v1");
        CHECK(saved.asr_model == "fresh-asr");
        CHECK(saved.cleanup_model == "llama");
        CHECK(saved.cleanup_prompt == kDefaultPrompt);
        CHECK(saved.trigger == combination(MOD_CONTROL | MOD_SHIFT, 'K'));
        CHECK(saved.capture_device_id == L"{dev2}");
        CHECK(!fake.suspended);

        // The application can publish a newer health snapshot while Settings is closed.
        window.set_server_health(LV_SERVER_HEALTH_CONNECTED);
        // Esc = Cancel, and a refresh finishing after close is ignored.
        fake.plan.push_back({200, models({{"late", true, true, false}})});
        window.show(saved);
        CHECK(window.is_open());
        CHECK(text_of(item(window, SettingsControl::endpoint_hint)).find(L"Server connected") != std::wstring::npos);
        CHECK(combo_selected(item(window, SettingsControl::shortcut_combo)) == L"Ctrl+Shift+K");
        CHECK(combo_selected(item(window, SettingsControl::microphone_combo)) == L"Headset");
        focus(window, SettingsControl::prompt_edit);
        send_keys(window.hwnd(), {{VK_ESCAPE, false}, {VK_ESCAPE, true}});
        CHECK(pump_until([&] { return !window.is_open(); }));
        CHECK(fake.save_calls == 1);
        pump_for(400);
        g_window = nullptr;
    }
    fake.executor.drain();
    fake.join();
    fake.executor.drain();
    fake.executor.shutdown();
    CoUninitialize();
    std::puts("settings_interactive_test passed");
    return 0;
}
