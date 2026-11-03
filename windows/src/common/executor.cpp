#include "executor.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <deque>
#include <mutex>
#include <utility>

namespace lemonvox::win {

namespace {
constexpr UINT kRunTasks = WM_APP + 1;
constexpr wchar_t kClassName[] = L"LemonVoxCoreExecutor";
}

struct UiThreadExecutor::State {
    std::mutex mutex;
    std::deque<std::function<void()>> tasks;
    HWND window = nullptr;
    DWORD owner_thread = 0;
    bool closed = false;
    bool signaled = false;
};

static LRESULT CALLBACK executor_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kRunTasks) {
        auto *executor = reinterpret_cast<UiThreadExecutor *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (executor) executor->drain();
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

UiThreadExecutor::UiThreadExecutor() : state_(new State) {
    state_->owner_thread = GetCurrentThreadId();
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = executor_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kClassName;
    RegisterClassExW(&window_class);
    state_->window = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (state_->window) SetWindowLongPtrW(state_->window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
}

UiThreadExecutor::~UiThreadExecutor() {
    shutdown();
    delete state_;
}

bool UiThreadExecutor::post(std::function<void()> task) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->closed || !state_->window) return false;
    state_->tasks.push_back(std::move(task));
    if (!state_->signaled) {
        state_->signaled = PostMessageW(state_->window, kRunTasks, 0, 0) != FALSE;
        if (!state_->signaled) {
            state_->tasks.pop_back();
            return false;
        }
    }
    return true;
}

void UiThreadExecutor::drain() {
    if (!is_owner_thread()) return;
    for (;;) {
        std::deque<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->signaled = false;
            if (state_->tasks.empty()) return;
            batch.swap(state_->tasks);
        }
        for (auto &task : batch) {
            if (task) task();
        }
    }
}

void UiThreadExecutor::shutdown() {
    HWND window = nullptr;
    std::deque<std::function<void()>> discarded;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->closed) return;
        state_->closed = true;
        discarded.swap(state_->tasks);
        window = state_->window;
        state_->window = nullptr;
    }
    if (window) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        DestroyWindow(window);
    }
}

bool UiThreadExecutor::is_owner_thread() const {
    return GetCurrentThreadId() == state_->owner_thread;
}

} // namespace lemonvox::win
