#include "ui/icon.h"

#include "ui/graphics.h"

#include <cstring>

namespace lemonvox::win::ui {
namespace {

using Gdiplus::Color;
using Gdiplus::PointF;

const Color kLeaf(255, 33, 64, 48);
const Color kLemon(255, 252, 212, 59);
const Color kRind(255, 150, 112, 0);

void draw_lemon(Gdiplus::Graphics &graphics, float size, IconBadge badge, bool tile) {
    const float scale = size / 100.0f;
    // macOS geometry is y-up in a 100x100 box.
    const auto point = [scale](float x, float y) { return PointF(x * scale, (100.0f - y) * scale); };

    if (tile) {
        Gdiplus::GraphicsPath background;
        add_round_rect(background, Gdiplus::RectF(2 * scale, 2 * scale, 96 * scale, 96 * scale), 22 * scale);
        Gdiplus::SolidBrush fill(kLeaf);
        graphics.FillPath(&fill, &background);
    }

    Gdiplus::GraphicsPath lemon;
    lemon.StartFigure();
    lemon.AddBezier(point(15, 42), point(20, 75), point(48, 90), point(77, 76));
    lemon.AddLine(point(77, 76), point(86, 79));
    lemon.AddLine(point(86, 79), point(83, 68));
    lemon.AddBezier(point(83, 68), point(89, 34), point(51, 9), point(23, 25));
    lemon.AddLine(point(23, 25), point(13, 22));
    lemon.AddLine(point(13, 22), point(15, 42));
    lemon.CloseFigure();
    Gdiplus::SolidBrush lemon_fill(kLemon);
    graphics.FillPath(&lemon_fill, &lemon);
    if (!tile) {
        // A thin rind keeps the yellow readable on light taskbars.
        Gdiplus::Pen rind(kRind, std::max(1.0f, size / 28.0f));
        graphics.DrawPath(&rind, &lemon);
    }

    const PointF wave[] = {point(30, 48), point(38, 48), point(43, 60), point(50, 35),
                           point(57, 65), point(63, 48), point(72, 48)};
    Gdiplus::Pen stroke(kLeaf, std::max(1.25f, 5.5f * scale));
    stroke.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
    stroke.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.DrawLines(&stroke, wave, 7);

    if (badge == IconBadge::none) return;
    Color dot;
    switch (badge) {
    case IconBadge::recording: dot = Color(255, 232, 17, 35); break;
    case IconBadge::processing: dot = Color(255, 0, 120, 212); break;
    default: dot = Color(255, 247, 99, 12); break;
    }
    const float diameter = size * 0.46f;
    const float outline = std::max(1.0f, size / 16.0f);
    const float x = size - diameter - outline * 0.5f;
    const float y = size - diameter - outline * 0.5f;
    Gdiplus::SolidBrush dot_fill(dot);
    Gdiplus::Pen ring(Color(255, 255, 255, 255), outline);
    graphics.FillEllipse(&dot_fill, x, y, diameter, diameter);
    graphics.DrawEllipse(&ring, x, y, diameter, diameter);
}

bool png_encoder(CLSID *clsid) {
    UINT count = 0, bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0) return false;
    std::vector<std::uint8_t> buffer(bytes);
    auto *codecs = reinterpret_cast<Gdiplus::ImageCodecInfo *>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok) return false;
    for (UINT index = 0; index < count; ++index) {
        if (std::wcscmp(codecs[index].MimeType, L"image/png") == 0) {
            *clsid = codecs[index].Clsid;
            return true;
        }
    }
    return false;
}

std::vector<std::uint8_t> encode_png(int size) {
    Gdiplus::Bitmap bitmap(size, size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&bitmap);
        graphics.Clear(Color(0, 0, 0, 0));
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        draw_lemon(graphics, static_cast<float>(size), IconBadge::none, true);
    }
    CLSID clsid{};
    IStream *stream = nullptr;
    std::vector<std::uint8_t> png;
    if (!png_encoder(&clsid) || FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return png;
    if (bitmap.Save(stream, &clsid, nullptr) == Gdiplus::Ok) {
        STATSTG stat{};
        LARGE_INTEGER origin{};
        if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.HighPart == 0 &&
            SUCCEEDED(stream->Seek(origin, STREAM_SEEK_SET, nullptr))) {
            png.resize(stat.cbSize.LowPart);
            ULONG read = 0;
            if (FAILED(stream->Read(png.data(), stat.cbSize.LowPart, &read)) || read != stat.cbSize.LowPart)
                png.clear();
        }
    }
    stream->Release();
    return png;
}

void put16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
}

} // namespace

HICON create_lemon_icon(int size_px, IconBadge badge, bool tile) {
    if (size_px <= 0 || size_px > 1024) return nullptr;
    Canvas canvas(size_px, size_px, PixelFormat32bppARGB);
    if (!canvas.ok()) return nullptr;
    draw_lemon(canvas.graphics(), static_cast<float>(size_px), badge, tile);
    HBITMAP color = canvas.finish();
    const int mask_stride = ((size_px + 15) / 16) * 2;
    std::vector<std::uint8_t> mask_bits(static_cast<size_t>(mask_stride) * static_cast<size_t>(size_px), 0);
    HBITMAP mask = CreateBitmap(size_px, size_px, 1, 1, mask_bits.data());
    if (!mask) return nullptr;
    ICONINFO info{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    return icon;
}

int tray_icon_size_for_dpi(UINT dpi) { return GetSystemMetricsForDpi(SM_CXSMICON, dpi == 0 ? 96 : dpi); }

std::vector<std::uint8_t> encode_app_icon(const std::vector<int> &sizes) {
    std::vector<std::uint8_t> file;
    if (sizes.empty() || sizes.size() > 32 || !ensure_gdiplus()) return file;
    std::vector<std::vector<std::uint8_t>> images;
    for (const int size : sizes) {
        if (size <= 0 || size > 256) return file;
        images.push_back(encode_png(size));
        if (images.back().empty()) return file;
    }
    put16(file, 0);
    put16(file, 1);
    put16(file, static_cast<std::uint16_t>(images.size()));
    std::uint32_t offset = static_cast<std::uint32_t>(6 + 16 * images.size());
    for (size_t index = 0; index < images.size(); ++index) {
        const int size = sizes[index];
        file.push_back(static_cast<std::uint8_t>(size >= 256 ? 0 : size));
        file.push_back(static_cast<std::uint8_t>(size >= 256 ? 0 : size));
        file.push_back(0);
        file.push_back(0);
        put16(file, 1);
        put16(file, 32);
        put32(file, static_cast<std::uint32_t>(images[index].size()));
        put32(file, offset);
        offset += static_cast<std::uint32_t>(images[index].size());
    }
    for (const auto &image : images) file.insert(file.end(), image.begin(), image.end());
    return file;
}

} // namespace lemonvox::win::ui
