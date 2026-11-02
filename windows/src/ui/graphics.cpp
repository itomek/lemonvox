#include "ui/graphics.h"

#include <mutex>

namespace lemonvox::win::ui {

bool ensure_gdiplus() {
    static std::once_flag once;
    static bool started = false;
    std::call_once(once, [] {
        ULONG_PTR token = 0;
        Gdiplus::GdiplusStartupInput input;
        started = Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    });
    return started;
}

void add_round_rect(Gdiplus::GraphicsPath &path, const Gdiplus::RectF &rect, float radius) {
    const float diameter = std::min({radius * 2.0f, rect.Width, rect.Height});
    if (diameter <= 0.0f) {
        path.AddRectangle(rect);
        return;
    }
    const float right = rect.X + rect.Width - diameter;
    const float bottom = rect.Y + rect.Height - diameter;
    path.StartFigure();
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(right, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(right, bottom, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, bottom, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

Canvas::Canvas(int width, int height, Gdiplus::PixelFormat format) {
    if (width <= 0 || height <= 0 || !ensure_gdiplus()) return;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    dib_ = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib_) return;
    bitmap_ = new Gdiplus::Bitmap(width, height, width * 4, format, static_cast<BYTE *>(bits));
    if (bitmap_->GetLastStatus() != Gdiplus::Ok) return;
    graphics_ = new Gdiplus::Graphics(bitmap_);
    if (graphics_->GetLastStatus() != Gdiplus::Ok) {
        delete graphics_;
        graphics_ = nullptr;
        return;
    }
    graphics_->Clear(Gdiplus::Color(0, 0, 0, 0));
    graphics_->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics_->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics_->SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
}

Canvas::~Canvas() {
    finish();
    if (dib_) DeleteObject(dib_);
}

HBITMAP Canvas::finish() {
    if (graphics_) graphics_->Flush(Gdiplus::FlushIntentionSync);
    delete graphics_;
    graphics_ = nullptr;
    delete bitmap_;
    bitmap_ = nullptr;
    GdiFlush();
    return dib_;
}

} // namespace lemonvox::win::ui
