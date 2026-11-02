#include "trigger/global_trigger.h"

#include "trigger/key_normalizer.h"

#include <windows.h>

#include <atomic>
#include <cassert>
#include <future>
#include <mutex>
#include <thread>

namespace lemonvox::win {

namespace {

constexpr UINT kApplyMessage = WM_APP + 1;
constexpr UINT kStopMessage = WM_APP + 2;
constexpr UINT_PTR kHealthTimer = 1;
constexpr UINT kHealthIntervalMs = 2000;
// GetLastInputInfo newer than our last hook callback by this much means the
// system saw input our hooks never received: Windows dropped them.
constexpr LONG kStaleHookMs = 1000;
constexpr wchar_t kWindowClass[] = L"LemonVoxTriggerListener";

struct Shared {
    std::atomic<bool> alive{true};
    std::atomic<bool> suspended{false};
    std::function<void()> on_trigger;
};

struct ApplyRequest {
    const TriggerBinding *binding = nullptr;
    std::wstring error;
    bool ok = false;
};

bool is_valid(const TriggerBinding &binding) {
    TriggerBinding parsed;
    return parse_trigger(serialize_trigger(binding), parsed) && parsed == binding;
}

std::wstring with_code(const std::wstring &text, DWORD code) {
    return text + L" (error " + std::to_wstring(code) + L")";
}

} // namespace

struct GlobalTrigger::Impl {
    Impl(Executor &executor_ref, std::function<void()> callback)
        : executor(executor_ref), shared(std::make_shared<Shared>()) {
        shared->on_trigger = std::move(callback);
    }

    Executor &executor;
    std::shared_ptr<Shared> shared;
    std::thread thread;
    DWORD thread_id = 0;
    HWND hwnd = nullptr;
    std::atomic<std::uint32_t> reset_generation{0};
    std::atomic<std::uint32_t> reinstalls{0};
    std::mutex control_mutex;
    mutable std::mutex state_mutex;
    std::optional<TriggerBinding> active_binding;
    std::wstring startup_error;

    // Listener-thread state.
    HHOOK keyboard_hook = nullptr;
    HHOOK mouse_hook = nullptr;
    HWINEVENTHOOK foreground_hook = nullptr;
    int hotkey_id = 0;
    KeyNormalizer normalizer;
    LVDoubleTapDetector detector{};
    std::uint32_t seen_generation = 0;
    DWORD last_hook_tick = 0;

    void run(std::promise<void> *ready);
    bool apply_on_thread(const TriggerBinding &binding, std::wstring *error);
    bool install_hooks(DWORD *code);
    void remove_hooks();
    void teardown();
    void health_check();
    void sync_reset();
    void on_key(WPARAM message, const KBDLLHOOKSTRUCT &info);
    void on_mouse(WPARAM message, const MSLLHOOKSTRUCT &info);
    void fire();
    void set_active(const std::optional<TriggerBinding> &binding) {
        std::lock_guard<std::mutex> lock(state_mutex);
        active_binding = binding;
    }
};

namespace {

thread_local GlobalTrigger::Impl *t_listener = nullptr;

LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && t_listener) {
        t_listener->on_key(wparam, *reinterpret_cast<const KBDLLHOOKSTRUCT *>(lparam));
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK mouse_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && t_listener) {
        t_listener->on_mouse(wparam, *reinterpret_cast<const MSLLHOOKSTRUCT *>(lparam));
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

void CALLBACK foreground_proc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (t_listener) lv_double_tap_cancel(&t_listener->detector);
}

LRESULT CALLBACK listener_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    GlobalTrigger::Impl *impl = t_listener;
    if (impl) {
        switch (message) {
        case kApplyMessage: {
            auto *request = reinterpret_cast<ApplyRequest *>(lparam);
            request->ok = impl->apply_on_thread(*request->binding, &request->error);
            return 0;
        }
        case kStopMessage:
            impl->teardown();
            DestroyWindow(hwnd);
            impl->hwnd = nullptr;
            PostQuitMessage(0);
            return 0;
        case WM_HOTKEY:
            if (impl->hotkey_id != 0 && static_cast<int>(wparam) == impl->hotkey_id) impl->fire();
            return 0;
        case WM_TIMER:
            if (wparam == kHealthTimer) impl->health_check();
            return 0;
        default:
            break;
        }
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace

void GlobalTrigger::Impl::run(std::promise<void> *ready) {
    t_listener = this;
    thread_id = GetCurrentThreadId();
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    static std::once_flag registered;
    std::call_once(registered, [instance] {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = listener_window_proc;
        window_class.hInstance = instance;
        window_class.lpszClassName = kWindowClass;
        RegisterClassExW(&window_class);
    });
    hwnd = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (!hwnd) startup_error = with_code(L"Couldn't start the trigger listener", GetLastError());
    lv_double_tap_init(&detector, LV_KEY_OPTION, LV_KEY_SIDE_ANY);
    ready->set_value();
    if (!hwnd) {
        t_listener = nullptr;
        return;
    }
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    t_listener = nullptr;
}

bool GlobalTrigger::Impl::apply_on_thread(const TriggerBinding &binding, std::wstring *error) {
    if (!is_valid(binding)) {
        *error = L"That trigger isn't supported";
        return false;
    }
    std::optional<TriggerBinding> current;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        current = active_binding;
    }
    if (current && *current == binding) return true;

    if (binding.kind == TriggerKind::combination) {
        // Register the new hotkey before releasing the old one so a conflict
        // leaves the previous binding untouched.
        const int id = hotkey_id == 1 ? 2 : 1;
        if (!RegisterHotKey(hwnd, id, binding.modifiers | MOD_NOREPEAT, binding.virtual_key)) {
            const DWORD code = GetLastError();
            const std::wstring label = describe_trigger(binding);
            *error = code == ERROR_HOTKEY_ALREADY_REGISTERED
                ? label + L" is already used by another app"
                : with_code(L"Couldn't register " + label, code);
            return false;
        }
        remove_hooks();
        if (hotkey_id != 0) UnregisterHotKey(hwnd, hotkey_id);
        hotkey_id = id;
    } else {
        if (!keyboard_hook) {
            DWORD code = 0;
            if (!install_hooks(&code)) {
                *error = with_code(L"Couldn't start listening for " + describe_trigger(binding), code);
                return false;
            }
        }
        lv_double_tap_init(&detector, binding.tap_key, binding.tap_side);
        if (hotkey_id != 0) {
            UnregisterHotKey(hwnd, hotkey_id);
            hotkey_id = 0;
        }
    }
    set_active(binding);
    return true;
}

bool GlobalTrigger::Impl::install_hooks(DWORD *code) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    HHOOK keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_proc, instance, 0);
    if (!keyboard) {
        *code = GetLastError();
        return false;
    }
    // The mouse hook is best effort: without it a click between taps simply
    // does not cancel the pending tap.
    HHOOK mouse = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, instance, 0);
    if (keyboard_hook) UnhookWindowsHookEx(keyboard_hook);
    keyboard_hook = keyboard;
    if (mouse) {
        if (mouse_hook) UnhookWindowsHookEx(mouse_hook);
        mouse_hook = mouse;
    }
    if (!foreground_hook) {
        foreground_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
            foreground_proc, 0, 0, WINEVENT_OUTOFCONTEXT);
    }
    normalizer.reset();
    lv_double_tap_cancel(&detector);
    last_hook_tick = GetTickCount();
    SetTimer(hwnd, kHealthTimer, kHealthIntervalMs, nullptr);
    return true;
}

void GlobalTrigger::Impl::remove_hooks() {
    if (hwnd) KillTimer(hwnd, kHealthTimer);
    if (keyboard_hook) UnhookWindowsHookEx(keyboard_hook);
    if (mouse_hook) UnhookWindowsHookEx(mouse_hook);
    if (foreground_hook) UnhookWinEvent(foreground_hook);
    keyboard_hook = nullptr;
    mouse_hook = nullptr;
    foreground_hook = nullptr;
    normalizer.reset();
    lv_double_tap_cancel(&detector);
}

void GlobalTrigger::Impl::teardown() {
    remove_hooks();
    if (hotkey_id != 0) UnregisterHotKey(hwnd, hotkey_id);
    hotkey_id = 0;
    set_active(std::nullopt);
}

void GlobalTrigger::Impl::health_check() {
    if (!keyboard_hook) return;
    LASTINPUTINFO info{};
    info.cbSize = sizeof(info);
    if (!GetLastInputInfo(&info)) return;
    if (static_cast<LONG>(info.dwTime - last_hook_tick) <= kStaleHookMs) return;
    // Windows removes a low-level hook without notice once it exceeds
    // LowLevelHooksTimeout; installing a fresh one restores delivery.
    DWORD code = 0;
    if (install_hooks(&code)) reinstalls.fetch_add(1);
}

void GlobalTrigger::Impl::sync_reset() {
    const std::uint32_t generation = reset_generation.load();
    if (generation == seen_generation) return;
    seen_generation = generation;
    lv_double_tap_cancel(&detector);
}

void GlobalTrigger::Impl::on_key(WPARAM message, const KBDLLHOOKSTRUCT &info) {
    last_hook_tick = GetTickCount();
    sync_reset();
    RawKeyEvent raw;
    raw.vk_code = info.vkCode;
    raw.scan_code = info.scanCode;
    raw.flags = info.flags;
    raw.message = static_cast<std::uint32_t>(message);
    raw.time_ms = info.time;
    raw.extra_info = info.dwExtraInfo;
    LVNormalizedKeyEvent event;
    if (!normalizer.key(raw, &event)) return;
    if (shared->suspended.load()) {
        lv_double_tap_cancel(&detector);
        return;
    }
    if (lv_double_tap_event(&detector, event)) fire();
}

void GlobalTrigger::Impl::on_mouse(WPARAM message, const MSLLHOOKSTRUCT &info) {
    last_hook_tick = GetTickCount();
    switch (message) {
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
        if (info.dwExtraInfo == kLemonVoxInjectedTag) return;
        lv_double_tap_event(&detector, normalizer.mouse_button(info.time));
        return;
    default:
        return;
    }
}

void GlobalTrigger::Impl::fire() {
    if (shared->suspended.load() || !shared->alive.load()) return;
    std::shared_ptr<Shared> state = shared;
    executor.post([state] {
        if (state->alive.load() && !state->suspended.load() && state->on_trigger) state->on_trigger();
    });
}

GlobalTrigger::GlobalTrigger(Executor &executor, std::function<void()> on_trigger)
    : impl_(std::make_unique<Impl>(executor, std::move(on_trigger))) {
    std::promise<void> ready;
    std::future<void> started = ready.get_future();
    Impl *impl = impl_.get();
    impl_->thread = std::thread([impl, &ready] { impl->run(&ready); });
    started.wait();
}

GlobalTrigger::~GlobalTrigger() {
    shutdown();
}

bool GlobalTrigger::apply(const TriggerBinding &binding, std::wstring *error) {
    std::wstring ignored;
    std::wstring &message = error ? *error : ignored;
    std::lock_guard<std::mutex> lock(impl_->control_mutex);
    assert(GetCurrentThreadId() != impl_->thread_id);
    if (!impl_->hwnd) {
        message = impl_->startup_error.empty() ? L"The trigger has shut down" : impl_->startup_error;
        return false;
    }
    ApplyRequest request;
    request.binding = &binding;
    SendMessageW(impl_->hwnd, kApplyMessage, 0, reinterpret_cast<LPARAM>(&request));
    if (!request.ok) message = request.error;
    return request.ok;
}

std::optional<TriggerBinding> GlobalTrigger::active() const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    return impl_->active_binding;
}

void GlobalTrigger::set_suspended(bool suspended) {
    impl_->shared->suspended.store(suspended);
    impl_->reset_generation.fetch_add(1);
}

bool GlobalTrigger::suspended() const {
    return impl_->shared->suspended.load();
}

std::uint32_t GlobalTrigger::hook_reinstalls() const {
    return impl_->reinstalls.load();
}

void GlobalTrigger::shutdown() {
    std::lock_guard<std::mutex> lock(impl_->control_mutex);
    impl_->shared->alive.store(false);
    if (!impl_->thread.joinable()) return;
    assert(GetCurrentThreadId() != impl_->thread_id);
    if (impl_->hwnd) SendMessageW(impl_->hwnd, kStopMessage, 0, 0);
    impl_->thread.join();
    impl_->set_active(std::nullopt);
}

} // namespace lemonvox::win
