#pragma once

#include <string>
#include <string_view>

namespace lemonvox::win {

// Strict conversions: invalid input yields an empty string and ok=false.
std::wstring utf8_to_wide(std::string_view text, bool *ok = nullptr);
std::string wide_to_utf8(std::wstring_view text, bool *ok = nullptr);

} // namespace lemonvox::win
