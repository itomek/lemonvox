#pragma once

// Notification-area icon and context menu. Left click (or Enter/Space on the
// focused icon) opens Settings, matching common Windows tray utilities; all
// dictation commands live in the right-click menu and the global shortcut.
// All members must be used on the thread that called create(), which must
// pump messages.

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lemonvox::win::ui {

enum class TrayActivity : std::uint8_t { idle, recording, processing, error };

struct TrayStatus {
    TrayActivity activity = TrayActivity::idle;
    bool copy_available = false; // fallback text is waiting to be copied
    std::wstring detail;         // optional tooltip suffix, e.g. a short error
};

enum class TrayCommand : UINT { toggle_dictation = 1, cancel, copy_last, settings, diagnostics, quit };

struct TrayMenuItem {
    TrayCommand command;
    std::wstring label;
    bool enabled;
    bool separator_before;
};

// Presentation mapping, kept pure for tests.
std::vector<TrayMenuItem> tray_menu_items(const TrayStatus &status, bool diagnostics_available);
// Tooltip text, already truncated to the 127-character shell limit.
std::wstring tray_tooltip(const TrayStatus &status);

// Invoked on the UI thread. Callbacks must not destroy the TrayIcon
// synchronously; quit should post (e.g. PostQuitMessage) instead.
struct TrayCallbacks {
    std::function<void()> toggle_dictation;
    std::function<void()> cancel;
    std::function<void()> copy_last;
    std::function<void()> open_settings;
    std::function<void()> open_diagnostics; // menu item omitted when empty
    std::function<void()> quit;
};

class TrayIcon {
public:
    explicit TrayIcon(TrayCallbacks callbacks);
    ~TrayIcon();
    TrayIcon(const TrayIcon &) = delete;
    TrayIcon &operator=(const TrayIcon &) = delete;

    // Creates the hidden owner window and adds the icon. Returns false only if
    // the window cannot be created; if Explorer is not running yet the icon is
    // added when it broadcasts TaskbarCreated.
    bool create(HINSTANCE instance);
    void destroy();

    void set_status(TrayStatus status);
    const TrayStatus &status() const;
    // Shows a balloon (a toast on Windows 10/11). Respects quiet time.
    bool notify(const std::wstring &title, const std::wstring &text, bool error);
    // Runs a menu command exactly as if it had been chosen from the menu.
    void invoke(TrayCommand command);

    HWND hwnd() const;
    bool icon_added() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lemonvox::win::ui
