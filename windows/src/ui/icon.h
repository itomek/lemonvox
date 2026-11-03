#pragma once

// The LemonVox mark (lemon with a voice wave), drawn at runtime from the same
// vector geometry as macos/LemonIcon.swift so every DPI gets a crisp raster.

#include <windows.h>

#include <cstdint>
#include <vector>

namespace lemonvox::win::ui {

enum class IconBadge : std::uint8_t { none, recording, processing, error };

// Returns a 32bpp alpha icon of size_px square, or nullptr. Caller owns it
// (DestroyIcon). `tile` draws the dark rounded app-tile background.
HICON create_lemon_icon(int size_px, IconBadge badge = IconBadge::none, bool tile = false);

// Tray icon size for a DPI (SM_CXSMICON scaled: 16, 20, 24, 32 ...).
int tray_icon_size_for_dpi(UINT dpi);

// A complete .ico file image (PNG-compressed entries) of the app tile.
std::vector<std::uint8_t> encode_app_icon(const std::vector<int> &sizes);

} // namespace lemonvox::win::ui
