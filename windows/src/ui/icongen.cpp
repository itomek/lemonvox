// Build-time tool: writes the multi-resolution app icon consumed by lemonvox.rc.

#include "ui/icon.h"

#include <cstdio>

int wmain(int argc, wchar_t **argv) {
    if (argc != 2) {
        std::fwprintf(stderr, L"usage: lv_icongen <output.ico>\n");
        return 2;
    }
    const auto ico = lemonvox::win::ui::encode_app_icon({16, 20, 24, 32, 40, 48, 64, 256});
    if (ico.empty()) {
        std::fwprintf(stderr, L"failed to render the app icon\n");
        return 1;
    }
    FILE *file = nullptr;
    if (_wfopen_s(&file, argv[1], L"wb") != 0 || !file) {
        std::fwprintf(stderr, L"cannot write %ls\n", argv[1]);
        return 1;
    }
    const size_t written = std::fwrite(ico.data(), 1, ico.size(), file);
    std::fclose(file);
    return written == ico.size() ? 0 : 1;
}
