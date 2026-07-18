#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace InpxWebReader::Inpx {

struct SInpxAuthor
{
    std::string DisplayNameUtf8;
    std::string LastNameUtf8;
    std::string FirstNameUtf8;
    std::string MiddleNameUtf8;
};

struct SInpxRecord
{
    std::vector<SInpxAuthor> Authors;
    std::vector<std::string> GenresUtf8;
    std::vector<std::string> KeywordsUtf8;
    std::string TitleUtf8;
    std::optional<std::string> SeriesUtf8 = std::nullopt;
    std::optional<int> SeriesNumber = std::nullopt;
    std::string FileNameUtf8;
    std::uint64_t FileSizeBytes = 0;
    std::string LibId;
    bool Deleted = false;
    std::string FileExtensionUtf8 = "fb2";
    std::optional<std::string> DateAddedUtf8 = std::nullopt;
    std::string LanguageUtf8;
    std::string InpEntryNameUtf8;
    std::string ArchiveNameUtf8;
    std::string EntryNameUtf8;
};

struct SInpxSegmentPayload
{
    std::string InpEntryNameUtf8;
    std::string ArchiveNameUtf8;
    std::string FingerprintUtf8;
    std::string Bytes;
};

struct SInpxWarning
{
    std::string MessageUtf8;
    std::string InpEntryNameUtf8;
    std::size_t LineNumber = 0;
    bool RecordSkipped = false;
};

struct SInpxParseSummary
{
    std::size_t InpEntryCount = 0;
    std::size_t TotalRecords = 0;
    std::size_t ParsedRecords = 0;
    std::size_t SkippedRecords = 0;
    std::size_t DeletedRecords = 0;
    std::size_t WarningCount = 0;
};

struct SInpxParseResult
{
    std::vector<SInpxRecord> Records;
    std::vector<SInpxWarning> Warnings;
    SInpxParseSummary Summary;
};

} // namespace InpxWebReader::Inpx
