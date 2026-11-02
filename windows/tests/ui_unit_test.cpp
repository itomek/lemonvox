#include "ui/icon.h"
#include "ui/indicator.h"
#include "ui/theme.h"
#include "ui/tray.h"

#include <windows.h>

#include <cassert>
#include <chrono>
#include <cstdio>

using namespace lemonvox::win::ui;
using namespace std::chrono_literals;

static bool same(const RECT &a, const RECT &b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

static void theme_parsing() {
    assert(!dark_from_apps_use_light_theme(std::nullopt));
    assert(dark_from_apps_use_light_theme(0));
    assert(!dark_from_apps_use_light_theme(1));
    assert(colorref_from_abgr(0xFFD47800u) == RGB(0x00, 0x78, 0xD4));
    assert(colorref_from_argb(0xC40078D4u) == RGB(0x00, 0x78, 0xD4));

    assert(is_theme_change(WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ImmersiveColorSet")));
    assert(!is_theme_change(WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment")));
    assert(!is_theme_change(WM_SETTINGCHANGE, SPI_SETWORKAREA, 0));
    assert(is_theme_change(WM_SETTINGCHANGE, SPI_SETHIGHCONTRAST, 0));
    assert(is_theme_change(WM_THEMECHANGED, 0, 0));
    assert(is_theme_change(WM_SYSCOLORCHANGE, 0, 0));
    assert(!is_theme_change(WM_PAINT, 0, 0));

    const Palette dark = make_palette(true, false, RGB(255, 200, 0));
    const Palette light = make_palette(false, false, RGB(0, 50, 120));
    assert(dark.text == RGB(255, 255, 255) && dark.accent_text == RGB(0, 0, 0));
    assert(light.text == RGB(26, 26, 26) && light.accent_text == RGB(255, 255, 255));
    const Palette contrast = make_palette(true, true, RGB(1, 2, 3));
    assert(contrast.background == GetSysColor(COLOR_WINDOW) && contrast.accent == GetSysColor(COLOR_HIGHLIGHT));

    const Theme theme = current_theme();
    assert(!(theme.dark && theme.high_contrast));
}

static void clamping() {
    const RECT work{0, 0, 1920, 1040};
    assert(same(clamp_rect_to_work_area(RECT{100, 100, 340, 144}, work), RECT{100, 100, 340, 144}));
    assert(same(clamp_rect_to_work_area(RECT{1800, 1020, 2040, 1064}, work), RECT{1680, 996, 1920, 1040}));
    assert(same(clamp_rect_to_work_area(RECT{-50, -20, 190, 24}, work), RECT{0, 0, 240, 44}));
    // A secondary monitor left of and above the primary uses negative coordinates.
    const RECT left{-2560, -200, 0, 1240};
    assert(same(clamp_rect_to_work_area(RECT{-100, 500, 140, 544}, left), RECT{-240, 500, 0, 544}));
    // Larger than the work area: pinned to the top-left.
    assert(same(clamp_rect_to_work_area(RECT{50, 50, 2050, 1150}, work), RECT{0, 0, 2000, 1100}));
}

static void indicator_text() {
    assert(format_elapsed(0ms) == L"0:00");
    assert(format_elapsed(65s) == L"1:05");
    assert(format_elapsed(3725s) == L"1:02:05");
    assert(format_elapsed(-5s) == L"0:00");

    assert(indicator_mode_for_phase(lemonvox::WorkflowPhase::idle) == IndicatorMode::hidden);
    assert(indicator_mode_for_phase(lemonvox::WorkflowPhase::preparing) == IndicatorMode::preparing);
    assert(indicator_mode_for_phase(lemonvox::WorkflowPhase::recording) == IndicatorMode::recording);
    assert(indicator_mode_for_phase(lemonvox::WorkflowPhase::transcribing) == IndicatorMode::transcribing);
    assert(indicator_mode_for_phase(lemonvox::WorkflowPhase::cleaning) == IndicatorMode::cleaning);
    assert(indicator_mode_for_phase(lemonvox::WorkflowPhase::inserting) == IndicatorMode::inserting);

    IndicatorState state;
    assert(indicator_status_text(state).empty() && indicator_buttons(state).empty());
    state.mode = IndicatorMode::recording;
    state.elapsed = 7s;
    assert(indicator_status_text(state) == L"Listening  0:07");
    assert(indicator_accessible_name(state) == L"LemonVox: Listening");
    assert((indicator_buttons(state) == std::vector<IndicatorButton>{IndicatorButton::cancel, IndicatorButton::stop}));
    state.mode = IndicatorMode::transcribing;
    assert(indicator_status_text(state) == L"Transcribing\u2026");
    assert(indicator_buttons(state) == std::vector<IndicatorButton>{IndicatorButton::cancel});
    state.mode = IndicatorMode::cleaning;
    assert(indicator_status_text(state) == L"Cleaning up\u2026");
    state.mode = IndicatorMode::inserting;
    assert(indicator_status_text(state) == L"Inserting\u2026" && indicator_buttons(state).empty());
    state.mode = IndicatorMode::success;
    assert(indicator_status_text(state) == L"Inserted");
    state.mode = IndicatorMode::error;
    state.message = L"Microphone is off.";
    assert(indicator_accessible_name(state) == L"LemonVox error: Microphone is off.");
    assert(indicator_buttons(state) == std::vector<IndicatorButton>{IndicatorButton::dismiss});
    state.action_label = L"Open microphone settings";
    assert((indicator_buttons(state) == std::vector<IndicatorButton>{IndicatorButton::dismiss, IndicatorButton::action}));
    assert(indicator_button_label(IndicatorButton::action, state) == L"Open microphone settings");
    state = IndicatorState{};
    state.mode = IndicatorMode::copy_fallback;
    assert(!indicator_status_text(state).empty());
    assert((indicator_buttons(state) == std::vector<IndicatorButton>{IndicatorButton::dismiss, IndicatorButton::copy}));
    assert(indicator_accessible_name(state).find(L"Copy") != std::wstring::npos);
    state = IndicatorState{};
    state.mode = IndicatorMode::notice;
    state.message = L"Administrator window";
    assert(indicator_status_text(state) == L"Administrator window" && indicator_buttons(state).empty());
    assert(indicator_accessible_name(state) == L"LemonVox: Administrator window");
}

static void tray_text() {
    TrayStatus status;
    auto items = tray_menu_items(status, false);
    assert(items.size() == 5);
    assert(items[0].command == TrayCommand::toggle_dictation && items[0].label == L"Start dictation" && items[0].enabled);
    assert(items[1].command == TrayCommand::cancel && !items[1].enabled);
    assert(items[2].command == TrayCommand::copy_last && !items[2].enabled);
    assert(items[3].command == TrayCommand::settings && items[3].separator_before);
    assert(items[4].command == TrayCommand::quit && items[4].separator_before);
    assert(tray_tooltip(status) == L"LemonVox \u2013 Ready");

    status.activity = TrayActivity::recording;
    items = tray_menu_items(status, true);
    assert(items.size() == 6 && items[4].command == TrayCommand::diagnostics);
    assert(items[0].label == L"Stop dictation" && items[0].enabled && items[1].enabled);

    status.activity = TrayActivity::processing;
    items = tray_menu_items(status, false);
    assert(items[0].label == L"Start dictation" && !items[0].enabled && items[1].enabled);

    status.activity = TrayActivity::idle;
    status.copy_available = true;
    assert(tray_menu_items(status, false)[2].enabled);

    status.activity = TrayActivity::error;
    status.detail.assign(300, L'x');
    const auto tip = tray_tooltip(status);
    assert(tip.size() == 127 && tip.back() == L'\u2026');
}

static void icons() {
    for (const int size : {16, 20, 24, 32}) {
        for (const IconBadge badge : {IconBadge::none, IconBadge::recording, IconBadge::processing, IconBadge::error}) {
            HICON icon = create_lemon_icon(size, badge);
            assert(icon != nullptr);
            ICONINFO info{};
            assert(GetIconInfo(icon, &info));
            BITMAP bitmap{};
            assert(GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) == sizeof(bitmap));
            assert(bitmap.bmWidth == size && bitmap.bmHeight == size && bitmap.bmBitsPixel == 32);
            DeleteObject(info.hbmColor);
            DeleteObject(info.hbmMask);
            DestroyIcon(icon);
        }
    }
    assert(create_lemon_icon(0) == nullptr);
    assert(tray_icon_size_for_dpi(96) == 16 && tray_icon_size_for_dpi(192) == 32);

    const auto ico = encode_app_icon({16, 32, 256});
    assert(ico.size() > 6 + 3 * 16);
    assert(ico[0] == 0 && ico[1] == 0 && ico[2] == 1 && ico[3] == 0 && ico[4] == 3 && ico[5] == 0);
    assert(ico[6] == 16 && ico[22] == 32 && ico[38] == 0);
    const size_t first = ico[18] | (ico[19] << 8) | (ico[20] << 16) | (static_cast<size_t>(ico[21]) << 24);
    assert(first == 6 + 3 * 16 && ico[first + 1] == 'P' && ico[first + 2] == 'N' && ico[first + 3] == 'G');
    assert(encode_app_icon({}).empty() && encode_app_icon({512}).empty());
}

int main() {
    theme_parsing();
    clamping();
    indicator_text();
    tray_text();
    icons();
    std::puts("ui_unit_test passed");
    return 0;
}
