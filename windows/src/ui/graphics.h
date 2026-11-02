#pragma once

// Private to lv_ui: GDI+ include shim plus shared drawing helpers.

#include <windows.h>

#include <objidl.h>

#include <algorithm>

namespace Gdiplus {
using std::max;
using std::min;
} // namespace Gdiplus

#include <gdiplus.h>

namespace lemonvox::win::ui {

// Starts GDI+ once per process. It is intentionally never shut down because
// GDI+ objects may still be alive in statics during teardown.
bool ensure_gdiplus();

void add_round_rect(Gdiplus::GraphicsPath &path, const Gdiplus::RectF &rect, float radius);

inline Gdiplus::Color gdip_color(COLORREF color, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

// A 32bpp top-down DIB section wrapped by a GDI+ bitmap in `format`.
class Canvas {
public:
    Canvas(int width, int height, Gdiplus::PixelFormat format);
    ~Canvas();
    Canvas(const Canvas &) = delete;
    Canvas &operator=(const Canvas &) = delete;

    bool ok() const { return bitmap_ != nullptr && graphics_ != nullptr; }
    Gdiplus::Graphics &graphics() { return *graphics_; }
    // Ends drawing; afterwards only dib() remains valid.
    HBITMAP finish();
    HBITMAP dib() const { return dib_; }

private:
    HBITMAP dib_ = nullptr;
    Gdiplus::Bitmap *bitmap_ = nullptr;
    Gdiplus::Graphics *graphics_ = nullptr;
};

} // namespace lemonvox::win::ui
