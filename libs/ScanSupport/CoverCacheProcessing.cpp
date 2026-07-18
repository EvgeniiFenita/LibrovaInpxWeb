#include "ScanSupport/CoverCacheProcessing.hpp"

#include <cstdint>
#include <stdexcept>
#include <string_view>

#include "Foundation/Logging.hpp"

namespace InpxWebReader::ScanSupport {
namespace {

constexpr std::uint32_t GCoverCacheMaxWidth = 456;
constexpr std::uint32_t GCoverCacheMaxHeight = 684;
constexpr std::uint32_t GCoverCacheFallbackMaxWidth = 400;
constexpr std::uint32_t GCoverCacheFallbackMaxHeight = 600;
constexpr std::uint32_t GCoverCacheTargetMaxBytes = 120u * 1024u;

[[nodiscard]] InpxWebReader::Domain::SCoverData BuildSourceCoverData(
    const InpxWebReader::Domain::SParsedBook& parsedBook)
{
    if (!parsedBook.CoverExtension.has_value()
        || parsedBook.CoverExtension->empty()
        || parsedBook.CoverBytes.empty())
    {
        throw std::invalid_argument("Parsed book cover data is incomplete.");
    }

    return {
        .Extension = parsedBook.CoverExtension.value(),
        .Bytes = parsedBook.CoverBytes
    };
}

[[nodiscard]] std::string_view CoverProcessingStatusLabel(
    const InpxWebReader::Domain::ECoverProcessingStatus status) noexcept
{
    switch (status)
    {
    case InpxWebReader::Domain::ECoverProcessingStatus::Unsupported: return "unsupported";
    case InpxWebReader::Domain::ECoverProcessingStatus::Failed: return "failed";
    case InpxWebReader::Domain::ECoverProcessingStatus::Processed: return "processed-no-output";
    case InpxWebReader::Domain::ECoverProcessingStatus::Unchanged: return "unchanged-no-output";
    }

    return "unknown";
}

} // namespace

std::string_view ToString(const ECoverCacheResolution resolution) noexcept
{
    switch (resolution)
    {
    case ECoverCacheResolution::StoredProcessed:
        return "stored-processed";
    case ECoverCacheResolution::StoredUnchanged:
        return "stored-unchanged";
    case ECoverCacheResolution::StoredOriginalWithoutProcessor:
        return "stored-original-no-processor";
    case ECoverCacheResolution::SkippedProcessorFailure:
        return "skipped-processor-failure";
    }

    return "unknown";
}

SResolvedCoverCacheData ResolveCoverCacheData(
    const InpxWebReader::Domain::SParsedBook& parsedBook,
    const InpxWebReader::Domain::ICoverImageProcessor* const coverImageProcessor,
    const std::string_view sourceLabel,
    const std::uint64_t maxDecodedBytes,
    const std::stop_token stopToken)
{
    const auto sourceCover = BuildSourceCoverData(parsedBook);

    if (coverImageProcessor == nullptr)
    {
        return {
            .Cover = sourceCover,
            .Resolution = ECoverCacheResolution::StoredOriginalWithoutProcessor
        };
    }

    const auto processingResult = coverImageProcessor->ProcessForCache({
        .Cover = sourceCover,
        .MaxWidth = GCoverCacheMaxWidth,
        .MaxHeight = GCoverCacheMaxHeight,
        .FallbackMaxWidth = GCoverCacheFallbackMaxWidth,
        .FallbackMaxHeight = GCoverCacheFallbackMaxHeight,
        .TargetMaxBytes = GCoverCacheTargetMaxBytes,
        .MaxDecodedBytes = maxDecodedBytes,
        .PreserveSmallerImages = true,
        .AllowFormatConversion = false,
        .StopToken = stopToken
    });

    switch (processingResult.Status)
    {
    case InpxWebReader::Domain::ECoverProcessingStatus::Processed:
    case InpxWebReader::Domain::ECoverProcessingStatus::Unchanged:
        if (processingResult.HasOutputCover())
        {
            InpxWebReader::Logging::DebugIfInitialized(
                "cover: stored source='{}' status='{}' ext='{}' dimensions={}x{} bytes={}",
                sourceLabel,
                processingResult.Status == InpxWebReader::Domain::ECoverProcessingStatus::Processed
                    ? "processed"
                    : "unchanged",
                processingResult.Cover.Extension,
                processingResult.PixelWidth,
                processingResult.PixelHeight,
                processingResult.Cover.Bytes.size());

            return {
                .Cover = processingResult.Cover,
                .Resolution = processingResult.Status == InpxWebReader::Domain::ECoverProcessingStatus::Processed
                    ? ECoverCacheResolution::StoredProcessed
                    : ECoverCacheResolution::StoredUnchanged
            };
        }

        break;
    case InpxWebReader::Domain::ECoverProcessingStatus::Unsupported:
    case InpxWebReader::Domain::ECoverProcessingStatus::Failed:
        break;
    }

    const std::string_view reason = processingResult.DiagnosticMessage.empty()
        ? std::string_view("(no diagnostic message from processor)")
        : std::string_view(processingResult.DiagnosticMessage);
    InpxWebReader::Logging::WarnIfInitialized(
        "cover: optimization skipped source='{}' status='{}' reason='{}' input_ext='{}' input_bytes={}",
        sourceLabel,
        CoverProcessingStatusLabel(processingResult.Status),
        reason,
        sourceCover.Extension,
        sourceCover.Bytes.size());

    return {
        .Cover = {},
        .Resolution = ECoverCacheResolution::SkippedProcessorFailure
    };
}

} // namespace InpxWebReader::ScanSupport
