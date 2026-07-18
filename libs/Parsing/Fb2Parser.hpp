#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "Domain/ServiceContracts.hpp"

namespace InpxWebReader::Fb2Parsing {

constexpr std::uint64_t GMaxFb2MetadataXmlBytes = 8ull * 1024ull * 1024ull;
constexpr std::uint64_t GMaxFb2MetadataXmlNodes = 100'000;
constexpr std::uint64_t GMaxFb2MetadataXmlAttributes = 200'000;

class CFb2Parser final
{
public:
    [[nodiscard]] InpxWebReader::Domain::SParsedBook ParseBytes(
        std::string rawBytes,
        std::string_view logicalSourceLabel = {},
        const InpxWebReader::Domain::SBookParseOptions& options = {}) const;
};

} // namespace InpxWebReader::Fb2Parsing
