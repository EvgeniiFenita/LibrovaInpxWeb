#pragma once

#include <cstdint>

namespace InpxWebReader::Foundation {

constexpr std::uint64_t GMaxBookPayloadBytes = 250ull * 1024ull * 1024ull;

[[nodiscard]] constexpr bool IsBookPayloadSizeAllowed(const std::uint64_t sizeBytes) noexcept
{
    return sizeBytes <= GMaxBookPayloadBytes;
}

} // namespace InpxWebReader::Foundation
