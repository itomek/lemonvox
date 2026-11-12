#include "insertion/win_insertion.h"

#include "common/utf.h"
#include "insertion/integrity.h"
#include "insertion/target.h"
#include "trigger/key_normalizer.h"

#include <objbase.h>

#include <array>
#include <iterator>
#include <utility>

namespace lemonvox::win {

namespace {

constexpr wchar_t kWorkerOwnerClass[] = L"LemonVoxInsertionClipboard";
constexpr DWORD kModifierPollMs = 15;

constexpr std::array<int, 8> kModifierKeys = {
    VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN,
};

LRESULT CALLBACK worker_owner_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

bool any_modifier_down() {
    for (int key : kModifierKeys) {
        if (GetAsyncKeyState(key) & 0x8000) return true;
    }
    return false;
}

INPUT key_input(WORD key, bool up) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    input.ki.dwExtraInfo = kLemonVoxInjectedTag;
    return input;
}

// Ctrl+V only. Enter or any submission key is never synthesized.
bool send_paste() {
    std::array<INPUT, 4> inputs = {
        key_input(VK_CONTROL, false), key_input('V', false), key_input('V', true), key_input(VK_CONTROL, true),
    };
    const UINT sent = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    if (sent == inputs.size()) return true;
    // A partially delivered chord must not leave Ctrl logically held.
    std::array<INPUT, 2> release = {key_input('V', true), key_input(VK_CONTROL, true)};
    SendInput(static_cast<UINT>(release.size()), release.data(), sizeof(INPUT));
    return false;
}

void pump_worker_messages() {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

} // namespace

WinInsertion::WinInsertion(Executor &executor, InsertionOptions options)
    : executor_(executor), options_(std::move(options)), alive_(std::make_shared<std::atomic<bool>>(true)),
      table_(std::make_unique<TargetTable>()) {
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (wake_) worker_ = std::thread([this] { run_worker(); });
}

WinInsertion::~WinInsertion() {
    shutdown();
    if (wake_) CloseHandle(wake_);
}

bool WinInsertion::enqueue(Job job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || !worker_.joinable()) return false;
        jobs_.push_back(std::move(job));
    }
    SetEvent(wake_);
    return true;
}

void WinInsertion::run_worker() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = worker_owner_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kWorkerOwnerClass;
    RegisterClassExW(&window_class);
    clipboard_owner_ = CreateWindowExW(0, kWorkerOwnerClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    {
        // Jobs never query accessibility or retain keyboard focus.
        for (;;) {
            MsgWaitForMultipleObjectsEx(1, &wake_, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            pump_worker_messages();
            for (;;) {
                Job job;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (stopping_ || jobs_.empty()) break;
                    job = std::move(jobs_.front());
                    jobs_.pop_front();
                }
                job();
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) break;
        }
    }
    if (clipboard_owner_) DestroyWindow(clipboard_owner_);
    clipboard_owner_ = nullptr;
    if (SUCCEEDED(com)) CoUninitialize();
}

bool WinInsertion::canceled(OperationId operation) {
    std::lock_guard<std::mutex> lock(mutex_);
    // WorkflowCore runs one operation at a time, so an older operation is
    // always finished once a newer one has captured its target.
    return stopping_ || operation < newest_operation_ || canceled_.count(operation) != 0;
}

bool WinInsertion::wait(DWORD milliseconds, OperationId operation, bool cancellable) {
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    for (;;) {
        pump_worker_messages();
        if (cancellable && canceled(operation)) return false;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return true;
        const DWORD remaining = static_cast<DWORD>(deadline - now);
        MsgWaitForMultipleObjectsEx(0, nullptr, remaining < kModifierPollMs ? remaining : kModifierPollMs,
            QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

bool WinInsertion::modifiers_released(OperationId operation, AdapterError &error) {
    const ULONGLONG deadline = GetTickCount64() + options_.modifier_release_timeout_ms;
    while (any_modifier_down()) {
        if (GetTickCount64() >= deadline) {
            error = AdapterError::insertion_unavailable;
            return false;
        }
        if (!wait(kModifierPollMs, operation, true)) {
            error = AdapterError::canceled;
            return false;
        }
    }
    return true;
}

void WinInsertion::post(TargetCompletion completion, OperationId operation, TargetResult result) {
    if (!completion) return;
    auto alive = alive_;
    executor_.post([alive, completion = std::move(completion), operation, result]() {
        if (alive->load()) completion(operation, result);
    });
}

void WinInsertion::post(VoidCompletion completion, OperationId operation, Completion result) {
    if (!completion) return;
    auto alive = alive_;
    executor_.post([alive, completion = std::move(completion), operation, result]() {
        if (alive->load()) completion(operation, result);
    });
}

void WinInsertion::capture_original(OperationId operation, TargetCompletion completion) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shut_down_) return;
        if (operation > newest_operation_) {
            newest_operation_ = operation;
            pending_dispatch_.store(operation);
            canceled_.erase(canceled_.begin(), canceled_.lower_bound(operation));
        }
    }
    table_->retain_only(operation);
    enqueue([this, operation, completion = std::move(completion)]() mutable {
        TargetResult result;
        if (canceled(operation)) {
            result.error = AdapterError::canceled;
        } else {
            // This token authorizes a paste operation, not a discovered field.
            result.target = {{table_->allocate(operation, {})}, true, false, true, true};
        }
        if (canceled(operation)) {
            table_->invalidate(operation);
            result = {};
            result.error = AdapterError::canceled;
        }
        post(std::move(completion), operation, result);
    });
}

void WinInsertion::inspect(OperationId operation, TargetToken original, TargetCompletion completion) {
    enqueue([this, operation, original, completion = std::move(completion)]() mutable {
        TargetResult result;
        TargetIdentity unused;
        if (canceled(operation)) {
            result.error = AdapterError::canceled;
        } else if (original.value && !table_->lookup(original.value, operation, unused)) {
            result.error = AdapterError::target_changed;
        } else {
            const TargetToken token = original.value ? original : TargetToken{table_->allocate(operation, {})};
            // Shared-core dispatch eligibility, not claims about an OS field.
            result.target = {token, true, false, true, original.value != 0};
        }
        if (canceled(operation)) {
            table_->invalidate(operation);
            result = {};
            result.error = AdapterError::canceled;
        }
        post(std::move(completion), operation, result);
    });
}

void WinInsertion::insert(OperationId operation, TargetToken target, std::string text, VoidCompletion completion) {
    enqueue([this, operation, target, text = std::move(text), completion = std::move(completion)]() mutable {
        if (options_.insert_started) options_.insert_started(operation);
        Completion result{run_insert(operation, target, text)};
        // The text was dispatched or abandoned; the operation's tokens are finished.
        table_->invalidate(operation);
        post(std::move(completion), operation, result);
    });
}

AdapterError WinInsertion::run_insert(OperationId operation, TargetToken token,
    const std::string &text) {
    TargetIdentity identity;
    if (!table_->lookup(token.value, operation, identity)) {
        return canceled(operation) ? AdapterError::canceled : AdapterError::target_changed;
    }
    if (canceled(operation)) return AdapterError::canceled;
    bool valid = false;
    const std::wstring wide = utf8_to_wide(text, &valid);
    if (!valid) return AdapterError::invalid_input;

    AdapterError error = AdapterError::none;
    if (!modifiers_released(operation, error)) return error;

    if (wide.empty()) return AdapterError::none;

    ClipboardSnapshot snapshot;
    bool captured = false;
    for (int attempt = 0; attempt < 2 && !captured; ++attempt) {
        if (snapshot_clipboard(clipboard_owner_, snapshot) != ClipboardStatus::ok) {
            return AdapterError::insertion_unavailable;
        }
        // Snapshotting delayed-rendered formats can take time; never
        // overwrite a copy made in the meantime.
        captured = GetClipboardSequenceNumber() == snapshot.sequence;
    }
    if (!captured) return AdapterError::insertion_unavailable;
    if (canceled(operation)) return AdapterError::canceled;

    DWORD ours = 0;
    const ClipboardStatus written = write_private_text(clipboard_owner_, wide, &ours);
    if (written != ClipboardStatus::ok) {
        if (written == ClipboardStatus::failed) restore_clipboard(clipboard_owner_, snapshot, nullptr);
        return AdapterError::insertion_unavailable;
    }
    const auto restore_if_ours = [&]() {
        if (GetClipboardSequenceNumber() == ours) restore_clipboard(clipboard_owner_, snapshot, nullptr);
    };

    if (canceled(operation)) {
        restore_if_ours();
        return AdapterError::canceled;
    }
    if (any_modifier_down() || (options_.before_send && !options_.before_send())) {
        restore_if_ours();
        return AdapterError::insertion_unavailable;
    }
    // A callback or modifier-release wait may have canceled the operation.
    // Check the window that will receive input now, never the capture window.
    if (canceled(operation)) {
        restore_if_ours();
        return AdapterError::canceled;
    }
    DWORD process_id = 0;
    const HWND foreground = GetForegroundWindow();
    if (foreground) GetWindowThreadProcessId(foreground, &process_id);
    if (foreground && input_integrity(process_id) == IntegrityDecision::target_higher) {
        restore_if_ours();
        return AdapterError::insertion_unavailable;
    }
    // Atomically commit dispatch against cancellation, without holding a lock
    // while Windows calls the UI thread's low-level keyboard hook.
    OperationId pending = operation;
    if (!pending_dispatch_.compare_exchange_strong(pending, 0)) {
        restore_if_ours();
        return AdapterError::canceled;
    }
    if (!send_paste()) {
        restore_if_ours();
        return AdapterError::insertion_unavailable;
    }
    // Once dispatched, cancellation must not restore before the target has
    // had time to read the pasted text.
    wait(options_.restore_delay_ms, operation, false);
    restore_if_ours();
    return AdapterError::none;
}

void WinInsertion::cancel(OperationId operation) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shut_down_) return;
        canceled_.insert(operation);
        OperationId pending = operation;
        pending_dispatch_.compare_exchange_strong(pending, 0);
    }
    table_->invalidate(operation);
}

void WinInsertion::release(OperationId operation) {
    table_->invalidate(operation);
}

std::size_t WinInsertion::token_count() const {
    return table_->size();
}

void WinInsertion::shutdown() {
    std::deque<Job> discarded;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shut_down_) return;
        shut_down_ = true;
        stopping_ = true;
        pending_dispatch_.store(0);
        discarded.swap(jobs_);
    }
    alive_->store(false);
    table_->clear();
    discarded.clear();
    if (!worker_.joinable()) return;
    SetEvent(wake_);
    // Clipboard delayed rendering or SendInput may call a window/hook on this
    // thread. Pump sent messages without running posted executor tasks.
    HANDLE thread = worker_.native_handle();
    for (;;) {
        const DWORD wait = MsgWaitForMultipleObjectsEx(1, &thread, INFINITE, QS_SENDMESSAGE, 0);
        if (wait != WAIT_OBJECT_0 + 1) break;
        MSG message;
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
    worker_.join();
}

} // namespace lemonvox::win
