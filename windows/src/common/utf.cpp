#include "utf.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <climits>

namespace lemonvox::win {

std::wstring utf8_to_wide(std::string_view text, bool *ok) {
    if (ok) *ok = true;
    if (text.empty()) return {};
    if (text.size() > static_cast<size_t>(INT_MAX)) {
        if (ok) *ok = false;
        return {};
    }
    const int input = static_cast<int>(text.size());
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), input, nullptr, 0);
    if (size <= 0) {
        if (ok) *ok = false;
        return {};
    }
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), input, result.data(), size);
    return result;
}

std::string wide_to_utf8(std::wstring_view text, bool *ok) {
    if (ok) *ok = true;
    if (text.empty()) return {};
    if (text.size() > static_cast<size_t>(INT_MAX)) {
        if (ok) *ok = false;
        return {};
    }
    const int input = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), input, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        if (ok) *ok = false;
        return {};
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), input, result.data(), size, nullptr, nullptr);
    return result;
}

} // namespace lemonvox::win
