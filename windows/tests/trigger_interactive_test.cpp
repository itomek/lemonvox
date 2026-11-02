// Drives the real WH_KEYBOARD_LL / RegisterHotKey paths with SendInput.
// Holds the interactive lock for its whole run and only ever injects while its
// own window is the foreground window.

#include "common/executor.h"
#include "common/trigger_binding.h"
#include "support/interactive_lock.h"
#include "trigger/global_trigger.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

using namespace lemonvox::win;

#define REQUIRE(condition)                                                                   \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);       \
            return false;                                                                    \
        }                                                                                    \
    } while (0)

namespace {

HWND g_window = nullptr;
HWND g_edit = nullptr;
DWORD g_ui_thread = 0;
std::atomic<int> g_fired{0};
std::atomic<bool> g_wrong_thread{false};

struct Key {
    WORD vk;
    bool down;
};

INPUT make_input(WORD vk, bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    input.ki.wVk = vk;
    input.ki.wScan = static_cast<WORD>(scan & 0xFF);
    input.ki.dwFlags = ((scan & 0xFF00) == 0xE000 ? KEYEVENTF_EXTENDEDKEY : 0u) | (down ? 0u : KEYEVENTF_KEYUP);
    return input;
}

bool foreground_is_ours() {
    return GetForegroundWindow() == g_window;
}

bool send(std::initializer_list<Key> keys) {
    std::vector<INPUT> inputs;
    for (const Key &key : keys) inputs.push_back(make_input(key.vk, key.down));
    if (!foreground_is_ours()) {
        std::fprintf(stderr, "test window lost the foreground; refusing to inject\n");
        return false;
    }
    return SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size();
}

bool double_alt(WORD vk = VK_LMENU) {
    return send({{vk, true}, {vk, false}, {vk, true}, {vk, false}});
}

bool combo(WORD vk) {
    return send({{VK_LCONTROL, true}, {VK_LMENU, true}, {VK_LSHIFT, true}, {vk, true}, {vk, false},
        {VK_LSHIFT, false}, {VK_LMENU, false}, {VK_LCONTROL, false}});
}

bool wait_for(int expected, DWORD timeout_ms = 2000) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (g_fired.load() != expected && GetTickCount64() < deadline) Sleep(10);
    return g_fired.load() == expected;
}

// Lets the input queue drain and stays outside the double-tap window.
bool settle_and_expect(int expected) {
    Sleep(700);
    return g_fired.load() == expected;
}

bool in_menu_mode() {
    Sleep(150);
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    return GetGUIThreadInfo(g_ui_thread, &info) && (info.flags & GUI_INMENUMODE) != 0;
}

std::wstring edit_text() {
    wchar_t buffer[64] = {};
    GetWindowTextW(g_edit, buffer, 64);
    return buffer;
}

bool typed_text_reaches_edit() {
    SetWindowTextW(g_edit, L"");
    REQUIRE(send({{'A', true}, {'A', false}, {'B', true}, {'B', false}, {'C', true}, {'C', false}}));
    const ULONGLONG deadline = GetTickCount64() + 2000;
    while (edit_text() != L"abc" && GetTickCount64() < deadline) Sleep(10);
    if (edit_text() != L"abc") {
        std::fprintf(stderr, "EDIT contains \"%ls\" instead of \"abc\"\n", edit_text().c_str());
        return false;
    }
    return true;
}

bool scenario(GlobalTrigger &trigger, UINT blocked_vk) {
    std::wstring error;
    REQUIRE(trigger.apply(default_trigger(), &error));
    REQUIRE(trigger.active() && *trigger.active() == default_trigger());
    Sleep(200);

    // Double Alt fires exactly once, and leaves the window out of menu mode.
    REQUIRE(double_alt());
    REQUIRE(wait_for(1));
    REQUIRE(settle_and_expect(1));
    REQUIRE(!in_menu_mode());
    std::printf("double Alt: fired once, menu mode afterwards: no\n");
    REQUIRE(double_alt(VK_RMENU));
    REQUIRE(wait_for(2));
    REQUIRE(settle_and_expect(2));
    REQUIRE(!in_menu_mode());

    // The hook never swallows ordinary typing.
    REQUIRE(typed_text_reaches_edit());
    REQUIRE(settle_and_expect(2));

    // Alt+letter chords don't trigger.
    REQUIRE(send({{VK_LMENU, true}, {'Q', true}, {'Q', false}, {VK_LMENU, false},
        {VK_LMENU, true}, {'Q', true}, {'Q', false}, {VK_LMENU, false}}));
    REQUIRE(settle_and_expect(2));
    REQUIRE(!in_menu_mode());

    // A lone Alt tap is Windows' own menu-bar toggle; LemonVox leaves it alone.
    REQUIRE(send({{VK_LMENU, true}, {VK_LMENU, false}}));
    const bool single_tap_menu = in_menu_mode();
    std::printf("single Alt tap: menu mode %s\n", single_tap_menu ? "entered" : "not entered");
    REQUIRE(settle_and_expect(2));
    REQUIRE(send({{VK_LMENU, true}, {VK_LMENU, false}}));
    REQUIRE(settle_and_expect(2));
    REQUIRE(!in_menu_mode());
    REQUIRE(typed_text_reaches_edit());

    // Suspended (shortcut recorder active): nothing fires.
    trigger.set_suspended(true);
    REQUIRE(double_alt());
    REQUIRE(settle_and_expect(2));
    trigger.set_suspended(false);
    REQUIRE(double_alt());
    REQUIRE(wait_for(3));
    REQUIRE(settle_and_expect(3));

    // RegisterHotKey combination.
    TriggerBinding f11;
    f11.kind = TriggerKind::combination;
    f11.modifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    f11.virtual_key = VK_F11;
    REQUIRE(trigger.apply(f11, &error));
    REQUIRE(combo(VK_F11));
    REQUIRE(wait_for(4));
    REQUIRE(settle_and_expect(4));
    REQUIRE(double_alt());
    REQUIRE(settle_and_expect(4));
    REQUIRE(!in_menu_mode());

    // A combination owned elsewhere fails readably and keeps the previous one.
    TriggerBinding taken = f11;
    taken.virtual_key = blocked_vk;
    error.clear();
    REQUIRE(!trigger.apply(taken, &error));
    std::printf("conflict error: %ls\n", error.c_str());
    REQUIRE(error.find(L"already used by another app") != std::wstring::npos);
    REQUIRE(trigger.active() && *trigger.active() == f11);
    TriggerBinding invalid;
    invalid.kind = TriggerKind::combination;
    invalid.modifiers = MOD_SHIFT;
    invalid.virtual_key = 'A';
    REQUIRE(!trigger.apply(invalid, &error));
    REQUIRE(trigger.active() && *trigger.active() == f11);
    REQUIRE(combo(VK_F11));
    REQUIRE(wait_for(5));
    REQUIRE(settle_and_expect(5));

    // Back to double tap: the hotkey is released and the hook works again.
    REQUIRE(trigger.apply(default_trigger(), &error));
    REQUIRE(combo(VK_F11));
    REQUIRE(settle_and_expect(5));
    REQUIRE(double_alt());
    REQUIRE(wait_for(6));
    REQUIRE(settle_and_expect(6));

    // After shutdown nothing fires.
    trigger.shutdown();
    REQUIRE(!trigger.active());
    REQUIRE(double_alt());
    REQUIRE(settle_and_expect(6));
    REQUIRE(!in_menu_mode());
    REQUIRE(!trigger.apply(default_trigger(), &error));
    REQUIRE(!g_wrong_thread.load());
    std::printf("hook reinstalls during run: %u\n", trigger.hook_reinstalls());
    return true;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_SETFOCUS:
        if (g_edit) SetFocus(g_edit);
        return 0;
    case WM_SIZE:
        if (g_edit) MoveWindow(g_edit, 8, 8, LOWORD(lparam) - 16, 28, TRUE);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

void pump_for(DWORD ms) {
    const ULONGLONG deadline = GetTickCount64() + ms;
    MSG message;
    do {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    } while (GetTickCount64() < deadline);
}

bool bring_to_foreground() {
    ShowWindow(g_window, SW_SHOWNORMAL);
    UpdateWindow(g_window);
    for (int attempt = 0; attempt < 30 && !foreground_is_ours(); ++attempt) {
        const HWND current = GetForegroundWindow();
        const DWORD current_thread = current ? GetWindowThreadProcessId(current, nullptr) : 0;
        const DWORD self = GetCurrentThreadId();
        const bool attached = current_thread != 0 && current_thread != self
            && AttachThreadInput(self, current_thread, TRUE);
        if (attempt % 2 == 1) {
            ShowWindow(g_window, SW_MINIMIZE);
            ShowWindow(g_window, SW_RESTORE);
        }
        BringWindowToTop(g_window);
        SetForegroundWindow(g_window);
        if (attached) AttachThreadInput(self, current_thread, FALSE);
        pump_for(100);
    }
    SetFocus(g_edit);
    pump_for(100);
    return foreground_is_ours();
}

bool modifiers_released() {
    for (int attempt = 0; attempt < 100; ++attempt) {
        bool held = false;
        for (int vk : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN}) held = held || (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (!held) return true;
        Sleep(50);
    }
    return false;
}

} // namespace

int main() {
    test::InteractiveLock lock;
    g_ui_thread = GetCurrentThreadId();
    if (!modifiers_released()) {
        std::fprintf(stderr, "a modifier key is held down; cannot run\n");
        return 1;
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = L"LemonVoxTriggerInteractiveTest";
    RegisterClassExW(&window_class);
    // A classic menu bar makes Alt's menu-mode side effect observable.
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, 1, L"&Nothing");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    g_window = CreateWindowExW(WS_EX_TOPMOST, window_class.lpszClassName, L"LemonVox trigger test",
        WS_OVERLAPPEDWINDOW, 200, 200, 420, 140, nullptr, menu, instance, nullptr);
    g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        8, 8, 380, 28, g_window, nullptr, instance, nullptr);
    if (!g_window || !g_edit) {
        std::fprintf(stderr, "could not create the test window\n");
        return 1;
    }
    if (!bring_to_foreground()) {
        std::fprintf(stderr, "test window could not become foreground; refusing to inject input\n");
        DestroyWindow(g_window);
        return 1;
    }

    // A hidden window in this process owns a combination so the conflict path is real.
    HWND blocker = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    UINT blocked_vk = 0;
    for (UINT vk : {VK_F12, VK_F10, VK_F9, VK_F8}) {
        if (RegisterHotKey(blocker, 1, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, vk)) {
            blocked_vk = vk;
            break;
        }
    }
    if (!blocked_vk) {
        std::fprintf(stderr, "could not reserve a conflicting hotkey\n");
        return 1;
    }

    bool ok = false;
    {
        UiThreadExecutor executor;
        GlobalTrigger trigger(executor, [] {
            if (GetCurrentThreadId() != g_ui_thread) g_wrong_thread = true;
            g_fired.fetch_add(1);
        });
        std::atomic<bool> done{false};
        std::thread driver([&] {
            ok = scenario(trigger, blocked_vk);
            done = true;
        });
        MSG message;
        while (!done.load()) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
        driver.join();
        trigger.shutdown();
        executor.drain();
        if (g_fired.load() != 6) ok = false;
        executor.shutdown();
    }

    UnregisterHotKey(blocker, 1);
    DestroyWindow(blocker);
    DestroyWindow(g_window);
    std::printf("trigger_interactive_test: %s (fired %d)\n", ok ? "ok" : "FAILED", g_fired.load());
    return ok ? 0 : 1;
}
