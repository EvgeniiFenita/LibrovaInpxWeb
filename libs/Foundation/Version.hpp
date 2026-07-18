#pragma once

#include <string_view>

namespace InpxWebReader::Core {

class CVersion
{
public:
    [[nodiscard]] static std::string_view GetValue() noexcept;
};

} // namespace InpxWebReader::Core
