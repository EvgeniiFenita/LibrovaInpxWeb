#include "Inpx/InpxParser.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zip.h>

#include "Foundation/StringUtils.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "Foundation/Sha256Fingerprint.hpp"

namespace InpxWebReader::Inpx {
namespace {

constexpr char GFieldSeparator = '\x04';
constexpr std::size_t GExpectedFieldCount = 15;
constexpr std::size_t GCancellationCheckpointIntervalBytes = 64ull * 1024ull;
constexpr std::size_t GMaxAuthorsPerRecord = 256;
constexpr std::size_t GMaxAuthorNameParts = 3;
constexpr std::size_t GMaxGenresPerRecord = 512;
constexpr std::size_t GMaxKeywordsPerRecord = 512;
constexpr std::size_t GMaxIntegerFieldBytes = 32;
constexpr std::size_t GMaxFileExtensionBytes = 32;

void RunCheckpoint(const CInpxParser::FCheckpointCallback& checkpoint)
{
    if (checkpoint)
    {
        checkpoint();
    }
}

class CZipFileHandle final
{
public:
    explicit CZipFileHandle(zip_file_t* const file)
        : m_file(file)
    {
        if (m_file == nullptr)
        {
            throw std::runtime_error("Failed to open INPX entry.");
        }
    }

    ~CZipFileHandle()
    {
        if (m_file != nullptr)
        {
            zip_fclose(m_file);
        }
    }

    CZipFileHandle(const CZipFileHandle&) = delete;
    CZipFileHandle& operator=(const CZipFileHandle&) = delete;

    [[nodiscard]] zip_file_t* Get() const noexcept
    {
        return m_file;
    }

    void Close()
    {
        if (m_file == nullptr)
        {
            return;
        }

        zip_file_t* const file = std::exchange(m_file, nullptr);
        if (zip_fclose(file) != 0)
        {
            throw std::runtime_error("Failed to close INPX entry after reading.");
        }
    }

private:
    zip_file_t* m_file = nullptr;
};

class CZipArchive final
{
public:
    explicit CZipArchive(const std::filesystem::path& archivePath)
    {
        int errorCode = ZIP_ER_OK;
        const std::string utf8Path = Unicode::PathToUtf8(archivePath);
        m_archive = zip_open(utf8Path.c_str(), ZIP_RDONLY, &errorCode);
        if (m_archive == nullptr)
        {
            zip_error_t errorState;
            zip_error_init_with_code(&errorState, errorCode);
            const std::string message = zip_error_strerror(&errorState);
            zip_error_fini(&errorState);
            throw std::runtime_error("Failed to open INPX archive: " + message);
        }
    }

    ~CZipArchive()
    {
        if (m_archive != nullptr)
        {
            zip_close(m_archive);
        }
    }

    CZipArchive(const CZipArchive&) = delete;
    CZipArchive& operator=(const CZipArchive&) = delete;
    CZipArchive(CZipArchive&&) = delete;
    CZipArchive& operator=(CZipArchive&&) = delete;

    [[nodiscard]] zip_uint64_t EntryCount() const
    {
        const zip_int64_t count = zip_get_num_entries(m_archive, 0);
        if (count < 0)
        {
            throw std::runtime_error("Failed to read INPX entry count.");
        }
        return static_cast<zip_uint64_t>(count);
    }

    [[nodiscard]] std::string EntryName(const zip_uint64_t index) const
    {
        zip_stat_t stat = {};
        if (zip_stat_index(m_archive, index, 0, &stat) != 0 || stat.name == nullptr)
        {
            throw std::runtime_error("Failed to read INPX entry name.");
        }

        return stat.name;
    }

    [[nodiscard]] zip_uint64_t EntrySize(const zip_uint64_t index) const
    {
        zip_stat_t stat = {};
        if (zip_stat_index(m_archive, index, 0, &stat) != 0)
        {
            throw std::runtime_error("Failed to stat INPX entry.");
        }

        if (!IsInpEntryPayloadSizeAllowed(stat.size))
        {
            throw std::runtime_error("INP entry too large.");
        }
        return stat.size;
    }

    [[nodiscard]] std::string ReadTextEntry(
        const zip_uint64_t index,
        const zip_uint64_t entrySize,
        const CInpxParser::FCheckpointCallback& checkpoint) const
    {
        CZipFileHandle file(zip_fopen_index(m_archive, index, 0));
        std::string bytes;
        bytes.reserve(static_cast<std::size_t>(entrySize));
        std::array<char, GCancellationCheckpointIntervalBytes> buffer{};
        zip_uint64_t remainingBytes = entrySize;
        while (remainingBytes > 0)
        {
            RunCheckpoint(checkpoint);
            const auto bytesToRead = static_cast<zip_uint64_t>((std::min)(
                buffer.size(),
                static_cast<std::size_t>(remainingBytes)));
            const zip_int64_t readCount = zip_fread(
                file.Get(),
                buffer.data(),
                bytesToRead);
            if (readCount <= 0 || static_cast<zip_uint64_t>(readCount) > bytesToRead)
            {
                throw std::runtime_error("Failed to read INPX entry.");
            }
            bytes.append(buffer.data(), static_cast<std::size_t>(readCount));
            remainingBytes -= static_cast<zip_uint64_t>(readCount);
        }

        RunCheckpoint(checkpoint);
        char trailingByte = '\0';
        if (zip_fread(file.Get(), &trailingByte, 1) != 0)
        {
            throw std::runtime_error("Failed to validate INPX entry payload checksum and size.");
        }
        file.Close();

        return bytes;
    }

private:
    zip_t* m_archive = nullptr;
};

[[nodiscard]] std::string CopyTextWithCheckpoints(
    const std::string_view value,
    const CInpxParser::FCheckpointCallback& checkpoint)
{
    RunCheckpoint(checkpoint);
    if (value.size() <= GCancellationCheckpointIntervalBytes)
    {
        return std::string{value};
    }

    std::string result;
    result.reserve(value.size());
    std::size_t offset = 0;
    while (offset < value.size())
    {
        if (offset != 0)
        {
            RunCheckpoint(checkpoint);
        }
        const std::size_t chunkSize = (std::min)(
            GCancellationCheckpointIntervalBytes,
            value.size() - offset);
        result.append(value.data() + offset, chunkSize);
        offset += chunkSize;
    }
    return result;
}

void AppendTextWithCheckpoints(
    std::string& destination,
    const std::string_view value,
    const CInpxParser::FCheckpointCallback& checkpoint)
{
    RunCheckpoint(checkpoint);
    if (value.size() <= GCancellationCheckpointIntervalBytes)
    {
        destination.append(value);
        return;
    }

    std::size_t offset = 0;
    while (offset < value.size())
    {
        if (offset != 0)
        {
            RunCheckpoint(checkpoint);
        }
        const std::size_t chunkSize = (std::min)(
            GCancellationCheckpointIntervalBytes,
            value.size() - offset);
        destination.append(value.data() + offset, chunkSize);
        offset += chunkSize;
    }
}

[[nodiscard]] std::string ComputeFingerprintWithCheckpoints(
    const std::string_view bytes,
    const CInpxParser::FCheckpointCallback& checkpoint)
{
    Foundation::CSha256FingerprintBuilder fingerprint;
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        RunCheckpoint(checkpoint);
        const std::size_t chunkSize = (std::min)(
            GCancellationCheckpointIntervalBytes,
            bytes.size() - offset);
        fingerprint.Update(bytes.substr(offset, chunkSize));
        offset += chunkSize;
    }
    RunCheckpoint(checkpoint);
    return fingerprint.Finalize();
}

[[nodiscard]] std::size_t FindByteWithCheckpoints(
    const std::string_view value,
    const char expected,
    const std::size_t start,
    const CInpxParser::FCheckpointCallback& checkpoint)
{
    RunCheckpoint(checkpoint);
    if (start >= value.size())
    {
        return std::string_view::npos;
    }
    if (value.size() - start <= GCancellationCheckpointIntervalBytes)
    {
        const std::size_t relative = value.substr(start).find(expected);
        return relative == std::string_view::npos
            ? std::string_view::npos
            : start + relative;
    }

    std::size_t offset = start;
    while (offset < value.size())
    {
        if (offset != start)
        {
            RunCheckpoint(checkpoint);
        }
        const std::size_t chunkSize = (std::min)(
            GCancellationCheckpointIntervalBytes,
            value.size() - offset);
        const std::size_t relative = value.substr(offset, chunkSize).find(expected);
        if (relative != std::string_view::npos)
        {
            return offset + relative;
        }
        offset += chunkSize;
    }
    return std::string_view::npos;
}

[[nodiscard]] std::string TrimAsciiWhitespace(
    const std::string_view value,
    const CInpxParser::FCheckpointCallback& checkpoint = {})
{
    RunCheckpoint(checkpoint);
    if (value.size() <= GCancellationCheckpointIntervalBytes)
    {
        std::size_t start = 0;
        std::size_t end = value.size();
        while (start < end && static_cast<unsigned char>(value[start]) <= 0x20)
        {
            ++start;
        }
        while (end > start && static_cast<unsigned char>(value[end - 1]) <= 0x20)
        {
            --end;
        }
        return std::string{value.substr(start, end - start)};
    }

    std::size_t start = 0;
    std::size_t end = value.size();
    std::size_t bytesSinceCheckpoint = 0;
    while (start < end && static_cast<unsigned char>(value[start]) <= 0x20)
    {
        ++start;
        if (++bytesSinceCheckpoint == GCancellationCheckpointIntervalBytes)
        {
            RunCheckpoint(checkpoint);
            bytesSinceCheckpoint = 0;
        }
    }
    while (end > start && static_cast<unsigned char>(value[end - 1]) <= 0x20)
    {
        --end;
        if (++bytesSinceCheckpoint == GCancellationCheckpointIntervalBytes)
        {
            RunCheckpoint(checkpoint);
            bytesSinceCheckpoint = 0;
        }
    }
    return CopyTextWithCheckpoints(value.substr(start, end - start), checkpoint);
}

struct SSplitViewsResult
{
    std::vector<std::string_view> Parts;
    bool ExceededLimit = false;
};

[[nodiscard]] SSplitViewsResult SplitViews(
    const std::string_view value,
    const char separator,
    const std::size_t maxParts,
    const CInpxParser::FCheckpointCallback& checkpoint = {})
{
    SSplitViewsResult result;
    result.Parts.reserve((std::min)(maxParts, GExpectedFieldCount));
    std::size_t start = 0;
    while (start <= value.size())
    {
        if (result.Parts.size() == maxParts)
        {
            result.ExceededLimit = true;
            return result;
        }

        const std::size_t end = FindByteWithCheckpoints(value, separator, start, checkpoint);
        if (end == std::string_view::npos)
        {
            result.Parts.emplace_back(value.substr(start));
            break;
        }

        result.Parts.emplace_back(value.substr(start, end - start));
        start = end + 1;
    }
    return result;
}

struct SSplitUtf8Result
{
    std::vector<std::string> Parts;
    bool ExceededLimit = false;
};

[[nodiscard]] SSplitUtf8Result SplitNonEmptyUtf8(
    const std::string_view value,
    const char separator,
    const std::size_t maxParts,
    const CInpxParser::FCheckpointCallback& checkpoint = {})
{
    const auto split = SplitViews(value, separator, maxParts, checkpoint);
    if (split.ExceededLimit)
    {
        return {.ExceededLimit = true};
    }

    SSplitUtf8Result result;
    result.Parts.reserve(split.Parts.size());
    for (const auto part : split.Parts)
    {
        std::string trimmed = TrimAsciiWhitespace(part, checkpoint);
        if (!trimmed.empty())
        {
            result.Parts.push_back(std::move(trimmed));
        }
    }
    return result;
}

template <typename T>
[[nodiscard]] std::optional<T> TryParseInteger(const std::string_view value)
{
    if (value.empty() || value.size() > GMaxIntegerFieldBytes)
    {
        return std::nullopt;
    }

    T parsed = {};
    const auto [ptr, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || ptr != value.data() + value.size())
    {
        return std::nullopt;
    }

    return parsed;
}

void EmitWarning(
    SInpxParseSummary& summary,
    const CInpxParser::FWarningCallback& onWarning,
    const std::string_view inpEntryNameUtf8,
    const std::size_t lineNumber,
    const std::string& messageUtf8,
    const bool recordSkipped = false)
{
    ++summary.WarningCount;
    if (onWarning)
    {
        onWarning({
            .MessageUtf8 = messageUtf8,
            .InpEntryNameUtf8 = std::string{inpEntryNameUtf8},
            .LineNumber = lineNumber,
            .RecordSkipped = recordSkipped
        });
    }
}

[[nodiscard]] std::string DecodeInpPayloadToUtf8(
    const std::string_view bytes,
    const CInpxParser::FCheckpointCallback& checkpoint)
{
    if (bytes.size() >= 3
        && static_cast<unsigned char>(bytes[0]) == 0xEF
        && static_cast<unsigned char>(bytes[1]) == 0xBB
        && static_cast<unsigned char>(bytes[2]) == 0xBF)
    {
        return CopyTextWithCheckpoints(bytes.substr(3), checkpoint);
    }

    if (bytes.size() >= 2
        && static_cast<unsigned char>(bytes[0]) == 0xFF
        && static_cast<unsigned char>(bytes[1]) == 0xFE)
    {
        return Unicode::Utf16LeToUtf8(bytes.data(), bytes.size(), checkpoint);
    }

    if (bytes.size() >= 2
        && static_cast<unsigned char>(bytes[0]) == 0xFE
        && static_cast<unsigned char>(bytes[1]) == 0xFF)
    {
        return Unicode::Utf16BeToUtf8(bytes.data(), bytes.size(), checkpoint);
    }

    if (Unicode::IsValidUtf8(bytes, checkpoint))
    {
        return CopyTextWithCheckpoints(bytes, checkpoint);
    }

    return Unicode::DecodeLegacyCyrillicToUtf8(
        bytes,
        "Failed to decode INP payload from CP1251 or CP866.",
        checkpoint);
}

[[nodiscard]] std::string BuildArchiveName(const std::string_view inpEntryNameUtf8)
{
    const std::size_t separator = inpEntryNameUtf8.find_last_of("/\\");
    std::string fileName = separator == std::string_view::npos
        ? std::string{inpEntryNameUtf8}
        : std::string{inpEntryNameUtf8.substr(separator + 1)};

    if (Foundation::EndsWithAsciiInsensitive(fileName, ".inp"))
    {
        fileName.resize(fileName.size() - 4);
    }

    return fileName;
}

[[nodiscard]] std::optional<SInpxAuthor> ParseAuthor(
    const std::string_view rawAuthor,
    const CInpxParser::FCheckpointCallback& checkpoint)
{
    const auto split = SplitViews(rawAuthor, ',', GMaxAuthorNameParts, checkpoint);
    if (split.ExceededLimit)
    {
        return std::nullopt;
    }

    const auto& parts = split.Parts;
    SInpxAuthor author;
    if (!parts.empty())
    {
        author.LastNameUtf8 = TrimAsciiWhitespace(parts[0], checkpoint);
    }
    if (parts.size() > 1)
    {
        author.FirstNameUtf8 = TrimAsciiWhitespace(parts[1], checkpoint);
    }
    if (parts.size() > 2)
    {
        author.MiddleNameUtf8 = TrimAsciiWhitespace(parts[2], checkpoint);
    }

    std::string displayName;
    if (!author.FirstNameUtf8.empty())
    {
        AppendTextWithCheckpoints(displayName, author.FirstNameUtf8, checkpoint);
    }
    if (!author.MiddleNameUtf8.empty())
    {
        if (!displayName.empty())
        {
            displayName.push_back(' ');
        }
        AppendTextWithCheckpoints(displayName, author.MiddleNameUtf8, checkpoint);
    }
    if (!author.LastNameUtf8.empty())
    {
        if (!displayName.empty())
        {
            displayName.push_back(' ');
        }
        AppendTextWithCheckpoints(displayName, author.LastNameUtf8, checkpoint);
    }
    if (displayName.empty())
    {
        displayName = TrimAsciiWhitespace(rawAuthor, checkpoint);
    }

    author.DisplayNameUtf8 = std::move(displayName);
    return author;
}

[[nodiscard]] bool ParseRecordLine(
    const std::string_view archiveNameUtf8,
    const std::string_view inpEntryNameUtf8,
    const std::size_t lineNumber,
    const std::string_view lineUtf8,
    SInpxParseSummary& summary,
    const CInpxParser::FRecordCallback& onRecord,
    const CInpxParser::FWarningCallback& onWarning,
    const CInpxParser::FCheckpointCallback& checkpoint)
{
    const auto fieldSplit = SplitViews(
        lineUtf8,
        GFieldSeparator,
        GExpectedFieldCount,
        checkpoint);
    if (fieldSplit.ExceededLimit || fieldSplit.Parts.size() != GExpectedFieldCount)
    {
        EmitWarning(
            summary,
            onWarning,
            inpEntryNameUtf8,
            lineNumber,
            "Skipped malformed INPX record: expected 15 fields.",
            true);
        return false;
    }
    const auto& fields = fieldSplit.Parts;

    std::string fileNameUtf8 = TrimAsciiWhitespace(fields[5], checkpoint);
    std::string libId = TrimAsciiWhitespace(fields[7], checkpoint);
    if (fileNameUtf8.empty() || libId.empty())
    {
        EmitWarning(
            summary,
            onWarning,
            inpEntryNameUtf8,
            lineNumber,
            "Skipped malformed INPX record: file name or LibId is empty.",
            true);
        return false;
    }

    const auto fileSizeBytes = TryParseInteger<std::uint64_t>(TrimAsciiWhitespace(fields[6], checkpoint));
    if (!fileSizeBytes.has_value())
    {
        EmitWarning(
            summary,
            onWarning,
            inpEntryNameUtf8,
            lineNumber,
            "Skipped malformed INPX record: FileSize is not a valid integer.",
            true);
        return false;
    }

    const auto deletedRaw = TrimAsciiWhitespace(fields[8], checkpoint);
    const auto deletedValue = deletedRaw.empty() ? std::optional<int>{0} : TryParseInteger<int>(deletedRaw);
    if (!deletedValue.has_value())
    {
        EmitWarning(
            summary,
            onWarning,
            inpEntryNameUtf8,
            lineNumber,
            "Skipped malformed INPX record: Deleted is not a valid integer.",
            true);
        return false;
    }

    std::string fileExtensionUtf8 = TrimAsciiWhitespace(fields[9], checkpoint);
    if (fileExtensionUtf8.size() > GMaxFileExtensionBytes)
    {
        EmitWarning(
            summary,
            onWarning,
            inpEntryNameUtf8,
            lineNumber,
            "Skipped malformed INPX record: FileExt exceeds 32 bytes.",
            true);
        return false;
    }

    const auto authorSplit = SplitViews(
        fields[0],
        ':',
        GMaxAuthorsPerRecord,
        checkpoint);
    if (authorSplit.ExceededLimit)
    {
        EmitWarning(
            summary,
            onWarning,
            inpEntryNameUtf8,
            lineNumber,
            "Skipped malformed INPX record: too many authors.",
            true);
        return false;
    }

    std::vector<SInpxAuthor> authors;
    authors.reserve(authorSplit.Parts.size());
    for (const auto authorRaw : authorSplit.Parts)
    {
        const std::string trimmed = TrimAsciiWhitespace(authorRaw, checkpoint);
        if (trimmed.empty())
        {
            continue;
        }

        auto author = ParseAuthor(trimmed, checkpoint);
        if (!author.has_value())
        {
            EmitWarning(
                summary,
                onWarning,
                inpEntryNameUtf8,
                lineNumber,
                "Skipped malformed INPX record: author has more than three name components.",
                true);
            return false;
        }
        authors.push_back(std::move(*author));
    }

    auto genres = SplitNonEmptyUtf8(fields[1], ':', GMaxGenresPerRecord, checkpoint);
    if (genres.ExceededLimit)
    {
        EmitWarning(
            summary,
            onWarning,
            inpEntryNameUtf8,
            lineNumber,
            "Skipped malformed INPX record: too many genres.",
            true);
        return false;
    }

    auto keywords = SplitNonEmptyUtf8(fields[13], ':', GMaxKeywordsPerRecord, checkpoint);
    if (keywords.ExceededLimit)
    {
        EmitWarning(
            summary,
            onWarning,
            inpEntryNameUtf8,
            lineNumber,
            "Skipped malformed INPX record: too many keywords.",
            true);
        return false;
    }

    SInpxRecord record;
    record.InpEntryNameUtf8 = CopyTextWithCheckpoints(inpEntryNameUtf8, checkpoint);
    record.ArchiveNameUtf8 = CopyTextWithCheckpoints(archiveNameUtf8, checkpoint);
    record.FileNameUtf8 = std::move(fileNameUtf8);
    record.FileSizeBytes = *fileSizeBytes;
    record.LibId = std::move(libId);
    record.Deleted = *deletedValue != 0;
    record.FileExtensionUtf8 = Foundation::ToLowerAscii(fileExtensionUtf8);
    if (record.FileExtensionUtf8.empty())
    {
        record.FileExtensionUtf8 = "fb2";
    }
    AppendTextWithCheckpoints(record.EntryNameUtf8, record.FileNameUtf8, checkpoint);
    record.EntryNameUtf8.push_back('.');
    record.EntryNameUtf8.append(record.FileExtensionUtf8);
    record.TitleUtf8 = TrimAsciiWhitespace(fields[2], checkpoint);
    if (record.TitleUtf8.empty())
    {
        record.TitleUtf8 = CopyTextWithCheckpoints(record.FileNameUtf8, checkpoint);
    }

    std::string seriesUtf8 = TrimAsciiWhitespace(fields[3], checkpoint);
    if (!seriesUtf8.empty())
    {
        record.SeriesUtf8 = std::move(seriesUtf8);
    }

    const std::string seriesNumberRaw = TrimAsciiWhitespace(fields[4], checkpoint);
    if (!seriesNumberRaw.empty())
    {
        const auto seriesNumber = TryParseInteger<int>(seriesNumberRaw);
        if (seriesNumber.has_value())
        {
            record.SeriesNumber = seriesNumber;
        }
        else
        {
            EmitWarning(
                summary,
                onWarning,
                inpEntryNameUtf8,
                lineNumber,
                "INPX record has invalid SeriesNumber; value ignored.");
        }
    }

    std::string dateAddedUtf8 = TrimAsciiWhitespace(fields[10], checkpoint);
    if (!dateAddedUtf8.empty())
    {
        record.DateAddedUtf8 = std::move(dateAddedUtf8);
    }
    record.LanguageUtf8 = TrimAsciiWhitespace(fields[11], checkpoint);
    record.Authors = std::move(authors);
    record.GenresUtf8 = std::move(genres.Parts);
    record.KeywordsUtf8 = std::move(keywords.Parts);

    if (onRecord)
    {
        onRecord(std::move(record));
    }

    return true;
}

} // namespace

SInpxParseResult CInpxParser::ParseAll(const std::filesystem::path& inpxPath) const
{
    SInpxParseResult result;
    result.Summary = Parse(
        inpxPath,
        [&result](SInpxRecord&& record) {
            result.Records.push_back(std::move(record));
        },
        [&result](const SInpxWarning& warning) {
            result.Warnings.push_back(warning);
        });
    return result;
}

SInpxParseSummary CInpxParser::Parse(
    const std::filesystem::path& inpxPath,
    FRecordCallback onRecord,
    const FWarningCallback& onWarning) const
{
    SInpxParseSummary summary;
    static_cast<void>(ReadSegments(inpxPath, [&](SInpxSegmentPayload&& segment) {
        const auto segmentSummary = ParseSegment(segment, onRecord, onWarning);
        ++summary.InpEntryCount;
        summary.TotalRecords += segmentSummary.TotalRecords;
        summary.ParsedRecords += segmentSummary.ParsedRecords;
        summary.SkippedRecords += segmentSummary.SkippedRecords;
        summary.DeletedRecords += segmentSummary.DeletedRecords;
        summary.WarningCount += segmentSummary.WarningCount;
    }));

    return summary;
}

std::size_t CInpxParser::ReadSegments(
    const std::filesystem::path& inpxPath,
    const FSegmentCallback& onSegment,
    const FCheckpointCallback& checkpoint,
    const std::uint64_t maxTotalInputBytes) const
{
    RunCheckpoint(checkpoint);
    CZipArchive archive(inpxPath);
    std::size_t segmentCount = 0;
    std::uint64_t totalInputBytes = 0;
    for (zip_uint64_t index = 0; index < archive.EntryCount(); ++index)
    {
        RunCheckpoint(checkpoint);
        const std::string entryNameUtf8 = archive.EntryName(index);
        if (!Foundation::EndsWithAsciiInsensitive(entryNameUtf8, ".inp"))
        {
            continue;
        }

        const zip_uint64_t entrySize = archive.EntrySize(index);
        if (entrySize > maxTotalInputBytes - totalInputBytes)
        {
            throw std::runtime_error(
                "INPX metadata input exceeds the configured scan planning limit of "
                + std::to_string(maxTotalInputBytes) + " bytes.");
        }
        totalInputBytes += entrySize;
        std::string bytes = archive.ReadTextEntry(index, entrySize, checkpoint);
        const std::string fingerprintUtf8 = ComputeFingerprintWithCheckpoints(bytes, checkpoint);
        ++segmentCount;
        if (onSegment)
        {
            onSegment({
                .InpEntryNameUtf8 = entryNameUtf8,
                .ArchiveNameUtf8 = BuildArchiveName(entryNameUtf8),
                .FingerprintUtf8 = fingerprintUtf8,
                .Bytes = std::move(bytes)
            });
        }
    }
    return segmentCount;
}

SInpxParseSummary CInpxParser::ParseSegment(
    const SInpxSegmentPayload& segment,
    FRecordCallback onRecord,
    const FWarningCallback& onWarning,
    const FCheckpointCallback& checkpoint) const
{
    SInpxParseSummary summary;
    summary.InpEntryCount = 1;
    const std::string decodedText = DecodeInpPayloadToUtf8(segment.Bytes, checkpoint);
    std::size_t lineNumber = 0;
    std::size_t start = 0;
    while (start <= decodedText.size())
    {
        RunCheckpoint(checkpoint);
        const std::size_t end = FindByteWithCheckpoints(decodedText, '\n', start, checkpoint);
        std::string_view line = end == std::string::npos
            ? std::string_view{decodedText}.substr(start)
            : std::string_view{decodedText}.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }

        ++lineNumber;
        if (!line.empty())
        {
            if (line.size() > GMaxInpRecordLineBytes)
            {
                throw std::runtime_error(
                    "INP entry '" + segment.InpEntryNameUtf8 + "' line "
                    + std::to_string(lineNumber) + " exceeds the "
                    + std::to_string(GMaxInpRecordLineBytes) + "-byte record limit.");
            }
            ++summary.TotalRecords;
            if (ParseRecordLine(
                    segment.ArchiveNameUtf8,
                    segment.InpEntryNameUtf8,
                    lineNumber,
                    line,
                    summary,
                    [&summary, &onRecord](SInpxRecord&& record) {
                        ++summary.ParsedRecords;
                        if (record.Deleted)
                        {
                            ++summary.DeletedRecords;
                        }
                        if (onRecord)
                        {
                            onRecord(std::move(record));
                        }
                    },
                    onWarning,
                    checkpoint))
            {
                // handled in callback
            }
            else
            {
                ++summary.SkippedRecords;
            }
        }

        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }
    return summary;
}

std::vector<std::string> CInpxParser::ReadArchiveNameSamples(
    const std::filesystem::path& inpxPath,
    const std::size_t maxSamples) const
{
    CZipArchive archive(inpxPath);
    std::vector<std::string> archiveNames;
    archiveNames.reserve(maxSamples);
    std::set<std::string> observedArchiveNames;

    for (zip_uint64_t index = 0; index < archive.EntryCount() && archiveNames.size() < maxSamples; ++index)
    {
        const std::string entryNameUtf8 = archive.EntryName(index);
        if (!Foundation::EndsWithAsciiInsensitive(entryNameUtf8, ".inp"))
        {
            continue;
        }

        const std::string archiveNameUtf8 = BuildArchiveName(entryNameUtf8);
        if (observedArchiveNames.insert(archiveNameUtf8).second)
        {
            archiveNames.push_back(archiveNameUtf8);
        }
    }

    return archiveNames;
}

} // namespace InpxWebReader::Inpx
