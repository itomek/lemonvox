// lemonvox_ui_demo: the #23 native host proof. A worker thread drives the
// tray icon and indicator through every state via UiThreadExecutor, proving
// UI-thread dispatch, a nonactivating indicator and orderly shutdown.
//
//   lemonvox_ui_demo           loop the script until Quit is chosen in the tray menu
//   lemonvox_ui_demo --smoke   run the script once (fast), verify, exit 0 on success

#include "common/executor.h"
#include "support/interactive_lock.h"
#include "ui/indicator.h"
#include "ui/tray.h"

#include <windows.h>

#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using lemonvox::win::UiThreadExecutor;
using namespace lemonvox::win::ui;

namespace {

struct Step {
    std::chrono::milliseconds hold;
    IndicatorState indicator;
    TrayStatus tray;
};

IndicatorState indicator(IndicatorMode mode, std::wstring message = {}, std::wstring action = {}) {
    IndicatorState state;
    state.mode = mode;
    state.message = std::move(message);
    state.action_label = std::move(action);
    return state;
}

std::vector<Step> script() {
    return {
        {600ms, indicator(IndicatorMode::preparing), {TrayActivity::processing, false, {}}},
        {2400ms, indicator(IndicatorMode::recording), {TrayActivity::recording, false, {}}},
        {900ms, indicator(IndicatorMode::transcribing), {TrayActivity::processing, false, {}}},
        {700ms, indicator(IndicatorMode::cleaning), {TrayActivity::processing, false, {}}},
        {400ms, indicator(IndicatorMode::inserting), {TrayActivity::processing, false, {}}},
        {1200ms, indicator(IndicatorMode::success), {TrayActivity::idle, false, {}}},
        {2500ms,
         indicator(IndicatorMode::error, L"Microphone access is turned off for desktop apps.",
                   L"Open microphone settings"),
         {TrayActivity::error, false, L"Microphone access is off"}},
        {2500ms,
         indicator(IndicatorMode::copy_fallback, L"Couldn\u2019t insert into this app. The text is ready to copy."),
         {TrayActivity::idle, true, {}}},
        {400ms, indicator(IndicatorMode::hidden), {TrayActivity::idle, false, {}}},
    };
}

// Nonactivation evidence collected on the UI thread (and the WinEvent hook,
// which is delivered out of context to this same thread).
struct Probe {
    HWND indicator = nullptr;
    unsigned checks = 0;
    unsigned violations = 0;
    unsigned off_thread = 0;
    unsigned foreground_events = 0;

    void fail(const char *what) {
        ++violations;
        std::printf("FAIL: %s\n", what);
    }
    void check(const IndicatorWindow &window) {
        ++checks;
        HWND foreground = GetForegroundWindow();
        if (foreground && foreground == window.hwnd()) fail("indicator became the foreground window");
        if (GetActiveWindow() == window.hwnd() && window.hwnd()) fail("indicator became the active window");
        if (GetFocus() == window.hwnd() && window.hwnd()) fail("indicator received keyboard focus");
        if (window.activation_count() != 0) fail("indicator received WM_ACTIVATE/WM_SETFOCUS");
        if (foreground) {
            DWORD pid = 0;
            GetWindowThreadProcessId(foreground, &pid);
            if (pid == GetCurrentProcessId()) fail("demo process took the foreground");
        }
        const LONG_PTR ex = GetWindowLongPtrW(window.hwnd(), GWL_EXSTYLE);
        const LONG_PTR required = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED;
        if ((ex & required) != required) fail("indicator lost a required extended style");
        if ((window.state().mode != IndicatorMode::hidden) != window.visible()) fail("indicator visibility mismatch");
    }
};

Probe g_probe;

void CALLBACK on_foreground(HWINEVENTHOOK, DWORD, HWND window, LONG, LONG, DWORD, DWORD) {
    ++g_probe.foreground_events;
    if (window && window == g_probe.indicator) g_probe.fail("EVENT_SYSTEM_FOREGROUND reported the indicator");
}

void CALLBACK on_watchdog(HWND, UINT, UINT_PTR, DWORD) {
    g_probe.fail("smoke run timed out");
    PostQuitMessage(3);
}

int run(HINSTANCE instance, bool smoke) {
    UiThreadExecutor executor;
    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::condition_variable wake;
    const auto request_stop = [&] {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        wake.notify_all();
    };

    TrayIcon *tray_ptr = nullptr;
    IndicatorWindow indicator_window(IndicatorCallbacks{
        [] { OutputDebugStringW(L"LemonVox demo: cancel\n"); },
        [] { OutputDebugStringW(L"LemonVox demo: stop\n"); },
        [] { OutputDebugStringW(L"LemonVox demo: copy\n"); },
        [] { OutputDebugStringW(L"LemonVox demo: dismiss\n"); },
        [] { OutputDebugStringW(L"LemonVox demo: action\n"); },
        [](POINT anchor) {
            wchar_t line[96];
            std::swprintf(line, 96, L"LemonVox demo: anchor %ld,%ld\n", anchor.x, anchor.y);
            OutputDebugStringW(line);
        },
    });
    TrayIcon tray(TrayCallbacks{
        [] {},
        [] {},
        [] {},
        [&tray_ptr] {
            if (tray_ptr) tray_ptr->notify(L"LemonVox", L"Settings are provided by the settings module.", false);
        },
        {},
        [&] {
            request_stop();
            PostQuitMessage(0);
        },
    });
    tray_ptr = &tray;
    if (!indicator_window.create(instance) || !tray.create(instance)) {
        std::printf("FAIL: could not create the indicator or tray windows\n");
        return 1;
    }
    indicator_window.set_auto_hide_delay(smoke ? 150ms : 900ms);
    g_probe.indicator = indicator_window.hwnd();

    HWINEVENTHOOK hook = nullptr;
    UINT_PTR watchdog = 0;
    if (smoke) {
        if (!tray.icon_added()) g_probe.fail("tray icon was not added");
        hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, on_foreground, 0, 0,
                               WINEVENT_OUTOFCONTEXT);
        watchdog = SetTimer(nullptr, 0, 60000, on_watchdog);
    }

    const auto post_checked = [&](std::function<void()> task) {
        executor.post([&, task = std::move(task)] {
            if (!executor.is_owner_thread()) ++g_probe.off_thread;
            task();
            if (smoke) g_probe.check(indicator_window);
        });
    };

    std::thread worker([&] {
        const auto wait = [&](std::chrono::milliseconds duration) {
            std::unique_lock<std::mutex> lock(mutex);
            return !wake.wait_for(lock, duration, [&] { return stop.load(); });
        };
        do {
            for (const Step &step : script()) {
                if (stop) break;
                post_checked([&indicator_window, &tray, step] {
                    indicator_window.set_state(step.indicator);
                    tray.set_status(step.tray);
                });
                const auto hold = smoke ? step.hold / 4 : step.hold;
                if (step.indicator.mode != IndicatorMode::recording) {
                    if (!wait(hold)) break;
                    continue;
                }
                const auto start = std::chrono::steady_clock::now();
                for (int frame = 0;; ++frame) {
                    const auto elapsed =
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
                    if (elapsed >= hold || !wait(40ms)) break;
                    const double level = 0.35 + 0.6 * std::fabs(std::sin(frame * 0.37));
                    // Show realistic elapsed time even in the compressed smoke run.
                    const auto shown = smoke ? elapsed * 4 : elapsed;
                    post_checked([&indicator_window, level, shown] { indicator_window.set_level(level, shown); });
                }
            }
        } while (!smoke && !stop);
        executor.post([] { PostQuitMessage(0); });
    });

    MSG message;
    int exit_code = 0;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (message.message == WM_QUIT) exit_code = static_cast<int>(message.wParam);

    // Orderly shutdown: stop the producer, retire the executor so no queued task
    // can touch a window, then destroy the windows on their owner thread.
    request_stop();
    worker.join();
    executor.shutdown();
    const bool rejected_after_shutdown = !executor.post([] {});
    if (watchdog) KillTimer(nullptr, watchdog);
    if (hook) UnhookWinEvent(hook);
    indicator_window.destroy();
    tray.destroy();

    if (!smoke) return exit_code;
    if (!rejected_after_shutdown) g_probe.fail("executor accepted work after shutdown");
    if (g_probe.off_thread != 0) g_probe.fail("a UI task ran off the UI thread");
    if (g_probe.checks < 20) g_probe.fail("too few state transitions were verified");
    std::printf("ui_demo smoke: %u checks, %u foreground events observed, %u violations\n", g_probe.checks,
                g_probe.foreground_events, g_probe.violations);
    std::fflush(stdout);
    return g_probe.violations == 0 && exit_code == 0 ? 0 : 1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    bool smoke = false;
    int argc = 0;
    if (LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int index = 1; index < argc; ++index) smoke = smoke || std::wstring(argv[index]) == L"--smoke";
        LocalFree(argv);
    }
    if (!smoke) return run(instance, false);
    // The smoke run shows windows on the real desktop; serialize with other
    // interactive tests so their foreground checks are not disturbed.
    lemonvox::win::test::InteractiveLock lock;
    return run(instance, true);
}
