#include "ui/theme.h"

#include <dwmapi.h>

#include <cwchar>

namespace lemonvox::win::ui {
namespace {

constexpr wchar_t kPersonalizeKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr COLORREF kDefaultAccent = RGB(0, 120, 212);

std::optional<DWORD> read_dword(const wchar_t *key, const wchar_t *name) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return std::nullopt;
    return value;
}

COLORREF readable_text_on(COLORREF color) {
    const int luminance = GetRValue(color) * 299 + GetGValue(color) * 587 + GetBValue(color) * 114;
    return luminance > 150000 ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

} // namespace

bool dark_from_apps_use_light_theme(std::optional<DWORD> value) { return value.has_value() && *value == 0; }

COLORREF colorref_from_abgr(DWORD abgr) { return static_cast<COLORREF>(abgr & 0x00FFFFFFu); }

COLORREF colorref_from_argb(DWORD argb) {
    return RGB((argb >> 16) & 0xFFu, (argb >> 8) & 0xFFu, argb & 0xFFu);
}

Palette make_palette(bool dark, bool high_contrast, COLORREF accent) {
    if (high_contrast) {
        return Palette{GetSysColor(COLOR_WINDOW),       GetSysColor(COLOR_BTNFACE),    GetSysColor(COLOR_WINDOWTEXT),
                       GetSysColor(COLOR_WINDOWTEXT),   GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_HIGHLIGHT),
                       GetSysColor(COLOR_HIGHLIGHTTEXT), GetSysColor(COLOR_WINDOWTEXT)};
    }
    if (dark) {
        return Palette{RGB(43, 43, 43), RGB(62, 62, 62), RGB(255, 255, 255), RGB(200, 200, 200),
                       RGB(84, 84, 84), accent,          readable_text_on(accent), RGB(255, 99, 99)};
    }
    return Palette{RGB(249, 249, 249), RGB(232, 232, 232), RGB(26, 26, 26), RGB(92, 92, 92),
                   RGB(208, 208, 208), accent,             readable_text_on(accent), RGB(196, 43, 28)};
}

bool apps_use_dark_theme() { return dark_from_apps_use_light_theme(read_dword(kPersonalizeKey, L"AppsUseLightTheme")); }

bool system_uses_dark_theme() {
    return dark_from_apps_use_light_theme(read_dword(kPersonalizeKey, L"SystemUsesLightTheme"));
}

bool high_contrast_enabled() {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
           (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

COLORREF accent_color() {
    if (const auto stored = read_dword(L"Software\\Microsoft\\Windows\\DWM", L"AccentColor"))
        return colorref_from_abgr(*stored);
    DWORD colorization = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&colorization, &opaque))) return colorref_from_argb(colorization);
    return kDefaultAccent;
}

Theme current_theme() {
    Theme theme;
    theme.high_contrast = high_contrast_enabled();
    theme.dark = !theme.high_contrast && apps_use_dark_theme();
    theme.palette = make_palette(theme.dark, theme.high_contrast, accent_color());
    return theme;
}

bool is_theme_change(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_SYSCOLORCHANGE || message == WM_THEMECHANGED) return true;
    if (message != WM_SETTINGCHANGE) return false;
    if (wparam == SPI_SETHIGHCONTRAST) return true;
    const auto *area = reinterpret_cast<const wchar_t *>(lparam);
    return area != nullptr && std::wcscmp(area, L"ImmersiveColorSet") == 0;
}

bool set_dark_title_bar(HWND window, bool dark) {
    const BOOL value = dark ? TRUE : FALSE;
    // 20 is DWMWA_USE_IMMERSIVE_DARK_MODE from Windows 10 20H1; 19 predates it.
    if (SUCCEEDED(DwmSetWindowAttribute(window, 20, &value, sizeof(value)))) return true;
    return SUCCEEDED(DwmSetWindowAttribute(window, 19, &value, sizeof(value)));
}

} // namespace lemonvox::win::ui
