// Interactive proof that the indicator never takes foreground or focus:
// a real foreground window with a focused EDIT stays foreground/focused while
// the indicator cycles through every state, is clicked and is dragged.
// Holds the shared InteractiveLock and refuses to inject input unless its own
// window is foreground and the target point is on the indicator.

#include "support/interactive_lock.h"
#include "ui/indicator.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>

using namespace lemonvox::win::ui;
using namespace std::chrono_literals;

namespace {

HWND g_window = nullptr;
HWND g_edit = nullptr;

[[noreturn]] void fail(const char *what) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
}

void expect(bool condition, const char *what) {
    if (!condition) fail(what);
}

void pump_for(std::chrono::milliseconds duration, const std::function<bool()> &done = {}) {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(duration.count());
    MSG message;
    do {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (done && done()) return;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    } while (GetTickCount64() < deadline);
}

LRESULT CALLBACK test_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_SIZE && g_edit) MoveWindow(g_edit, 10, 10, LOWORD(lparam) - 20, HIWORD(lparam) - 20, TRUE);
    return DefWindowProcW(window, message, wparam, lparam);
}

bool make_foreground(HWND window) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (GetForegroundWindow() == window) return true;
        HWND current = GetForegroundWindow();
        const DWORD self = GetCurrentThreadId();
        const DWORD other = current ? GetWindowThreadProcessId(current, nullptr) : 0;
        const bool attached = other != 0 && other != self && AttachThreadInput(self, other, TRUE);
        ShowWindow(window, SW_SHOWNORMAL);
        BringWindowToTop(window);
        SetForegroundWindow(window);
        if (attached) AttachThreadInput(self, other, FALSE);
        pump_for(100ms);
    }
    return GetForegroundWindow() == window;
}

void expect_target_unchanged(const IndicatorWindow &indicator, const char *where) {
    if (GetForegroundWindow() != g_window) {
        std::fprintf(stderr, "at %s\n", where);
        fail("test window lost the foreground");
    }
    if (GetFocus() != g_edit) {
        std::fprintf(stderr, "at %s\n", where);
        fail("EDIT control lost keyboard focus");
    }
    if (GetActiveWindow() != g_window) {
        std::fprintf(stderr, "at %s\n", where);
        fail("test window is no longer the active window");
    }
    if (indicator.activation_count() != 0) {
        std::fprintf(stderr, "at %s\n", where);
        fail("indicator received WM_ACTIVATE/WM_SETFOCUS");
    }
}

bool point_in(const RECT &rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

void send_mouse(DWORD flags, POINT point) {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>(((point.x - left) * 65535LL + width / 2) / (width - 1));
    input.mi.dy = static_cast<LONG>(((point.y - top) * 65535LL + height / 2) / (height - 1));
    input.mi.dwFlags = flags | MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    expect(SendInput(1, &input, sizeof(input)) == 1, "SendInput was blocked");
}

// Verifies the injection target before any input is sent.
void guard_injection(const IndicatorWindow &indicator, POINT point, const char *where) {
    expect_target_unchanged(indicator, where);
    HWND hit = WindowFromPoint(point);
    if (hit != indicator.hwnd()) {
        std::fprintf(stderr, "at %s: point %ld,%ld is not on the indicator\n", where, point.x, point.y);
        fail("refusing to inject outside the indicator");
    }
}

void click(const IndicatorWindow &indicator, IndicatorButton button, const char *where) {
    const auto rect = indicator.button_rect(button);
    expect(rect.has_value(), "button is not visible");
    const POINT center{(rect->left + rect->right) / 2, (rect->top + rect->bottom) / 2};
    guard_injection(indicator, center, where);
    send_mouse(0, center);
    pump_for(60ms);
    guard_injection(indicator, center, where);
    send_mouse(MOUSEEVENTF_LEFTDOWN, center);
    pump_for(40ms);
    send_mouse(MOUSEEVENTF_LEFTUP, center);
}

IndicatorState make(IndicatorMode mode, std::wstring message = {}, std::wstring action = {}) {
    IndicatorState state;
    state.mode = mode;
    state.message = std::move(message);
    state.action_label = std::move(action);
    return state;
}

std::wstring window_text(HWND window) {
    wchar_t buffer[512] = {};
    GetWindowTextW(window, buffer, 512);
    return buffer;
}

} // namespace

int main() {
    lemonvox::win::test::InteractiveLock lock;
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = test_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"LemonVoxUiInteractiveTest";
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    expect(RegisterClassExW(&window_class) != 0, "RegisterClassExW failed");

    HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    const RECT work = info.rcWork;
    const int scale = static_cast<int>(GetDpiForSystem());
    const int width = MulDiv(480, scale, 96), height = MulDiv(160, scale, 96);
    const int left = work.left + (work.right - work.left - width) / 2;
    const int top = work.top + (work.bottom - work.top) / 4;
    g_window = CreateWindowExW(0, window_class.lpszClassName, L"LemonVox UI interactive test", WS_OVERLAPPEDWINDOW,
                               left, top, width, height, nullptr, nullptr, instance, nullptr);
    expect(g_window != nullptr, "CreateWindowExW failed");
    g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL, 0,
                             0, 0, 0, g_window, nullptr, instance, nullptr);
    expect(g_edit != nullptr, "EDIT creation failed");
    ShowWindow(g_window, SW_SHOWNORMAL);
    UpdateWindow(g_window);
    RECT client{};
    GetClientRect(g_window, &client);
    test_proc(g_window, WM_SIZE, 0, MAKELPARAM(client.right, client.bottom));
    if (!make_foreground(g_window)) fail("could not make the test window foreground; not injecting input");
    SetFocus(g_edit);
    pump_for(100ms);
    expect(GetFocus() == g_edit, "could not focus the EDIT control");

    int cancels = 0, stops = 0, copies = 0, dismissals = 0, actions = 0, moves = 0;
    POINT reported{};
    IndicatorWindow indicator(IndicatorCallbacks{
        [&] { ++cancels; },
        [&] { ++stops; },
        [&] { ++copies; },
        [&] { ++dismissals; },
        [&] { ++actions; },
        [&](POINT anchor) {
            ++moves;
            reported = anchor;
        },
    });
    expect(indicator.create(instance), "indicator creation failed");
    RECT test_rect{};
    GetWindowRect(g_window, &test_rect);
    // Place the bar below the test window so neither covers the other.
    indicator.set_anchor(POINT{(test_rect.left + test_rect.right) / 2, test_rect.bottom + MulDiv(120, scale, 96)});
    indicator.set_auto_hide_delay(250ms);
    expect_target_unchanged(indicator, "setup");

    // Every state: visible, topmost, accurate accessible name, no activation.
    const IndicatorState states[] = {
        make(IndicatorMode::preparing),
        make(IndicatorMode::recording),
        make(IndicatorMode::transcribing),
        make(IndicatorMode::cleaning),
        make(IndicatorMode::inserting),
        make(IndicatorMode::error, L"Microphone access is turned off.", L"Open microphone settings"),
        make(IndicatorMode::copy_fallback, L"Couldn\u2019t insert into this app. The text is ready to copy."),
        make(IndicatorMode::success),
    };
    for (const IndicatorState &state : states) {
        indicator.set_state(state);
        if (state.mode == IndicatorMode::recording) {
            for (int frame = 0; frame < 10; ++frame) {
                indicator.set_level(frame / 10.0, std::chrono::milliseconds(frame * 100));
                pump_for(30ms);
            }
        }
        pump_for(150ms);
        expect(indicator.visible(), "indicator is not visible");
        expect((GetWindowLongPtrW(indicator.hwnd(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0, "indicator is not topmost");
        expect(window_text(indicator.hwnd()) == indicator_accessible_name(state), "accessible name mismatch");
        expect_target_unchanged(indicator, "state cycle");
    }
    pump_for(600ms, [&] { return !indicator.visible(); });
    expect(!indicator.visible(), "success state did not auto-hide");

    POINT saved_cursor{};
    GetCursorPos(&saved_cursor);

    // Clicking Copy in the fallback panel runs the callback without activation.
    indicator.set_state(make(IndicatorMode::copy_fallback, L"Couldn\u2019t insert into this app."));
    pump_for(150ms);
    click(indicator, IndicatorButton::copy, "copy click");
    pump_for(2000ms, [&] { return copies == 1; });
    expect(copies == 1, "Copy click did not reach the callback");
    pump_for(100ms);
    expect_target_unchanged(indicator, "after copy click");

    click(indicator, IndicatorButton::dismiss, "dismiss click");
    pump_for(2000ms, [&] { return dismissals == 1; });
    expect(dismissals == 1 && !indicator.visible(), "Dismiss did not hide the indicator");
    expect_target_unchanged(indicator, "after dismiss click");

    // Stop button while recording.
    indicator.set_state(make(IndicatorMode::recording));
    pump_for(150ms);
    click(indicator, IndicatorButton::stop, "stop click");
    pump_for(2000ms, [&] { return stops == 1; });
    expect(stops == 1 && cancels == 0, "Stop click did not reach the callback");
    expect_target_unchanged(indicator, "after stop click");

    // Drag by the bar body (between the buttons) moves it and reports the anchor.
    RECT before{};
    GetWindowRect(indicator.hwnd(), &before);
    const auto cancel_rect = indicator.button_rect(IndicatorButton::cancel);
    const auto stop_rect = indicator.button_rect(IndicatorButton::stop);
    expect(cancel_rect && stop_rect, "recording buttons missing");
    const POINT grab{(cancel_rect->right + stop_rect->left) / 2, (before.top + before.bottom) / 2};
    expect(!point_in(*cancel_rect, grab) && !point_in(*stop_rect, grab), "grab point overlaps a button");
    const LONG dx = MulDiv(60, scale, 96), dy = -MulDiv(40, scale, 96);
    guard_injection(indicator, grab, "drag start");
    send_mouse(0, grab);
    pump_for(50ms);
    send_mouse(MOUSEEVENTF_LEFTDOWN, grab);
    pump_for(50ms);
    for (int step = 1; step <= 6; ++step) {
        send_mouse(0, POINT{grab.x + dx * step / 6, grab.y + dy * step / 6});
        pump_for(30ms);
    }
    send_mouse(MOUSEEVENTF_LEFTUP, POINT{grab.x + dx, grab.y + dy});
    pump_for(2000ms, [&] { return moves == 1; });
    RECT after{};
    GetWindowRect(indicator.hwnd(), &after);
    SetCursorPos(saved_cursor.x, saved_cursor.y);
    expect(moves == 1, "drag did not report a position change");
    expect(after.left - before.left == dx && after.top - before.top == dy, "drag did not move the indicator");
    expect(reported.x == (after.left + after.right) / 2 && reported.y == after.bottom, "reported anchor mismatch");
    expect(indicator.anchor().x == reported.x && indicator.anchor().y == reported.y, "anchor not retained");
    expect(stops == 1 && cancels == 0, "drag triggered a button");
    expect_target_unchanged(indicator, "after drag");

    // Restoring a persisted anchor that is now off-screen clamps into a work area.
    indicator.set_anchor(POINT{GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 5000,
                               GetSystemMetrics(SM_YVIRTUALSCREEN) - 5000});
    pump_for(100ms);
    RECT clamped{};
    GetWindowRect(indicator.hwnd(), &clamped);
    HMONITOR nearest = MonitorFromRect(&clamped, MONITOR_DEFAULTTONULL);
    expect(nearest != nullptr, "restored indicator is not on any monitor");
    MONITORINFO nearest_info{sizeof(nearest_info)};
    GetMonitorInfoW(nearest, &nearest_info);
    RECT inside{};
    expect(IntersectRect(&inside, &clamped, &nearest_info.rcWork) && EqualRect(&inside, &clamped),
           "restored indicator is not inside a work area");
    expect_target_unchanged(indicator, "after restore");

    indicator.set_state(IndicatorState{});
    indicator.destroy();
    DestroyWindow(g_window);
    std::puts("ui_interactive_test passed");
    return 0;
}
