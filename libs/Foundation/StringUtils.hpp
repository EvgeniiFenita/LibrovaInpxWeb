#pragma once

#include <string>
#include <string_view>

namespace InpxWebReader::Foundation {

[[nodiscard]] std::string ToLowerAscii(std::string value);

[[nodiscard]] bool EndsWithAsciiInsensitive(std::string_view value, std::string_view suffix) noexcept;

} // namespace InpxWebReader::Foundation
