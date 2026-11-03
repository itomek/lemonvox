// Interactive insertion test. It opens its own top-level window on the real
// desktop and pastes into it, so it holds the interactive lock for its whole
// duration and refuses to synthesize input unless its own window is the
// foreground window. The person's clipboard is restored at the end.

#include "common/executor.h"
#include "common/utf.h"
#include "insertion/clipboard.h"
#include "insertion/win_insertion.h"
#include "support/interactive_lock.h"

#include <windows.h>
#include <richedit.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace lemonvox;
using namespace lemonvox::win;

namespace {

constexpr char kText[] = "LemonVox fixed text \xE2\x9C\x93 123";
constexpr wchar_t kSentinel[] = L"LemonVox interactive sentinel \u2713";

ClipboardSnapshot *g_saved_clipboard = nullptr;
HWND g_main = nullptr;
HWND g_other = nullptr;
std::atomic<int> g_sends{0};
std::atomic<bool> g_foreign_foreground{false};
HANDLE g_gate = nullptr;
std::atomic<bool> g_gate_armed{false};

void restore_saved_clipboard() {
    if (!g_saved_clipboard) return;
    ClipboardSnapshot *saved = g_saved_clipboard;
    g_saved_clipboard = nullptr;
    restore_clipboard(nullptr, *saved, nullptr);
}

[[noreturn]] void fail(const char *expression, int line) {
    std::fprintf(stderr, "insertion_interactive_test:%d: check failed: %s\n", line, expression);
    restore_saved_clipboard();
    std::fflush(stderr);
    std::exit(1);
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS *) {
    restore_saved_clipboard();
    return EXCEPTION_CONTINUE_SEARCH;
}

#define CHECK(expression) ((expression) ? (void)0 : fail(#expression, __LINE__))

void pump_for(DWORD milliseconds) {
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    MSG message;
    do {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
    } while (GetTickCount64() < deadline);
}

template <typename Predicate>
void pump_until(Predicate done, DWORD timeout = 15000) {
    const ULONGLONG deadline = GetTickCount64() + timeout;
    MSG message;
    while (!done()) {
        CHECK(GetTickCount64() < deadline);
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!done()) MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

bool try_foreground(HWND window) {
    ShowWindow(window, SW_SHOWNORMAL);
    if (GetForegroundWindow() == window) return true;
    SetForegroundWindow(window);
    pump_for(50);
    if (GetForegroundWindow() == window) return true;
    // The test may have been started by a background process. Joining the
    // current foreground thread's input queue lets it take activation without
    // synthesizing any input into that window.
    const DWORD foreground_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD own_thread = GetCurrentThreadId();
    const bool attached = foreground_thread != 0 && foreground_thread != own_thread &&
        AttachThreadInput(own_thread, foreground_thread, TRUE);
    BringWindowToTop(window);
    SetForegroundWindow(window);
    SetActiveWindow(window);
    if (attached) AttachThreadInput(own_thread, foreground_thread, FALSE);
    pump_for(100);
    if (GetForegroundWindow() == window) return true;
    ShowWindow(window, SW_MINIMIZE);
    pump_for(100);
    ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
    pump_for(200);
    return GetForegroundWindow() == window;
}

void bring_to_front(HWND window) {
    bool foreground = false;
    for (int attempt = 0; attempt < 5 && !foreground; ++attempt) foreground = try_foreground(window);
    if (!foreground) {
        std::fprintf(stderr, "insertion_interactive_test: test window could not become foreground; "
                             "refusing to inject input into another window\n");
    }
    CHECK(foreground);
}

void focus(HWND control) {
    CHECK(GetForegroundWindow() == g_main);
    SetFocus(control);
    pump_for(50);
    CHECK(GetFocus() == control);
}

std::wstring text_of(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(copied));
    return text;
}

void set_sentinel_clipboard(UINT custom) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (OpenClipboard(g_main)) break;
        CHECK(attempt < 49);
        Sleep(10);
    }
    CHECK(EmptyClipboard());
    const std::wstring sentinel = kSentinel;
    const std::size_t text_size = (sentinel.size() + 1) * sizeof(wchar_t);
    HGLOBAL text = GlobalAlloc(GMEM_MOVEABLE, text_size);
    std::memcpy(GlobalLock(text), sentinel.c_str(), text_size);
    GlobalUnlock(text);
    CHECK(SetClipboardData(CF_UNICODETEXT, text) != nullptr);
    const unsigned char marker[4] = {0x4C, 0x56, 0x01, 0x02};
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, sizeof(marker));
    std::memcpy(GlobalLock(data), marker, sizeof(marker));
    GlobalUnlock(data);
    CHECK(SetClipboardData(custom, data) != nullptr);
    CloseClipboard();
}

void check_sentinel_clipboard(UINT custom) {
    std::wstring current;
    CHECK(read_clipboard_text(current));
    CHECK(current == kSentinel);
    CHECK(IsClipboardFormatAvailable(custom));
}

struct Harness {
    UiThreadExecutor &executor;
    WinInsertion &insertion;

    TargetResult capture(OperationId operation) {
        CHECK(GetForegroundWindow() == g_main || GetForegroundWindow() == g_other);
        int calls = 0;
        TargetResult out;
        insertion.capture_original(operation, [&](OperationId completed, TargetResult result) {
            CHECK(completed == operation);
            ++calls;
            out = result;
        });
        pump_until([&] { return calls != 0; });
        pump_for(20);
        CHECK(calls == 1);
        return out;
    }

    TargetResult inspect(OperationId operation, TargetToken original) {
        int calls = 0;
        TargetResult out;
        insertion.inspect(operation, original, [&](OperationId completed, TargetResult result) {
            CHECK(completed == operation);
            ++calls;
            out = result;
        });
        pump_until([&] { return calls != 0; });
        pump_for(20);
        CHECK(calls == 1);
        return out;
    }

    AdapterError insert(OperationId operation, TargetToken target, bool cancel_while_queued = false) {
        int calls = 0;
        AdapterError out = AdapterError::none;
        if (cancel_while_queued) g_gate_armed = true;
        insertion.insert(operation, target, kText, [&](OperationId completed, Completion result) {
            CHECK(completed == operation);
            ++calls;
            out = result.error;
        });
        if (cancel_while_queued) {
            insertion.cancel(operation);
            SetEvent(g_gate);
        }
        pump_until([&] { return calls != 0; });
        pump_for(50);
        CHECK(calls == 1);
        return out;
    }
};

HWND make_child(HWND parent, const wchar_t *window_class, DWORD style, int id, int top) {
    HWND child = CreateWindowExW(WS_EX_CLIENTEDGE, window_class, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
        10, top, 600, 60, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    CHECK(child != nullptr);
    return child;
}

void run() {
    const UINT custom = RegisterClipboardFormatW(L"LemonVoxInteractiveSentinel");
    CHECK(LoadLibraryW(L"Msftedit.dll") != nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = L"LemonVoxInsertionInteractiveTest";
    CHECK(RegisterClassExW(&window_class) != 0);
    g_main = CreateWindowExW(WS_EX_TOPMOST, window_class.lpszClassName, L"LemonVox insertion test",
        WS_OVERLAPPEDWINDOW, 100, 100, 640, 280, nullptr, nullptr, window_class.hInstance, nullptr);
    g_other = CreateWindowExW(WS_EX_TOPMOST, window_class.lpszClassName, L"LemonVox insertion test (other)",
        WS_OVERLAPPEDWINDOW, 160, 160, 400, 160, nullptr, nullptr, window_class.hInstance, nullptr);
    CHECK(g_main && g_other);
    // Multiline edits with ES_WANTRETURN make a stray Enter visible as a newline.
    HWND edit = make_child(g_main, L"EDIT", ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL, 1, 10);
    HWND password = make_child(g_main, L"EDIT", ES_PASSWORD | ES_AUTOHSCROLL, 2, 80);
    HWND rich = make_child(g_main, MSFTEDIT_CLASS, ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL, 3, 150);
    HWND other_edit = make_child(g_other, L"EDIT", ES_AUTOHSCROLL, 4, 10);

    set_sentinel_clipboard(custom);
    bring_to_front(g_main);

    UiThreadExecutor executor;
    InsertionOptions options;
    options.insert_started = [](OperationId) {
        if (g_gate_armed.exchange(false)) WaitForSingleObject(g_gate, 10000);
    };
    options.before_send = [] {
        ++g_sends;
        if (GetForegroundWindow() != g_main) {
            g_foreign_foreground = true;
            return false;
        }
        return true;
    };
    WinInsertion insertion(executor, options);
    Harness harness{executor, insertion};
    const std::wstring expected = utf8_to_wide(kText);

    // Native EDIT: inserted exactly once, no newline, clipboard restored.
    focus(edit);
    TargetResult original = harness.capture(1);
    CHECK(original.error == AdapterError::none);
    CHECK(original.target.token.value != 0);
    CHECK(original.target.editable && !original.target.secure && original.target.frontmost);
    TargetResult current = harness.inspect(1, original.target.token);
    CHECK(current.error == AdapterError::none);
    CHECK(current.target.matches_original && current.target.frontmost && current.target.editable);
    CHECK(!current.target.secure && current.target.token.value == original.target.token.value);
    CHECK(GetForegroundWindow() == g_main);
    CHECK(harness.insert(1, current.target.token) == AdapterError::none);
    CHECK(g_sends == 1 && !g_foreign_foreground);
    pump_for(150);
    CHECK(text_of(edit) == expected);
    check_sentinel_clipboard(custom);
    insertion.release(1);
    CHECK(insertion.token_count() == 0);
    std::puts("native edit: inserted once, clipboard restored");

    // Password EDIT: reported secure and never tokenized or typed into.
    focus(password);
    original = harness.capture(2);
    CHECK(original.error == AdapterError::none);
    CHECK(original.target.secure && original.target.token.value == 0);
    current = harness.inspect(2, TargetToken{});
    CHECK(current.error == AdapterError::none && current.target.secure && current.target.token.value == 0);
    CHECK(text_of(password).empty());
    std::puts("password edit: secure, rejected");

    // Focus moved to another control between capture and inspect.
    focus(edit);
    original = harness.capture(3);
    CHECK(original.error == AdapterError::none && original.target.token.value != 0);
    focus(rich);
    current = harness.inspect(3, original.target.token);
    CHECK(current.error == AdapterError::none && !current.target.matches_original);
    CHECK(current.target.token.value != original.target.token.value);
    CHECK(GetForegroundWindow() == g_main);
    CHECK(harness.insert(3, original.target.token) == AdapterError::target_changed);
    CHECK(g_sends == 1);
    CHECK(text_of(edit) == expected && text_of(rich).empty());
    std::puts("changed control: not matching, nothing inserted");

    // Focus moved to another top-level window.
    bring_to_front(g_main);
    focus(edit);
    original = harness.capture(4);
    CHECK(original.error == AdapterError::none && original.target.token.value != 0);
    bring_to_front(g_other);
    SetFocus(other_edit);
    pump_for(50);
    current = harness.inspect(4, original.target.token);
    CHECK(current.error == AdapterError::none && !current.target.matches_original);
    CHECK(harness.insert(4, original.target.token) == AdapterError::target_changed);
    CHECK(g_sends == 1);
    CHECK(text_of(other_edit).empty() && text_of(edit) == expected);
    bring_to_front(g_main);
    std::puts("changed window: not matching, nothing inserted");

    // RichEdit receives the same text without a newline.
    focus(rich);
    original = harness.capture(5);
    CHECK(original.error == AdapterError::none && original.target.token.value != 0 && original.target.editable);
    current = harness.inspect(5, original.target.token);
    CHECK(current.target.matches_original);
    CHECK(GetForegroundWindow() == g_main);
    CHECK(harness.insert(5, current.target.token) == AdapterError::none);
    CHECK(g_sends == 2 && !g_foreign_foreground);
    pump_for(150);
    CHECK(text_of(rich) == expected);
    check_sentinel_clipboard(custom);
    std::puts("rich edit: inserted once, clipboard restored");

    // Cancellation before insert inserts nothing.
    SetWindowTextW(edit, L"");
    focus(edit);
    original = harness.capture(6);
    CHECK(original.error == AdapterError::none && original.target.token.value != 0);
    CHECK(harness.insert(6, original.target.token, true) == AdapterError::canceled);
    original = harness.capture(7);
    CHECK(original.error == AdapterError::none && original.target.token.value != 0);
    insertion.cancel(7);
    CHECK(harness.insert(7, original.target.token) == AdapterError::canceled);
    pump_for(150);
    CHECK(g_sends == 2);
    CHECK(text_of(edit).empty());
    check_sentinel_clipboard(custom);
    std::puts("cancel: nothing inserted, clipboard untouched");

    insertion.shutdown();
    executor.shutdown();
    DestroyWindow(g_other);
    DestroyWindow(g_main);
}

} // namespace

int main() {
    SetUnhandledExceptionFilter(crash_filter);
    lemonvox::win::test::InteractiveLock lock;
    g_gate = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(g_gate != nullptr);
    auto saved = std::make_unique<ClipboardSnapshot>();
    CHECK(snapshot_clipboard(nullptr, *saved) == ClipboardStatus::ok);
    g_saved_clipboard = saved.get();
    run();
    restore_saved_clipboard();
    CloseHandle(g_gate);
    std::puts("insertion_interactive_test passed");
    return 0;
}
