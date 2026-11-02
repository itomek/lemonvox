#pragma once

// Appearance shared by every LemonVox window (indicator, tray, settings).
// All functions are cheap, stateless reads of the current user's settings and
// may be called from any thread; windows should re-read current_theme() when
// is_theme_change() reports a relevant broadcast.

#include <windows.h>

#include <optional>

namespace lemonvox::win::ui {

struct Palette {
    COLORREF background;     // window / bar fill
    COLORREF surface;        // secondary buttons
    COLORREF text;
    COLORREF secondary_text;
    COLORREF border;
    COLORREF accent;         // primary buttons, focus, progress
    COLORREF accent_text;    // text drawn on accent
    COLORREF danger;         // recording / error marks
};

struct Theme {
    bool dark = false;
    bool high_contrast = false;
    Palette palette{};
};

// HKCU\...\Themes\Personalize\AppsUseLightTheme: 0 means dark; a missing
// value (older builds, stripped policy) means light.
bool dark_from_apps_use_light_theme(std::optional<DWORD> value);
// HKCU\Software\Microsoft\Windows\DWM\AccentColor is stored as 0xAABBGGRR.
COLORREF colorref_from_abgr(DWORD abgr);
// DwmGetColorizationColor reports 0xAARRGGBB.
COLORREF colorref_from_argb(DWORD argb);
// High contrast palettes come from GetSysColor and ignore `dark`/`accent`.
Palette make_palette(bool dark, bool high_contrast, COLORREF accent);

bool apps_use_dark_theme();
// Taskbar / notification area appearance (SystemUsesLightTheme).
bool system_uses_dark_theme();
bool high_contrast_enabled();
COLORREF accent_color();
Theme current_theme();

// True for WM_SETTINGCHANGE "ImmersiveColorSet" or SPI_SETHIGHCONTRAST,
// WM_SYSCOLORCHANGE and WM_THEMECHANGED.
bool is_theme_change(UINT message, WPARAM wparam, LPARAM lparam);

// Applies DWMWA_USE_IMMERSIVE_DARK_MODE so a framed window's title bar
// matches the app theme. Returns false where the attribute is unsupported.
bool set_dark_title_bar(HWND window, bool dark);

} // namespace lemonvox::win::ui
