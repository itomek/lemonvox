#include "ui/indicator.h"

#include "ui/graphics.h"
#include "ui/theme.h"

#include <shellscalingapi.h>
#include <windowsx.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace lemonvox::win::ui {
namespace {

constexpr wchar_t kClassName[] = L"LemonVoxIndicator";
constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT_PTR kAutoHideTimer = 2;
constexpr UINT kAnimationIntervalMs = 80;
constexpr int kMaxMessageLines = 4;

bool is_processing(IndicatorMode mode) {
    return mode == IndicatorMode::preparing || mode == IndicatorMode::transcribing || mode == IndicatorMode::cleaning ||
           mode == IndicatorMode::inserting;
}

bool is_message(IndicatorMode mode) {
    return mode == IndicatorMode::error || mode == IndicatorMode::copy_fallback || mode == IndicatorMode::notice;
}

constexpr UINT kNoticeHideMs = 4500;

bool reduced_motion() {
    BOOL animations = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0) && !animations;
}

UINT monitor_dpi(HMONITOR monitor) {
    UINT x = 96, y = 96;
    return SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y)) ? x : 96;
}

RECT work_area(HMONITOR monitor) {
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    return info.rcWork;
}

POINT default_anchor() {
    HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    const RECT work = work_area(monitor);
    const float scale = static_cast<float>(monitor_dpi(monitor)) / 96.0f;
    return POINT{(work.left + work.right) / 2, work.bottom - std::lround(48.0f * scale)};
}

std::unique_ptr<Gdiplus::Font> make_font(float pixels, bool strong) {
    Gdiplus::FontFamily family(strong ? L"Segoe UI Semibold" : L"Segoe UI");
    if (family.GetLastStatus() == Gdiplus::Ok) {
        auto font = std::make_unique<Gdiplus::Font>(&family, pixels, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        if (font->GetLastStatus() == Gdiplus::Ok) return font;
    }
    return std::make_unique<Gdiplus::Font>(Gdiplus::FontFamily::GenericSansSerif(), pixels,
                                           strong ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
                                           Gdiplus::UnitPixel);
}

struct ButtonLayout {
    IndicatorButton id;
    Gdiplus::RectF rect;
    std::wstring label;
    bool glyph;
    bool primary;
};

struct Layout {
    int width = 0;
    int height = 0;
    float scale = 1.0f;
    float radius = 0.0f;
    std::vector<ButtonLayout> buttons;
    Gdiplus::RectF meter, spinner, mark, text;
    std::wstring label;
};

} // namespace

IndicatorMode indicator_mode_for_phase(lemonvox::WorkflowPhase phase) {
    switch (phase) {
    case lemonvox::WorkflowPhase::preparing: return IndicatorMode::preparing;
    case lemonvox::WorkflowPhase::recording: return IndicatorMode::recording;
    case lemonvox::WorkflowPhase::transcribing: return IndicatorMode::transcribing;
    case lemonvox::WorkflowPhase::cleaning: return IndicatorMode::cleaning;
    case lemonvox::WorkflowPhase::inserting: return IndicatorMode::inserting;
    case lemonvox::WorkflowPhase::idle: break;
    }
    return IndicatorMode::hidden;
}

std::wstring format_elapsed(std::chrono::milliseconds elapsed) {
    const long long total = elapsed.count() < 0 ? 0 : elapsed.count() / 1000;
    wchar_t buffer[32];
    if (total >= 3600)
        std::swprintf(buffer, 32, L"%lld:%02lld:%02lld", total / 3600, (total / 60) % 60, total % 60);
    else
        std::swprintf(buffer, 32, L"%lld:%02lld", total / 60, total % 60);
    return buffer;
}

std::wstring indicator_status_text(const IndicatorState &state) {
    switch (state.mode) {
    case IndicatorMode::hidden: return L"";
    case IndicatorMode::preparing: return L"Preparing\u2026";
    case IndicatorMode::recording: return L"Listening  " + format_elapsed(state.elapsed);
    case IndicatorMode::transcribing: return L"Transcribing\u2026";
    case IndicatorMode::cleaning: return L"Cleaning up\u2026";
    case IndicatorMode::inserting: return L"Pasting\u2026";
    case IndicatorMode::success: return state.message.empty() ? L"Inserted" : state.message;
    case IndicatorMode::notice: return state.message;
    case IndicatorMode::error: return state.message.empty() ? L"Dictation failed." : state.message;
    case IndicatorMode::copy_fallback:
        return state.message.empty() ? L"Couldn\u2019t insert the text. Copy it instead." : state.message;
    }
    return L"";
}

std::wstring indicator_accessible_name(const IndicatorState &state) {
    switch (state.mode) {
    case IndicatorMode::hidden: return L"LemonVox";
    case IndicatorMode::recording: return L"LemonVox: Listening";
    case IndicatorMode::error: return L"LemonVox error: " + indicator_status_text(state);
    case IndicatorMode::copy_fallback:
        return L"LemonVox: " + indicator_status_text(state) + L" Copy is also available from the tray menu.";
    default: return L"LemonVox: " + indicator_status_text(state);
    }
}

std::wstring indicator_button_label(IndicatorButton button, const IndicatorState &state) {
    switch (button) {
    case IndicatorButton::cancel: return L"Cancel dictation";
    case IndicatorButton::stop: return L"Finish recording";
    case IndicatorButton::copy: return L"Copy";
    case IndicatorButton::dismiss: return L"Dismiss";
    case IndicatorButton::action: return state.action_label;
    }
    return L"";
}

std::vector<IndicatorButton> indicator_buttons(const IndicatorState &state) {
    switch (state.mode) {
    case IndicatorMode::recording: return {IndicatorButton::cancel, IndicatorButton::stop};
    case IndicatorMode::preparing:
    case IndicatorMode::transcribing:
    case IndicatorMode::cleaning: return {IndicatorButton::cancel};
    case IndicatorMode::error:
        if (state.action_label.empty()) return {IndicatorButton::dismiss};
        return {IndicatorButton::dismiss, IndicatorButton::action};
    case IndicatorMode::copy_fallback: return {IndicatorButton::dismiss, IndicatorButton::copy};
    default: return {};
    }
}

RECT clamp_rect_to_work_area(const RECT &window, const RECT &work) {
    const LONG width = window.right - window.left;
    const LONG height = window.bottom - window.top;
    LONG left = window.left;
    LONG top = window.top;
    if (left + width > work.right) left = work.right - width;
    if (top + height > work.bottom) top = work.bottom - height;
    if (left < work.left) left = work.left;
    if (top < work.top) top = work.top;
    return RECT{left, top, left + width, top + height};
}

struct IndicatorWindow::Impl {
    IndicatorCallbacks callbacks;
    HWND hwnd = nullptr;
    IndicatorState state;
    Theme theme = current_theme();
    Layout layout;
    std::optional<POINT> anchor;
    std::chrono::milliseconds auto_hide{900};
    std::optional<IndicatorButton> hovered;
    std::optional<IndicatorButton> pressed;
    bool drag_armed = false;
    bool dragging = false;
    POINT drag_cursor{};
    POINT drag_origin{};
    bool tracking_leave = false;
    float tick = 0.0f;
    unsigned activations = 0;

    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);

    POINT effective_anchor() const { return anchor ? *anchor : default_anchor(); }
    Layout compute_layout(float scale) const;
    void paint(Gdiplus::Graphics &graphics) const;
    void paint_button(Gdiplus::Graphics &graphics, const ButtonLayout &button) const;
    void render(bool reposition);
    void hide();
    void update_timers();
    std::optional<IndicatorButton> hit_test(POINT client) const;
    void fire(IndicatorButton button);
    void finish_drag();
    void set_hovered(std::optional<IndicatorButton> button);
    void update_name();
};

void IndicatorWindow::Impl::update_name() {
    const std::wstring name = indicator_accessible_name(state);
    wchar_t current[512] = {};
    GetWindowTextW(hwnd, current, 512);
    // SetWindowText raises EVENT_OBJECT_NAMECHANGE for screen readers.
    if (name != current) SetWindowTextW(hwnd, name.c_str());
}

Layout IndicatorWindow::Impl::compute_layout(float k) const {
    using Gdiplus::RectF;
    Layout out;
    out.scale = k;
    out.label = indicator_status_text(state);
    const auto ids = indicator_buttons(state);
    if (!is_message(state.mode)) {
        const float width = 240 * k, height = 44 * k;
        out.width = static_cast<int>(std::ceil(width));
        out.height = static_cast<int>(std::ceil(height));
        out.radius = 22 * k;
        float text_left = 44 * k;
        float text_right = width - 14 * k;
        for (const IndicatorButton id : ids) {
            const RectF rect = id == IndicatorButton::stop ? RectF(width - 38 * k, 6 * k, 32 * k, 32 * k)
                                                           : RectF(6 * k, 6 * k, 32 * k, 32 * k);
            out.buttons.push_back({id, rect, indicator_button_label(id, state), true, false});
        }
        if (state.mode == IndicatorMode::recording) {
            out.meter = RectF(46 * k, 8 * k, 43 * k, 28 * k);
            text_left = 98 * k;
            text_right = width - 44 * k;
        } else if (is_processing(state.mode)) {
            const float x = ids.empty() ? 14 * k : 48 * k;
            out.spinner = RectF(x, 12 * k, 20 * k, 20 * k);
            text_left = x + 30 * k;
        } else {
            out.mark = RectF(14 * k, 12 * k, 20 * k, 20 * k);
        }
        out.text = RectF(text_left, 0, text_right - text_left, height);
        return out;
    }

    const float width = 360 * k, pad = 16 * k, button_height = 28 * k;
    Gdiplus::Bitmap scratch(1, 1, PixelFormat32bppPARGB);
    Gdiplus::Graphics measure(&scratch);
    measure.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    const auto font = make_font(13 * k, false);
    const auto button_font = make_font(13 * k, true);
    RectF bounds;
    measure.MeasureString(out.label.c_str(), static_cast<INT>(out.label.size()), font.get(),
                          RectF(0, 0, width - 2 * pad, 10000 * k), nullptr, &bounds);
    const float line = font->GetHeight(&measure);
    const float text_height = std::max(line, std::min(bounds.Height, line * kMaxMessageLines));
    out.text = RectF(pad, 14 * k, width - 2 * pad, text_height);
    const float row = out.text.Y + text_height + 12 * k;
    float right = width - pad;
    for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
        const std::wstring label = indicator_button_label(*it, state);
        measure.MeasureString(label.c_str(), static_cast<INT>(label.size()), button_font.get(),
                              Gdiplus::PointF(0, 0), &bounds);
        const float button_width = std::max(72 * k, std::ceil(bounds.Width) + 24 * k);
        right -= button_width;
        const bool primary = *it == IndicatorButton::copy || *it == IndicatorButton::action;
        out.buttons.insert(out.buttons.begin(),
                           {*it, RectF(right, row, button_width, button_height), label, false, primary});
        right -= 8 * k;
    }
    out.width = static_cast<int>(std::ceil(width));
    out.height = static_cast<int>(std::ceil(ids.empty() ? out.text.Y + text_height + 14 * k
                                                        : row + button_height + 14 * k));
    out.radius = 14 * k;
    return out;
}

void IndicatorWindow::Impl::paint_button(Gdiplus::Graphics &g, const ButtonLayout &button) const {
    const Palette &p = theme.palette;
    const float k = layout.scale;
    const bool hot = hovered == button.id;
    const bool down = pressed == button.id && hot;
    Gdiplus::GraphicsPath shape;
    if (button.glyph)
        shape.AddEllipse(button.rect);
    else
        add_round_rect(shape, button.rect, 6 * k);

    COLORREF fill = button.primary ? p.accent : p.surface;
    COLORREF ink = button.primary ? p.accent_text : p.text;
    if (theme.high_contrast && hot && !button.primary) {
        fill = p.accent;
        ink = p.accent_text;
    }
    Gdiplus::SolidBrush brush(gdip_color(fill));
    g.FillPath(&brush, &shape);
    if (!theme.high_contrast && (hot || down)) {
        Gdiplus::SolidBrush overlay(gdip_color(theme.dark || button.primary ? RGB(255, 255, 255) : RGB(0, 0, 0),
                                               static_cast<BYTE>(down ? 48 : 24)));
        g.FillPath(&overlay, &shape);
    }
    if (theme.high_contrast) {
        Gdiplus::Pen outline(gdip_color(p.border), std::max(1.0f, k));
        g.DrawPath(&outline, &shape);
    }

    const Gdiplus::Color ink_color = gdip_color(ink);
    const float cx = button.rect.X + button.rect.Width / 2;
    const float cy = button.rect.Y + button.rect.Height / 2;
    if (button.id == IndicatorButton::cancel) {
        Gdiplus::Pen pen(ink_color, 1.75f * k);
        pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
        const float half = 5 * k;
        g.DrawLine(&pen, cx - half, cy - half, cx + half, cy + half);
        g.DrawLine(&pen, cx - half, cy + half, cx + half, cy - half);
        return;
    }
    if (button.id == IndicatorButton::stop) {
        Gdiplus::GraphicsPath square;
        add_round_rect(square, Gdiplus::RectF(cx - 5 * k, cy - 5 * k, 10 * k, 10 * k), 2 * k);
        Gdiplus::SolidBrush ink_brush(ink_color);
        g.FillPath(&ink_brush, &square);
        return;
    }
    const auto font = make_font(13 * k, true);
    Gdiplus::StringFormat format;
    format.SetAlignment(Gdiplus::StringAlignmentCenter);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::SolidBrush ink_brush(ink_color);
    g.DrawString(button.label.c_str(), static_cast<INT>(button.label.size()), font.get(), button.rect, &format,
                 &ink_brush);
}

void IndicatorWindow::Impl::paint(Gdiplus::Graphics &g) const {
    const Palette &p = theme.palette;
    const float k = layout.scale;
    const float width = static_cast<float>(layout.width);
    const float height = static_cast<float>(layout.height);

    Gdiplus::GraphicsPath shape;
    add_round_rect(shape, Gdiplus::RectF(0.5f, 0.5f, width - 1.0f, height - 1.0f), layout.radius);
    Gdiplus::SolidBrush background(gdip_color(p.background, theme.high_contrast ? 255 : 246));
    g.FillPath(&background, &shape);
    Gdiplus::Pen border(gdip_color(p.border), theme.high_contrast ? 2 * k : 1.0f);
    g.DrawPath(&border, &shape);

    for (const auto &button : layout.buttons) paint_button(g, button);

    if (layout.meter.Width > 0) {
        COLORREF bar = theme.high_contrast ? p.accent : theme.dark ? RGB(255, 214, 10) : RGB(255, 149, 0);
        Gdiplus::SolidBrush brush(gdip_color(bar));
        const double level = std::clamp(state.level, 0.0, 1.0);
        for (int index = 0; index < 7; ++index) {
            const double amplitude = level * (0.45 + 0.55 * std::fabs(std::sin(index * 0.8 + tick)));
            const float bar_height = (4.0f + 18.0f * static_cast<float>(amplitude)) * k;
            Gdiplus::GraphicsPath path;
            add_round_rect(path,
                           Gdiplus::RectF(layout.meter.X + (index * 6.0f + 1.0f) * k,
                                          layout.meter.Y + (layout.meter.Height - bar_height) / 2, 3 * k, bar_height),
                           1.5f * k);
            g.FillPath(&brush, &path);
        }
    }
    if (layout.spinner.Width > 0) {
        const float cx = layout.spinner.X + layout.spinner.Width / 2;
        const float cy = layout.spinner.Y + layout.spinner.Height / 2;
        const float radius = 7 * k, dot = 1.8f * k;
        const int head = static_cast<int>(tick) % 8;
        for (int index = 0; index < 8; ++index) {
            const int age = (head - index + 8) % 8;
            const BYTE alpha = reduced_motion() ? BYTE{200} : static_cast<BYTE>(255 - age * 26);
            Gdiplus::SolidBrush brush(gdip_color(theme.high_contrast ? p.text : p.accent, alpha));
            const double angle = index * 3.14159265358979 / 4.0;
            g.FillEllipse(&brush, cx + radius * static_cast<float>(std::cos(angle)) - dot,
                          cy + radius * static_cast<float>(std::sin(angle)) - dot, dot * 2, dot * 2);
        }
    }
    if (layout.mark.Width > 0) {
        Gdiplus::Pen pen(gdip_color(theme.high_contrast ? p.text : p.accent), 2.25f * k);
        pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        const Gdiplus::RectF &m = layout.mark;
        const Gdiplus::PointF check[] = {{m.X + 3 * k, m.Y + 10 * k}, {m.X + 8 * k, m.Y + 15 * k},
                                         {m.X + 17 * k, m.Y + 5 * k}};
        g.DrawLines(&pen, check, 3);
    }

    const bool message = is_message(state.mode);
    const auto font = make_font(13 * k, !message);
    Gdiplus::StringFormat format;
    format.SetLineAlignment(message ? Gdiplus::StringAlignmentNear : Gdiplus::StringAlignmentCenter);
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    if (!message) format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::SolidBrush ink(gdip_color(p.text));
    g.DrawString(layout.label.c_str(), static_cast<INT>(layout.label.size()), font.get(), layout.text, &format, &ink);
}

void IndicatorWindow::Impl::render(bool reposition) {
    if (!hwnd) return;
    if (state.mode == IndicatorMode::hidden) {
        hide();
        return;
    }
    const bool place = reposition && !dragging && !drag_armed;
    HMONITOR monitor = nullptr;
    UINT dpi = 96;
    if (place) {
        monitor = MonitorFromPoint(effective_anchor(), MONITOR_DEFAULTTONEAREST);
        dpi = monitor_dpi(monitor);
    } else {
        dpi = GetDpiForWindow(hwnd);
    }
    layout = compute_layout(static_cast<float>(dpi == 0 ? 96 : dpi) / 96.0f);

    RECT target{};
    if (place) {
        const POINT a = effective_anchor();
        const LONG left = a.x - layout.width / 2;
        target = clamp_rect_to_work_area(RECT{left, a.y - layout.height, left + layout.width, a.y}, work_area(monitor));
    }

    Canvas canvas(layout.width, layout.height, PixelFormat32bppPARGB);
    if (!canvas.ok()) return;
    paint(canvas.graphics());
    HBITMAP bitmap = canvas.finish();

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    POINT source{0, 0};
    POINT destination{target.left, target.top};
    SIZE size{layout.width, layout.height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, screen, place ? &destination : nullptr, &size, memory, &source, 0, &blend, ULW_ALPHA);
    SelectObject(memory, previous);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);

    if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    if (place)
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void IndicatorWindow::Impl::hide() {
    if (!hwnd) return;
    KillTimer(hwnd, kAnimationTimer);
    KillTimer(hwnd, kAutoHideTimer);
    pressed.reset();
    hovered.reset();
    drag_armed = dragging = false;
    if (GetCapture() == hwnd) ReleaseCapture();
    ShowWindow(hwnd, SW_HIDE);
}

void IndicatorWindow::Impl::update_timers() {
    KillTimer(hwnd, kAnimationTimer);
    KillTimer(hwnd, kAutoHideTimer);
    if (is_processing(state.mode) && !reduced_motion()) SetTimer(hwnd, kAnimationTimer, kAnimationIntervalMs, nullptr);
    if (state.mode == IndicatorMode::success)
        SetTimer(hwnd, kAutoHideTimer, static_cast<UINT>(std::max<long long>(1, auto_hide.count())), nullptr);
    if (state.mode == IndicatorMode::notice) SetTimer(hwnd, kAutoHideTimer, kNoticeHideMs, nullptr);
}

std::optional<IndicatorButton> IndicatorWindow::Impl::hit_test(POINT client) const {
    const float x = static_cast<float>(client.x), y = static_cast<float>(client.y);
    for (const auto &button : layout.buttons) {
        const auto &r = button.rect;
        if (x >= r.X && x < r.X + r.Width && y >= r.Y && y < r.Y + r.Height) return button.id;
    }
    return std::nullopt;
}

void IndicatorWindow::Impl::set_hovered(std::optional<IndicatorButton> button) {
    if (hovered == button) return;
    hovered = button;
    render(false);
}

void IndicatorWindow::Impl::fire(IndicatorButton button) {
    std::function<void()> callback;
    switch (button) {
    case IndicatorButton::cancel: callback = callbacks.cancel; break;
    case IndicatorButton::stop: callback = callbacks.stop; break;
    case IndicatorButton::copy: callback = callbacks.copy; break;
    case IndicatorButton::action: callback = callbacks.action; break;
    case IndicatorButton::dismiss:
        callback = callbacks.dismiss;
        state = IndicatorState{};
        update_name();
        hide();
        break;
    }
    if (callback) callback();
}

void IndicatorWindow::Impl::finish_drag() {
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    const RECT clamped = clamp_rect_to_work_area(rect, work_area(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST)));
    if (clamped.left != rect.left || clamped.top != rect.top) {
        SetWindowPos(hwnd, nullptr, clamped.left, clamped.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    anchor = POINT{(clamped.left + clamped.right) / 2, clamped.bottom};
    if (callbacks.position_changed) callbacks.position_changed(*anchor);
}

LRESULT CALLBACK IndicatorWindow::Impl::proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
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

LRESULT IndicatorWindow::Impl::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ACTIVATE:
        if (LOWORD(wparam) != WA_INACTIVE) ++activations;
        break;
    case WM_SETFOCUS: ++activations; break;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            POINT cursor{};
            GetCursorPos(&cursor);
            ScreenToClient(hwnd, &cursor);
            SetCursor(LoadCursorW(nullptr, hit_test(cursor) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        const POINT client{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        SetCapture(hwnd);
        if (const auto button = hit_test(client)) {
            pressed = button;
            hovered = button;
            render(false);
        } else {
            drag_armed = true;
            dragging = false;
            drag_cursor = client;
            ClientToScreen(hwnd, &drag_cursor);
            RECT rect{};
            GetWindowRect(hwnd, &rect);
            drag_origin = POINT{rect.left, rect.top};
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const POINT client{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (drag_armed) {
            POINT screen = client;
            ClientToScreen(hwnd, &screen);
            const LONG dx = screen.x - drag_cursor.x, dy = screen.y - drag_cursor.y;
            if (!dragging && (std::labs(dx) >= GetSystemMetrics(SM_CXDRAG) || std::labs(dy) >= GetSystemMetrics(SM_CYDRAG)))
                dragging = true;
            if (dragging) {
                SetWindowPos(hwnd, nullptr, drag_origin.x + dx, drag_origin.y + dy, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
            return 0;
        }
        if (!tracking_leave) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            tracking_leave = TrackMouseEvent(&track) != FALSE;
        }
        set_hovered(hit_test(client));
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_leave = false;
        if (!pressed) set_hovered(std::nullopt);
        return 0;
    case WM_LBUTTONUP: {
        const POINT client{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (pressed) {
            const IndicatorButton button = *pressed;
            const bool inside = hit_test(client) == button;
            pressed.reset();
            if (GetCapture() == hwnd) ReleaseCapture();
            render(false);
            if (inside) fire(button);
        } else if (drag_armed) {
            const bool moved = dragging;
            drag_armed = dragging = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            if (moved) finish_drag();
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (pressed) {
            pressed.reset();
            render(false);
        }
        if (drag_armed) {
            const bool moved = dragging;
            drag_armed = dragging = false;
            if (moved) finish_drag();
        }
        return 0;
    case WM_TIMER:
        if (wparam == kAnimationTimer) {
            tick += 1.0f;
            render(false);
        } else if (wparam == kAutoHideTimer) {
            state = IndicatorState{};
            update_name();
            hide();
        }
        return 0;
    case WM_DPICHANGED:
        if (IsWindowVisible(hwnd)) render(false);
        return 0;
    case WM_DISPLAYCHANGE:
        if (IsWindowVisible(hwnd)) render(true);
        break;
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
        if (is_theme_change(message, wparam, lparam)) {
            theme = current_theme();
            if (IsWindowVisible(hwnd)) render(false);
        } else if (message == WM_SETTINGCHANGE && wparam == SPI_SETWORKAREA && IsWindowVisible(hwnd)) {
            render(true);
        }
        break;
    default: break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

IndicatorWindow::IndicatorWindow(IndicatorCallbacks callbacks) : impl_(std::make_unique<Impl>()) {
    impl_->callbacks = std::move(callbacks);
}

IndicatorWindow::~IndicatorWindow() { destroy(); }

bool IndicatorWindow::create(HINSTANCE instance) {
    if (impl_->hwnd) return true;
    if (!ensure_gdiplus()) return false;
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = &Impl::proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kClassName;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, kClassName,
                    indicator_accessible_name(impl_->state).c_str(), WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance,
                    impl_.get());
    return impl_->hwnd != nullptr;
}

void IndicatorWindow::destroy() {
    if (impl_ && impl_->hwnd) DestroyWindow(impl_->hwnd);
}

void IndicatorWindow::set_state(IndicatorState state) {
    Impl &impl = *impl_;
    state.level = std::clamp(state.level, 0.0, 1.0);
    const IndicatorMode previous = impl.state.mode;
    impl.state = std::move(state);
    if (!impl.hwnd) return;
    const auto buttons = indicator_buttons(impl.state);
    const auto present = [&](std::optional<IndicatorButton> button) {
        return button && std::find(buttons.begin(), buttons.end(), *button) != buttons.end();
    };
    if (!present(impl.pressed)) impl.pressed.reset();
    if (!present(impl.hovered)) impl.hovered.reset();
    impl.update_name();
    if (impl.state.mode == IndicatorMode::hidden) {
        impl.hide();
        return;
    }
    impl.render(true);
    impl.update_timers();
    if (is_message(impl.state.mode) && previous != impl.state.mode)
        NotifyWinEvent(EVENT_SYSTEM_ALERT, impl.hwnd, OBJID_WINDOW, CHILDID_SELF);
}

void IndicatorWindow::set_level(double level, std::chrono::milliseconds elapsed) {
    Impl &impl = *impl_;
    if (impl.state.mode != IndicatorMode::recording) return;
    impl.state.level = std::clamp(level, 0.0, 1.0);
    impl.state.elapsed = elapsed;
    if (!reduced_motion()) impl.tick += 0.35f;
    impl.render(false);
}

const IndicatorState &IndicatorWindow::state() const { return impl_->state; }

void IndicatorWindow::set_anchor(std::optional<POINT> anchor) {
    impl_->anchor = anchor;
    if (impl_->hwnd && IsWindowVisible(impl_->hwnd)) impl_->render(true);
}

POINT IndicatorWindow::anchor() const { return impl_->effective_anchor(); }

void IndicatorWindow::set_auto_hide_delay(std::chrono::milliseconds delay) { impl_->auto_hide = delay; }

HWND IndicatorWindow::hwnd() const { return impl_->hwnd; }

bool IndicatorWindow::visible() const { return impl_->hwnd && IsWindowVisible(impl_->hwnd); }

std::optional<RECT> IndicatorWindow::button_rect(IndicatorButton button) const {
    if (!visible()) return std::nullopt;
    RECT window{};
    GetWindowRect(impl_->hwnd, &window);
    for (const auto &entry : impl_->layout.buttons) {
        if (entry.id != button) continue;
        const auto &r = entry.rect;
        return RECT{window.left + static_cast<LONG>(std::ceil(r.X)), window.top + static_cast<LONG>(std::ceil(r.Y)),
                    window.left + static_cast<LONG>(r.X + r.Width), window.top + static_cast<LONG>(r.Y + r.Height)};
    }
    return std::nullopt;
}

unsigned IndicatorWindow::activation_count() const { return impl_->activations; }

} // namespace lemonvox::win::ui
