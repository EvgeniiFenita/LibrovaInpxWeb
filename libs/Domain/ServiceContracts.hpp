#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "Domain/Book.hpp"

namespace InpxWebReader::Domain {

struct SParsedBook
{
    SBookMetadata Metadata;
    std::optional<std::string> CoverExtension = std::nullopt;
    std::vector<std::byte> CoverBytes;
    std::optional<std::string> CoverDiagnosticMessage = std::nullopt;
    std::size_t ParserWarningCount = 0;

    [[nodiscard]] bool HasCover() const noexcept
    {
        return CoverExtension.has_value() && !CoverExtension->empty() && !CoverBytes.empty();
    }
};

struct SBookParseOptions
{
    bool ExtractCover = true;
    std::optional<std::string> MissingTitleFallbackUtf8 = std::nullopt;
    bool MissingLanguageMayBeRecoveredByCaller = false;
    std::stop_token StopToken;
};

struct SConversionRequest
{
    std::filesystem::path SourcePath;
    std::filesystem::path DestinationPath;
    EBookFormat SourceFormat = EBookFormat::Fb2;
    EBookFormat DestinationFormat = EBookFormat::Epub;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return !SourcePath.empty() && !DestinationPath.empty() && SourceFormat != DestinationFormat;
    }
};

enum class EConversionStatus
{
    Failed,
    Succeeded,
    Cancelled,
    TimedOut
};

struct SConversionResult
{
    EConversionStatus Status = EConversionStatus::Failed;
    std::filesystem::path OutputPath;
    std::vector<std::string> Warnings;

    [[nodiscard]] bool IsSuccess() const noexcept
    {
        return Status == EConversionStatus::Succeeded;
    }

    [[nodiscard]] bool IsCancelled() const noexcept
    {
        return Status == EConversionStatus::Cancelled;
    }

    [[nodiscard]] bool IsTimedOut() const noexcept
    {
        return Status == EConversionStatus::TimedOut;
    }

    [[nodiscard]] bool HasOutput() const noexcept
    {
        return !OutputPath.empty();
    }
};

struct SCoverData
{
    std::string Extension;
    std::vector<std::byte> Bytes;

    [[nodiscard]] bool IsEmpty() const noexcept
    {
        return Extension.empty() || Bytes.empty();
    }
};

enum class ECoverProcessingStatus
{
    Processed,
    Unchanged,
    Unsupported,
    Failed
};

struct SCoverProcessingRequest
{
    SCoverData Cover;
    std::uint32_t MaxWidth = 0;
    std::uint32_t MaxHeight = 0;
    std::uint32_t FallbackMaxWidth = 0;
    std::uint32_t FallbackMaxHeight = 0;
    std::uint32_t TargetMaxBytes = 0;
    std::uint64_t MaxDecodedBytes = 64ull * 1024ull * 1024ull;
    bool PreserveSmallerImages = true;
    bool AllowFormatConversion = false;
    std::stop_token StopToken;

    [[nodiscard]] bool IsValid() const noexcept
    {
        const bool hasValidPrimaryBounds = MaxWidth > 0 && MaxHeight > 0;
        const bool hasNoFallbackBounds = FallbackMaxWidth == 0 && FallbackMaxHeight == 0;
        const bool hasValidFallbackBounds = FallbackMaxWidth > 0 && FallbackMaxHeight > 0;
        return !Cover.IsEmpty() && hasValidPrimaryBounds && (hasNoFallbackBounds || hasValidFallbackBounds);
    }
};

struct SCoverProcessingResult
{
    ECoverProcessingStatus Status = ECoverProcessingStatus::Failed;
    SCoverData Cover;
    std::uint32_t PixelWidth = 0;
    std::uint32_t PixelHeight = 0;
    bool WasResized = false;
    std::string DiagnosticMessage;

    [[nodiscard]] bool HasOutputCover() const noexcept
    {
        return !Cover.IsEmpty();
    }
};

class IProgressSink
{
public:
    virtual ~IProgressSink() = default;

    virtual void ReportValue(int percent, std::string_view message) = 0;
    [[nodiscard]] virtual bool IsCancellationRequested() const = 0;
};

class IBookConverter
{
public:
    virtual ~IBookConverter() = default;

    [[nodiscard]] virtual bool CanConvert(EBookFormat sourceFormat, EBookFormat destinationFormat) const = 0;
    [[nodiscard]] virtual SConversionResult Convert(
        const SConversionRequest& request,
        IProgressSink& progressSink,
        std::stop_token stopToken) const = 0;
};

class ICoverImageProcessor
{
public:
    virtual ~ICoverImageProcessor() = default;

    [[nodiscard]] virtual SCoverProcessingResult ProcessForCache(
        const SCoverProcessingRequest& request) const = 0;
};

} // namespace InpxWebReader::Domain
