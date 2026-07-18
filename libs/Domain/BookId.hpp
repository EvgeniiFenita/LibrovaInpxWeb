#pragma once

#include <cstdint>

namespace InpxWebReader::Domain {

struct SBookId
{
    std::int64_t Value = 0;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return Value > 0;
    }
};

} // namespace InpxWebReader::Domain
