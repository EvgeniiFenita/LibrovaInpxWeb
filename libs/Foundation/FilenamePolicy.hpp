#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace InpxWebReader::Foundation {

[[nodiscard]] std::string SanitizeDownloadFilenameBaseUtf8(
    std::string valueUtf8,
    std::size_t maxBaseNameBytes,
    std::string_view fallbackUtf8 = "book");

} // namespace InpxWebReader::Foundation
