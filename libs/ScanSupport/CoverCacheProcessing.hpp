#pragma once

#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>

#include "Domain/ServiceContracts.hpp"

namespace InpxWebReader::ScanSupport {

enum class ECoverCacheResolution
{
    StoredProcessed,
    StoredUnchanged,
    StoredOriginalWithoutProcessor,
    SkippedProcessorFailure
};

[[nodiscard]] std::string_view ToString(ECoverCacheResolution resolution) noexcept;

struct SResolvedCoverCacheData
{
    InpxWebReader::Domain::SCoverData Cover;
    ECoverCacheResolution Resolution = ECoverCacheResolution::StoredOriginalWithoutProcessor;
};

[[nodiscard]] SResolvedCoverCacheData ResolveCoverCacheData(
    const InpxWebReader::Domain::SParsedBook& parsedBook,
    const InpxWebReader::Domain::ICoverImageProcessor* coverImageProcessor,
    std::string_view sourceLabel,
    std::uint64_t maxDecodedBytes = 64ull * 1024ull * 1024ull,
    std::stop_token stopToken = {});

} // namespace InpxWebReader::ScanSupport
