#pragma once

#include <string_view>

namespace InpxWebReader::Domain {

enum class EInpxBookAvailability
{
    Available,
    MissingFromIndex
};

[[nodiscard]] constexpr std::string_view ToString(const EInpxBookAvailability availability) noexcept
{
    switch (availability)
    {
    case EInpxBookAvailability::Available:
        return "available";
    case EInpxBookAvailability::MissingFromIndex:
        return "missing_from_index";
    }

    return "unknown";
}

[[nodiscard]] constexpr bool IsInpxBookAvailable(const std::string_view availability) noexcept
{
    return availability == ToString(EInpxBookAvailability::Available);
}

} // namespace InpxWebReader::Domain
