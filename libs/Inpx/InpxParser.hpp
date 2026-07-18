#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "Inpx/InpxRecord.hpp"

namespace InpxWebReader::Inpx {

constexpr std::uint64_t GMaxInpEntryPayloadBytes = 128ull * 1024ull * 1024ull;
constexpr std::size_t GMaxInpRecordLineBytes = 1ull * 1024ull * 1024ull;

[[nodiscard]] constexpr bool IsInpEntryPayloadSizeAllowed(const std::uint64_t sizeBytes) noexcept
{
    return sizeBytes <= GMaxInpEntryPayloadBytes;
}

// INPX format reference used by InpxWebReader's INPX catalog support:
// - an .inpx file is a ZIP archive containing one or more .inp text entries;
// - each non-empty .inp line is one record with 15 fields separated by byte 0x04;
// - fields are Authors, Genres, Title, Series, SeriesNumber, FileName, FileSize,
//   LibId, Deleted, FileExt, DateAdded, Language, IgnoredField12, Keywords, Reserved;
// - Authors are colon-separated, with each token stored as Last,First,Middle;
// - Genres and Keywords are colon-separated;
// - the .inp entry basename maps to the payload archive name
//   (for example fb2-123.inp -> fb2-123, while fb2-123.zip.inp -> fb2-123.zip);
// - the payload entry name is FileName + "." + FileExt; empty FileExt defaults to fb2;
// - Deleted != 0 marks a source deletion and is not indexed as an active book.
class CInpxParser
{
public:
    using FRecordCallback = std::function<void(SInpxRecord&&)>;
    using FWarningCallback = std::function<void(const SInpxWarning&)>;
    using FSegmentCallback = std::function<void(SInpxSegmentPayload&&)>;
    using FCheckpointCallback = std::function<void()>;

    [[nodiscard]] SInpxParseResult ParseAll(const std::filesystem::path& inpxPath) const;
    [[nodiscard]] SInpxParseSummary Parse(
        const std::filesystem::path& inpxPath,
        FRecordCallback onRecord,
        const FWarningCallback& onWarning = {}) const;
    [[nodiscard]] std::size_t ReadSegments(
        const std::filesystem::path& inpxPath,
        const FSegmentCallback& onSegment,
        const FCheckpointCallback& checkpoint = {},
        std::uint64_t maxTotalInputBytes = (std::numeric_limits<std::uint64_t>::max)()) const;
    [[nodiscard]] SInpxParseSummary ParseSegment(
        const SInpxSegmentPayload& segment,
        FRecordCallback onRecord,
        const FWarningCallback& onWarning = {},
        const FCheckpointCallback& checkpoint = {}) const;
    [[nodiscard]] std::vector<std::string> ReadArchiveNameSamples(
        const std::filesystem::path& inpxPath,
        std::size_t maxSamples) const;

};

} // namespace InpxWebReader::Inpx
