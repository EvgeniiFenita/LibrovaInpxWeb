#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace InpxWebReader::Domain {

[[nodiscard]] std::string NormalizeText(std::string_view value);
[[nodiscard]] std::optional<std::string> NormalizeIsbn(const std::optional<std::string>& value);

} // namespace InpxWebReader::Domain
