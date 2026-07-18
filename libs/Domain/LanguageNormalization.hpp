#pragma once

#include <string>
#include <string_view>

namespace InpxWebReader::Domain {

[[nodiscard]] std::string NormalizeLanguage(std::string_view languageUtf8);

} // namespace InpxWebReader::Domain
