#include "ui/tray.h"

#include "ui/icon.h"
#include "ui/theme.h"

#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <cwchar>

namespace lemonvox::win::ui {
namespace {

constexpr wchar_t kClassName[] = L"LemonVoxTrayHost";
constexpr UINT kCallbackMessage = WM_APP + 1;
constexpr UINT kIconId = 1;
constexpr size_t kTooltipLimit = 127;

template <size_t N> void copy_text(wchar_t (&target)[N], const std::wstring &text) {
    const size_t count = text.size() < N - 1 ? text.size() : N - 1;
    std::wmemcpy(target, text.data(), count);
    target[count] = L'\0';
}

IconBadge badge_for(TrayActivity activity) {
    switch (activity) {
    case TrayActivity::recording: return IconBadge::recording;
    case TrayActivity::processing: return IconBadge::processing;
    case TrayActivity::error: return IconBadge::error;
    case TrayActivity::idle: break;
    }
    return IconBadge::none;
}

UINT primary_dpi() {
    UINT x = 96, y = 96;
    HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    return SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y)) ? x : 96;
}

} // namespace

std::vector<TrayMenuItem> tray_menu_items(const TrayStatus &status, bool diagnostics_available) {
    const bool recording = status.activity == TrayActivity::recording;
    const bool active = recording || status.activity == TrayActivity::processing;
    std::vector<TrayMenuItem> items;
    items.push_back({TrayCommand::toggle_dictation, recording ? L"Stop dictation" : L"Start dictation",
                     recording || !active, false});
    items.push_back({TrayCommand::cancel, L"Cancel", active, false});
    items.push_back({TrayCommand::copy_last, L"Copy last result", status.copy_available, false});
    items.push_back({TrayCommand::settings, L"Settings\u2026", true, true});
    if (diagnostics_available) items.push_back({TrayCommand::diagnostics, L"Open diagnostics", true, false});
    items.push_back({TrayCommand::quit, L"Quit LemonVox", true, true});
    return items;
}

std::wstring tray_tooltip(const TrayStatus &status) {
    std::wstring text = L"LemonVox";
    switch (status.activity) {
    case TrayActivity::idle: text += status.copy_available ? L" \u2013 Text ready to copy" : L" \u2013 Ready"; break;
    case TrayActivity::recording: text += L" \u2013 Listening"; break;
    case TrayActivity::processing: text += L" \u2013 Working"; break;
    case TrayActivity::error: text += L" \u2013 Error"; break;
    }
    if (!status.detail.empty()) text += L": " + status.detail;
    if (text.size() > kTooltipLimit) text = text.substr(0, kTooltipLimit - 1) + L"\u2026";
    return text;
}

struct TrayIcon::Impl {
    TrayCallbacks callbacks;
    TrayStatus status;
    HWND hwnd = nullptr;
    HICON icon = nullptr;
    UINT taskbar_created = 0;
    bool added = false;

    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);

    NOTIFYICONDATAW data(UINT flags) const {
        NOTIFYICONDATAW nid{sizeof(nid)};
        nid.hWnd = hwnd;
        nid.uID = kIconId;
        nid.uFlags = flags;
        return nid;
    }
    void refresh_icon() {
        HICON next = create_lemon_icon(tray_icon_size_for_dpi(primary_dpi()), badge_for(status.activity));
        if (!next) return;
        if (icon) DestroyIcon(icon);
        icon = next;
    }
    void add() {
        refresh_icon();
        NOTIFYICONDATAW nid = data(NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP);
        nid.uCallbackMessage = kCallbackMessage;
        nid.hIcon = icon;
        copy_text(nid.szTip, tray_tooltip(status));
        added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
        if (!added) added = Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
        if (added) {
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
        }
    }
    void update() {
        if (!added) return;
        refresh_icon();
        NOTIFYICONDATAW nid = data(NIF_ICON | NIF_TIP | NIF_SHOWTIP);
        nid.hIcon = icon;
        copy_text(nid.szTip, tray_tooltip(status));
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    void show_menu(POINT anchor);
    void invoke(TrayCommand command);
};

void TrayIcon::Impl::show_menu(POINT anchor) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    for (const auto &item : tray_menu_items(status, static_cast<bool>(callbacks.open_diagnostics))) {
        if (item.separator_before) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (item.enabled ? MF_ENABLED : MF_GRAYED), static_cast<UINT_PTR>(item.command),
                    item.label.c_str());
    }
    SetMenuDefaultItem(menu, static_cast<UINT>(TrayCommand::settings), FALSE);
    // Required so the menu closes when the user clicks elsewhere.
    SetForegroundWindow(hwnd);
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN;
    flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const UINT chosen = static_cast<UINT>(TrackPopupMenuEx(menu, flags, anchor.x, anchor.y, hwnd, nullptr));
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (chosen != 0) invoke(static_cast<TrayCommand>(chosen));
}

void TrayIcon::Impl::invoke(TrayCommand command) {
    std::function<void()> callback;
    for (const auto &item : tray_menu_items(status, static_cast<bool>(callbacks.open_diagnostics))) {
        if (item.command != command) continue;
        if (!item.enabled) return;
        switch (command) {
        case TrayCommand::toggle_dictation: callback = callbacks.toggle_dictation; break;
        case TrayCommand::cancel: callback = callbacks.cancel; break;
        case TrayCommand::copy_last: callback = callbacks.copy_last; break;
        case TrayCommand::settings: callback = callbacks.open_settings; break;
        case TrayCommand::diagnostics: callback = callbacks.open_diagnostics; break;
        case TrayCommand::quit: callback = callbacks.quit; break;
        }
    }
    if (callback) callback();
}

LRESULT CALLBACK TrayIcon::Impl::proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto *impl = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
        impl->hwnd = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
    }
    auto *impl = reinterpret_cast<Impl *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!impl) return DefWindowProcW(window, message, wparam, lparam);
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        impl->hwnd = nullptr;
        return DefWindowProcW(window, message, wparam, lparam);
    }
    return impl->handle(message, wparam, lparam);
}

LRESULT TrayIcon::Impl::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    if (taskbar_created != 0 && message == taskbar_created) {
        // Explorer restarted: every notification icon must be re-added.
        added = false;
        add();
        return 0;
    }
    switch (message) {
    case kCallbackMessage:
        switch (LOWORD(lparam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT: invoke(TrayCommand::settings); break;
        case WM_CONTEXTMENU: show_menu(POINT{GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam)}); break;
        default: break;
        }
        return 0;
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE: update(); break;
    default: break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

TrayIcon::TrayIcon(TrayCallbacks callbacks) : impl_(std::make_unique<Impl>()) {
    impl_->callbacks = std::move(callbacks);
}

TrayIcon::~TrayIcon() {
    destroy();
    if (impl_->icon) DestroyIcon(impl_->icon);
}

bool TrayIcon::create(HINSTANCE instance) {
    if (impl_->hwnd) return true;
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = &Impl::proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kClassName;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    // A hidden top-level window (not HWND_MESSAGE) so it receives the
    // TaskbarCreated and settings broadcasts.
    CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"LemonVox", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance,
                    impl_.get());
    if (!impl_->hwnd) return false;
    impl_->taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(impl_->hwnd, impl_->taskbar_created, MSGFLT_ALLOW, nullptr);
    impl_->add();
    return true;
}

void TrayIcon::destroy() {
    if (!impl_->hwnd) return;
    if (impl_->added) {
        NOTIFYICONDATAW nid = impl_->data(0);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        impl_->added = false;
    }
    DestroyWindow(impl_->hwnd);
}

void TrayIcon::set_status(TrayStatus status) {
    const bool icon_changed = status.activity != impl_->status.activity;
    impl_->status = std::move(status);
    if (!impl_->added) return;
    if (icon_changed) {
        impl_->update();
        return;
    }
    NOTIFYICONDATAW nid = impl_->data(NIF_TIP | NIF_SHOWTIP);
    copy_text(nid.szTip, tray_tooltip(impl_->status));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

const TrayStatus &TrayIcon::status() const { return impl_->status; }

bool TrayIcon::notify(const std::wstring &title, const std::wstring &text, bool error) {
    if (!impl_->added) return false;
    NOTIFYICONDATAW nid = impl_->data(NIF_INFO);
    copy_text(nid.szInfoTitle, title);
    copy_text(nid.szInfo, text);
    nid.dwInfoFlags = (error ? NIIF_ERROR : NIIF_INFO) | NIIF_RESPECT_QUIET_TIME;
    return Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
}

void TrayIcon::invoke(TrayCommand command) { impl_->invoke(command); }

HWND TrayIcon::hwnd() const { return impl_->hwnd; }

bool TrayIcon::icon_added() const { return impl_->added; }

} // namespace lemonvox::win::ui
