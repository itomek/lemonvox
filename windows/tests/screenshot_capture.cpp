// Captures production native windows with synthetic services. No microphone,
// network, registry preference writes, or global shortcuts are used.
#include "ui/settings_window.h"
#include "ui/indicator.h"
#include "support/interactive_lock.h"

#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cstdio>
#include <filesystem>
#include <string>

using namespace lemonvox::win;

namespace {
// Unlike STATIC text controls, this opaque window participates in
// WindowFromPoint hit testing beneath the layered window's clear corners.
class CaptureBackdrop {
public:
    CaptureBackdrop() {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = DefWindowProcW;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH));
        window_class.lpszClassName = L"LemonVoxCaptureBackdrop";
        atom_ = RegisterClassW(&window_class);
        if (atom_) {
            hwnd = CreateWindowExW(0, window_class.lpszClassName, L"LemonVox synthetic screenshot fixture",
                WS_POPUP | WS_VISIBLE, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                nullptr, nullptr, window_class.hInstance, nullptr);
        }
    }
    ~CaptureBackdrop() {
        if (hwnd) DestroyWindow(hwnd);
        if (atom_) UnregisterClassW(L"LemonVoxCaptureBackdrop", GetModuleHandleW(nullptr));
    }
    HWND hwnd = nullptr;
private:
    ATOM atom_ = 0;
};

void pump(DWORD duration) {
    const ULONGLONG end = GetTickCount64() + duration;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(10);
    } while (GetTickCount64() < end);
}

bool capture(HWND window, const std::filesystem::path &path, HWND backdrop = nullptr) {
    RECT rect{};
    if (!GetWindowRect(window, &rect)) return false;
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)
        || rect.left < monitor.rcMonitor.left || rect.top < monitor.rcMonitor.top
        || rect.right > monitor.rcMonitor.right || rect.bottom > monitor.rcMonitor.bottom) {
        std::fprintf(stderr, "BLOCKED: capture window extends beyond its monitor.\n");
        return false;
    }
    if (backdrop) {
        RECT background{};
        if (!GetWindowRect(backdrop, &background) || rect.left < background.left || rect.top < background.top
            || rect.right > background.right || rect.bottom > background.bottom) return false;
    }
    // Fail closed if another application's visible window covers the sample.
    for (LONG y = rect.top + 1; y < rect.bottom; y += 8) {
        for (LONG x = rect.left + 1; x < rect.right; x += 8) {
            const HWND hit = WindowFromPoint(POINT{x, y});
            if (hit != window && !IsChild(window, hit) && (!backdrop || hit != backdrop)) {
                DWORD process = 0;
                GetWindowThreadProcessId(hit, &process);
                std::fprintf(stderr, "BLOCKED: another window overlaps the capture rectangle at %ld,%ld (pid=%lu).\n",
                    x, y, process);
                return false;
            }
        }
    }
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    const HDC screen = GetDC(nullptr);
    if (!screen) return false;
    const HDC memory = CreateCompatibleDC(screen);
    const HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    if (!memory || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return false;
    }
    const HGDIOBJ previous = SelectObject(memory, bitmap);
    if (!previous || previous == HGDI_ERROR) {
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return false;
    }
    const BOOL copied = BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT);
    SelectObject(memory, previous);
    const CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    bool saved = false;
    if (copied) {
        Gdiplus::Bitmap image(bitmap, nullptr);
        saved = image.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok;
    }
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return saved;
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: screenshot_capture <output-directory>\nRun on an unlocked interactive Windows desktop.\n");
        return 2;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    wchar_t name[128]{};
    DWORD needed = 0;
    const bool accessible = desktop && GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed)
        && _wcsicmp(name, L"Default") == 0 && GetForegroundWindow() != nullptr;
    if (desktop) CloseDesktop(desktop);
    if (!accessible) {
        std::fprintf(stderr, "BLOCKED: no unlocked interactive Default desktop in this process session; screenshots were not captured.\n");
        return 77;
    }
    test::InteractiveLock lock(1000);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR graphics = 0;
    if (Gdiplus::GdiplusStartup(&graphics, &input, nullptr) != Gdiplus::Ok) return 2;
    const std::filesystem::path output(argv[1]);
    std::filesystem::create_directories(output);
    bool ok = true;
    {
        // Cover the desktop before either native window is shown, including
        // the Settings frame's rounded corners.
        CaptureBackdrop background;
        const HWND backdrop = background.hwnd;
        if (!backdrop) {
            std::fprintf(stderr, "FAILED: could not create the synthetic backdrop.\n");
            ok = false;
        } else {
            SetForegroundWindow(backdrop);
            pump(100);
            // Regression preflight: STATIC backdrops were skipped here,
            // falsely identifying clear bar corners as another application's pixels.
            if (WindowFromPoint(POINT{1, 1}) != backdrop) {
                std::fprintf(stderr, "BLOCKED: synthetic backdrop failed hit-test preflight.\n");
                ok = false;
            } else {
                SettingsServices services;
                services.refresh_models = [](const std::string &, std::function<void(SettingsCatalogResult)> done) {
                    SettingsCatalogResult result;
                    result.models = {{"Whisper-Base", true, false, false, true, false, true},
                        {"Sample-Cleanup", false, true, false, true, false, true}};
                    done(std::move(result));
                };
                services.list_devices = [] { return std::vector<SettingsDevice>{{L"sample-mic", L"Sample microphone"}}; };
                WinPreferences preferences;
                preferences.asr_model = "Whisper-Base";
                preferences.cleanup_model = "Sample-Cleanup";
                preferences.capture_device_id = L"sample-mic";
                SettingsWindow settings(GetModuleHandleW(nullptr), std::move(services), lv_cleanup_default_prompt());
                settings.show(preferences);
                settings.set_server_health(LV_SERVER_HEALTH_CONNECTED);
                pump(500);
                const UINT dpi = GetDpiForWindow(settings.hwnd());
                std::printf("Windows native synthetic fixture; theme=%s; dpi=%u; scale=%u%%; current Settings is one combined form, not separate General/Models pages.\n",
                    settings.dark_mode_active() ? "system-dark" : "system-light-or-high-contrast", dpi, dpi * 100 / 96);
                if (GetForegroundWindow() != settings.hwnd()) {
                    std::fprintf(stderr, "BLOCKED: Settings could not become foreground; refusing unrelated desktop pixels.\n");
                    ok = false;
                } else {
                    ok = capture(settings.hwnd(), output / L"windows-settings.png", backdrop) && ok;
                }
                settings.close();
                SetForegroundWindow(backdrop);
                pump(100);
                if (GetForegroundWindow() != backdrop) {
                    std::fprintf(stderr, "BLOCKED: synthetic backdrop is not foreground.\n");
                    ok = false;
                } else {
                    ui::IndicatorWindow bar({});
                    if (!bar.create(GetModuleHandleW(nullptr))) ok = false;
                    else {
                        ui::IndicatorState state;
                        state.mode = ui::IndicatorMode::recording;
                        state.level = 0.55;
                        state.elapsed = std::chrono::seconds(12);
                        bar.set_anchor(POINT{GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2});
                        bar.set_state(state);
                        pump(350);
                        ok = GetForegroundWindow() == backdrop && capture(bar.hwnd(), output / L"windows-recording.png", backdrop) && ok;
                        state.mode = ui::IndicatorMode::transcribing;
                        bar.set_state(state);
                        pump(350);
                        ok = GetForegroundWindow() == backdrop && capture(bar.hwnd(), output / L"windows-processing.png", backdrop) && ok;
                    }
                }
            }
        }
    }
    Gdiplus::GdiplusShutdown(graphics);
    CoUninitialize();
    std::printf("%s: screenshot capture\n", ok ? "PASS" : "FAILED");
    return ok ? 0 : 1;
}
