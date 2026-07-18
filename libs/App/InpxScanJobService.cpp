#include "App/InpxScanJobService.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <BS_thread_pool.hpp>

#include "App/InpxArchiveAccess.hpp"
#include "App/InpxCacheBootstrap.hpp"
#include "App/IInpxCatalogApplication.hpp"
#include "Database/CatalogStatisticsMaintenance.hpp"
#include "Database/SearchIndexMaintenance.hpp"
#include "Database/SqliteConnection.hpp"
#include "Database/SqliteEntityHelpers.hpp"
#include "Database/SqliteGenreHelpers.hpp"
#include "Database/SqliteStatement.hpp"
#include "Database/SqliteTimePoint.hpp"
#include "Database/SqliteTransaction.hpp"
#include "Domain/BookFormat.hpp"
#include "Domain/InpxBookAvailability.hpp"
#include "Domain/LanguageNormalization.hpp"
#include "Domain/MetadataNormalization.hpp"
#include "Domain/ServiceContracts.hpp"
#include "Foundation/FileSystemUtils.hpp"
#include "Foundation/Logging.hpp"
#include "Foundation/StringUtils.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "Inpx/InpxParser.hpp"
#include "Foundation/Sha256Fingerprint.hpp"
#include "ScanSupport/ScanConcurrency.hpp"
#include "ScanSupport/CoverCacheProcessing.hpp"
#include "ScanSupport/ScanPerfTracker.hpp"
#include "ScanSupport/ScanPerfScopes.hpp"
#include "Parsing/Fb2Parser.hpp"
#include "Storage/InpxArchivePathResolver.hpp"
#include "Storage/InpxCacheLayout.hpp"
#include "Storage/PathSafety.hpp"

namespace InpxWebReader::ApplicationJobs {
namespace {

class CInpxScanCancelled final : public std::exception
{
public:
    [[nodiscard]] const char* what() const noexcept override
    {
        return "INPX scan cancelled.";
    }
};

struct SInpxScanWarningRecord
{
    std::string Code;
    std::optional<std::string> ArchiveNameUtf8 = std::nullopt;
    std::optional<std::string> EntryNameUtf8 = std::nullopt;
    std::string MessageUtf8;
};

struct SPreparedInpxPayload
{
    Domain::SParsedBook ParsedBook;
    std::optional<InpxWebReader::ScanSupport::ECoverCacheResolution> CoverStorageResolution = std::nullopt;
    std::size_t CoverWarningCount = 0;
};

struct SInFlightParsedRecord
{
    std::int64_t SegmentId = 0;
    const Inpx::SInpxRecord* InpxRecord = nullptr;
    std::shared_future<SPreparedInpxPayload> PreparedPayload;
    std::chrono::steady_clock::time_point StartedAt;
    std::uint64_t ReservedPayloadBytes = 0;
};

struct SExistingInpxBook
{
    std::int64_t BookId = 0;
    std::optional<std::string> CoverPathUtf8 = std::nullopt;
    std::string AvailabilityUtf8;
    std::string ArchiveNameUtf8;
    std::string EntryNameUtf8;
    std::int64_t FileSizeBytes = 0;
    std::string FormatUtf8;
};

constexpr std::string_view GUnicodeReplacementCharacterUtf8 = "\xEF\xBF\xBD";
constexpr std::size_t GExecutionPlanSortCheckpointInterval = 1'024;

// Matches CFb2Parser's anonymous-author fallback value.
constexpr std::string_view GAnonymousAuthorFallbackUtf8 =
    "\xD0\x90\xD0\xBD\xD0\xBE\xD0\xBD\xD0\xB8\xD0\xBC";

struct SInpxMetadataFallbackCounters
{
    std::size_t TitleFallbackCount = 0;
    std::size_t AuthorFallbackCount = 0;
    std::size_t LanguageFallbackCount = 0;

    [[nodiscard]] std::size_t Total() const noexcept
    {
        return TitleFallbackCount + AuthorFallbackCount + LanguageFallbackCount;
    }
};

struct SInpxCatalogApplyResult
{
    std::size_t AddedRecords = 0;
    std::size_t UpdatedRecords = 0;
    std::size_t MarkedUnavailableRecords = 0;
    std::size_t CachedCoverRecords = 0;
    std::uint64_t CachedCoverBytes = 0;
    std::size_t ParserWarningCount = 0;
    std::size_t CoverWarningCount = 0;
    SInpxMetadataFallbackCounters MetadataFallbacks;
};

struct SInpxRecordWriteResult
{
    bool Added = false;
    bool Updated = false;
    bool CachedCover = false;
    std::uint64_t CachedCoverBytes = 0;
    std::size_t ParserWarningCount = 0;
    std::size_t CoverWarningCount = 0;
    SInpxMetadataFallbackCounters MetadataFallbacks;
};

struct SInpxCachedCoverResult
{
    std::optional<std::string> RelativePathUtf8 = std::nullopt;
    std::uint64_t Bytes = 0;
    std::size_t WarningCount = 0;
};

constexpr std::size_t GInpxScanProgressLogEveryRecords = 10'000;
constexpr auto GInpxScanProgressLogEveryTime = std::chrono::seconds(30);

struct SInpxScanProgressLogView
{
    int Percent = 0;
    std::size_t TotalRecords = 0;
    std::size_t ScannedRecords = 0;
    std::size_t ParsedFb2Records = 0;
    std::size_t AddedRecords = 0;
    std::size_t UpdatedRecords = 0;
    std::size_t MarkedUnavailableRecords = 0;
    std::size_t SkippedRecords = 0;
    std::string CurrentArchiveNameUtf8;
    std::string CurrentEntryNameUtf8;
};

class CInpxScanProgressLogger final
{
public:
    CInpxScanProgressLogger(
        const TInpxScanJobId jobId,
        const std::chrono::steady_clock::time_point startedAt) noexcept
        : m_jobId(jobId)
        , m_startedAt(startedAt)
        , m_lastLogAt(startedAt)
    {
    }

    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> ClaimIfDue(
        const std::size_t scannedRecords) noexcept
    {
        const auto now = std::chrono::steady_clock::now();
        const bool firstRecord = scannedRecords == 1;
        const bool enoughRecords = scannedRecords >= m_nextRecordLog;
        const bool enoughTime = now - m_lastLogAt >= GInpxScanProgressLogEveryTime;
        if (!firstRecord && !enoughRecords && !enoughTime)
        {
            return std::nullopt;
        }

        while (m_nextRecordLog <= scannedRecords)
        {
            m_nextRecordLog += GInpxScanProgressLogEveryRecords;
        }
        m_lastLogAt = now;
        return now;
    }

    void Log(
        const SInpxScanProgressLogView& snapshot,
        const std::size_t scanWarningCount,
        const std::size_t inFlightPayloads,
        const std::chrono::steady_clock::time_point now) const noexcept
    {
        try
        {
            const double elapsedSec = std::chrono::duration<double>(now - m_startedAt).count();
            const double throughput = elapsedSec > 0.0
                ? static_cast<double>(snapshot.ScannedRecords) / elapsedSec
                : 0.0;
            std::string currentSource = snapshot.CurrentArchiveNameUtf8;
            if (!snapshot.CurrentEntryNameUtf8.empty())
            {
                if (!currentSource.empty())
                {
                    currentSource += "!";
                }
                currentSource += snapshot.CurrentEntryNameUtf8;
            }

            InpxWebReader::Logging::InfoIfInitialized(
                "INPX scan progress. jobId={} scanned={} total={} percent={} parsedFb2={} added={} updated={} unavailable={} skipped={} scanWarnings={} inFlightPayloads={} current='{}' throughput={:.1f} rec/s",
                m_jobId,
                snapshot.ScannedRecords,
                snapshot.TotalRecords,
                snapshot.Percent,
                snapshot.ParsedFb2Records,
                snapshot.AddedRecords,
                snapshot.UpdatedRecords,
                snapshot.MarkedUnavailableRecords,
                snapshot.SkippedRecords,
                scanWarningCount,
                inFlightPayloads,
                currentSource,
                throughput);
        }
        catch (...)
        {
            (void)0;
        }
    }

private:
    TInpxScanJobId m_jobId = 0;
    std::chrono::steady_clock::time_point m_startedAt;
    std::chrono::steady_clock::time_point m_lastLogAt;
    std::size_t m_nextRecordLog = GInpxScanProgressLogEveryRecords;
};

[[nodiscard]] std::string BuildModeLabel(const EInpxScanMode mode)
{
    return mode == EInpxScanMode::InitialScan ? "initial scan" : "rescan";
}

[[nodiscard]] int ComputePercent(const std::size_t scannedRecords, const std::size_t totalRecords) noexcept
{
    if (totalRecords == 0)
    {
        return 0;
    }

    return (std::min)(99, static_cast<int>((scannedRecords * 100) / totalRecords));
}

[[nodiscard]] std::string MakeScanId()
{
    static std::atomic_uint64_t sequence{0};
    const auto now = std::chrono::system_clock::now();
    return std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count())
        + '-' + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

[[nodiscard]] std::string BuildWarningMessage(
    const std::optional<std::string>& archiveNameUtf8,
    const std::optional<std::string>& entryNameUtf8,
    const std::string& messageUtf8)
{
    if (entryNameUtf8.has_value())
    {
        return *entryNameUtf8 + ": " + messageUtf8;
    }

    if (archiveNameUtf8.has_value())
    {
        return *archiveNameUtf8 + ": " + messageUtf8;
    }

    return messageUtf8;
}

[[nodiscard]] std::string BuildWarningSourceLabel(const SInpxScanWarningRecord& warning)
{
    if (warning.ArchiveNameUtf8.has_value() && warning.EntryNameUtf8.has_value())
    {
        return *warning.ArchiveNameUtf8 + "!" + *warning.EntryNameUtf8;
    }

    if (warning.EntryNameUtf8.has_value())
    {
        return *warning.EntryNameUtf8;
    }

    if (warning.ArchiveNameUtf8.has_value())
    {
        return *warning.ArchiveNameUtf8;
    }

    return "<inpx>";
}

void LogInpxScanWarningNoThrow(
    const TInpxScanJobId jobId,
    const SInpxScanWarningRecord& warning,
    const bool countAsSkipped) noexcept
{
    try
    {
        InpxWebReader::Logging::WarnIfInitialized(
            "INPX scan warning. jobId={} code='{}' source='{}' skipped={} message='{}'",
            jobId,
            warning.Code,
            BuildWarningSourceLabel(warning),
            countAsSkipped,
            warning.MessageUtf8);
    }
    catch (...)
    {
        (void)0;
    }
}

void InsertAuthors(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::int64_t bookId,
    const std::vector<std::string>& authors)
{
    std::unordered_set<std::string> insertedAuthors;

    for (std::size_t index = 0; index < authors.size(); ++index)
    {
        const std::string normalizedAuthor = InpxWebReader::Domain::NormalizeText(authors[index]);
        if (normalizedAuthor.empty() || !insertedAuthors.insert(normalizedAuthor).second)
        {
            continue;
        }

        const std::int64_t authorId = InpxWebReader::Sqlite::ResolveEntityId(
            connection,
            "authors",
            normalizedAuthor,
            authors[index]);

        InpxWebReader::Sqlite::CSqliteStatement linkStatement(
            connection.GetNativeHandle(),
            "INSERT INTO book_authors (book_id, author_id, author_order) VALUES (?, ?, ?);");
        linkStatement.BindInt64(1, bookId);
        linkStatement.BindInt64(2, authorId);
        linkStatement.BindInt(3, static_cast<int>(index));
        static_cast<void>(linkStatement.Step());
    }
}

void InsertTags(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::int64_t bookId,
    const std::vector<std::string>& tags)
{
    std::unordered_set<std::string> insertedTags;

    for (const std::string& tag : tags)
    {
        const std::string normalizedTag = InpxWebReader::Domain::NormalizeText(tag);
        if (normalizedTag.empty() || !insertedTags.insert(normalizedTag).second)
        {
            continue;
        }

        const std::int64_t tagId = InpxWebReader::Sqlite::ResolveEntityId(
            connection,
            "tags",
            normalizedTag,
            tag);

        InpxWebReader::Sqlite::CSqliteStatement linkStatement(
            connection.GetNativeHandle(),
            "INSERT INTO book_tags (book_id, tag_id) VALUES (?, ?);");
        linkStatement.BindInt64(1, bookId);
        linkStatement.BindInt64(2, tagId);
        static_cast<void>(linkStatement.Step());
    }
}

void ReplaceBookRelations(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::int64_t bookId,
    const InpxWebReader::Domain::SBookMetadata& metadata)
{
    for (const std::string_view tableName : {"book_authors", "book_tags", "book_genres"})
    {
        InpxWebReader::Sqlite::CSqliteStatement deleteStatement(
            connection.GetNativeHandle(),
            "DELETE FROM " + std::string{tableName} + " WHERE book_id = ?;");
        deleteStatement.BindInt64(1, bookId);
        static_cast<void>(deleteStatement.Step());
    }

    InsertAuthors(connection, bookId, metadata.AuthorsUtf8);
    InsertTags(connection, bookId, metadata.TagsUtf8);
    InpxWebReader::BookDatabase::CSqliteGenreHelpers::InsertGenres(connection, bookId, metadata.GenresUtf8);
}

[[nodiscard]] std::optional<InpxWebReader::Domain::SBookMetadata> ReadStoredMetadata(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::int64_t bookId)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT title, language, series, series_index, publisher, year, isbn, description, identifier "
        "FROM inpx_books WHERE id = ?;");
    statement.BindInt64(1, bookId);

    if (!statement.Step())
    {
        return std::nullopt;
    }

    InpxWebReader::Domain::SBookMetadata metadata;
    metadata.TitleUtf8 = statement.GetColumnText(0);
    metadata.Language = statement.GetColumnText(1);
    metadata.SeriesUtf8 = statement.IsColumnNull(2) ? std::nullopt : std::make_optional(statement.GetColumnText(2));
    metadata.SeriesIndex = statement.IsColumnNull(3) ? std::nullopt : std::make_optional(statement.GetColumnDouble(3));
    metadata.PublisherUtf8 = statement.IsColumnNull(4) ? std::nullopt : std::make_optional(statement.GetColumnText(4));
    metadata.Year = statement.IsColumnNull(5) ? std::nullopt : std::make_optional(statement.GetColumnInt(5));
    metadata.Isbn = statement.IsColumnNull(6) ? std::nullopt : std::make_optional(statement.GetColumnText(6));
    metadata.DescriptionUtf8 = statement.IsColumnNull(7) ? std::nullopt : std::make_optional(statement.GetColumnText(7));
    metadata.Identifier = statement.IsColumnNull(8) ? std::nullopt : std::make_optional(statement.GetColumnText(8));
    metadata.AuthorsUtf8 = InpxWebReader::Sqlite::ReadRelatedEntityNames(
        connection,
        "SELECT a.display_name "
        "FROM book_authors ba "
        "INNER JOIN authors a ON a.id = ba.author_id "
        "WHERE ba.book_id = ? "
        "ORDER BY ba.author_order ASC;",
        bookId);
    metadata.TagsUtf8 = InpxWebReader::Sqlite::ReadRelatedEntityNames(
        connection,
        "SELECT t.display_name "
        "FROM book_tags bt "
        "INNER JOIN tags t ON t.id = bt.tag_id "
        "WHERE bt.book_id = ? "
        "ORDER BY t.display_name COLLATE NOCASE ASC;",
        bookId);
    metadata.GenresUtf8 = InpxWebReader::Sqlite::ReadRelatedEntityNames(
        connection,
        "SELECT g.display_name "
        "FROM book_genres bg "
        "INNER JOIN genres g ON g.id = bg.genre_id "
        "WHERE bg.book_id = ? "
        "ORDER BY g.display_name COLLATE NOCASE ASC;",
        bookId);
    return metadata;
}

[[nodiscard]] std::int64_t ReadInpxSourceId(const InpxWebReader::Sqlite::CSqliteConnection& connection)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT id FROM inpx_sources ORDER BY id ASC LIMIT 1;");
    if (!statement.Step())
    {
        throw std::runtime_error("INPX source row is missing.");
    }

    return statement.GetColumnInt64(0);
}

[[nodiscard]] std::optional<SExistingInpxBook> ReadExistingInpxBook(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::int64_t sourceId,
    const std::string& libId)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT l.book_id, b.cover_path, l.availability, l.archive_name, l.entry_name, l.file_size_bytes, l.format "
        "FROM inpx_book_locations l "
        "INNER JOIN inpx_books b ON b.id = l.book_id "
        "WHERE l.source_id = ? AND l.lib_id = ?;");
    statement.BindInt64(1, sourceId);
    statement.BindText(2, libId);

    if (!statement.Step())
    {
        return std::nullopt;
    }

    return SExistingInpxBook{
        .BookId = statement.GetColumnInt64(0),
        .CoverPathUtf8 = statement.IsColumnNull(1) || statement.GetColumnText(1).empty()
            ? std::nullopt
            : std::make_optional(statement.GetColumnText(1)),
        .AvailabilityUtf8 = statement.GetColumnText(2),
        .ArchiveNameUtf8 = statement.GetColumnText(3),
        .EntryNameUtf8 = statement.GetColumnText(4),
        .FileSizeBytes = statement.GetColumnInt64(5),
        .FormatUtf8 = statement.GetColumnText(6)
    };
}

[[nodiscard]] bool ContainsUnicodeReplacementCharacter(const std::string_view value) noexcept
{
    return value.find(GUnicodeReplacementCharacterUtf8) != std::string_view::npos;
}

[[nodiscard]] bool ContainsUnicodeReplacementCharacter(const std::vector<std::string>& values) noexcept
{
    return std::any_of(values.begin(), values.end(), [](const std::string& value) {
        return ContainsUnicodeReplacementCharacter(value);
    });
}

[[nodiscard]] bool IsAnonymousAuthorFallback(const std::vector<std::string>& authors) noexcept
{
    return authors.size() == 1 && authors.front() == GAnonymousAuthorFallbackUtf8;
}

[[nodiscard]] std::vector<std::string> BuildCleanInpxAuthorDisplayNames(const Inpx::SInpxRecord& inpxRecord)
{
    std::vector<std::string> authors;
    authors.reserve(inpxRecord.Authors.size());

    for (const Inpx::SInpxAuthor& author : inpxRecord.Authors)
    {
        if (!author.DisplayNameUtf8.empty() && !ContainsUnicodeReplacementCharacter(author.DisplayNameUtf8))
        {
            authors.push_back(author.DisplayNameUtf8);
        }
    }

    return authors;
}

struct SInpxStoredMetadataResult
{
    InpxWebReader::Domain::SBookMetadata Metadata;
    SInpxMetadataFallbackCounters Fallbacks;
};

[[nodiscard]] SInpxStoredMetadataResult BuildStoredMetadata(
    const Inpx::SInpxRecord& inpxRecord,
    const InpxWebReader::Domain::SParsedBook& parsedBook)
{
    SInpxStoredMetadataResult result = {
        .Metadata = parsedBook.Metadata
    };
    InpxWebReader::Domain::SBookMetadata& metadata = result.Metadata;

    if (ContainsUnicodeReplacementCharacter(metadata.TitleUtf8)
        && !inpxRecord.TitleUtf8.empty()
        && !ContainsUnicodeReplacementCharacter(inpxRecord.TitleUtf8))
    {
        ++result.Fallbacks.TitleFallbackCount;
        InpxWebReader::Logging::WarnIfInitialized(
            "INPX scan recovered replacement-character title from INPX row. libId='{}' source='{}!{}'",
            inpxRecord.LibId,
            inpxRecord.ArchiveNameUtf8,
            inpxRecord.EntryNameUtf8);
        metadata.TitleUtf8 = inpxRecord.TitleUtf8;
    }

    const std::vector<std::string> inpxAuthors = BuildCleanInpxAuthorDisplayNames(inpxRecord);
    if (!inpxAuthors.empty()
        && (metadata.AuthorsUtf8.empty()
            || ContainsUnicodeReplacementCharacter(metadata.AuthorsUtf8)
            || IsAnonymousAuthorFallback(metadata.AuthorsUtf8)))
    {
        ++result.Fallbacks.AuthorFallbackCount;
        InpxWebReader::Logging::WarnIfInitialized(
            "INPX scan recovered payload author metadata from INPX row. libId='{}' source='{}!{}'",
            inpxRecord.LibId,
            inpxRecord.ArchiveNameUtf8,
            inpxRecord.EntryNameUtf8);
        metadata.AuthorsUtf8 = inpxAuthors;
    }

    if (!metadata.Identifier.has_value() || metadata.Identifier->empty())
    {
        metadata.Identifier = inpxRecord.LibId;
    }

    if (metadata.Language.empty() && !inpxRecord.LanguageUtf8.empty())
    {
        ++result.Fallbacks.LanguageFallbackCount;
        metadata.Language = inpxRecord.LanguageUtf8;
    }

    metadata.Language = InpxWebReader::Domain::NormalizeLanguage(metadata.Language);

    return result;
}

[[nodiscard]] std::string BuildInpxSourceLabel(const Inpx::SInpxRecord& inpxRecord)
{
    return inpxRecord.ArchiveNameUtf8 + "!" + inpxRecord.EntryNameUtf8;
}

[[nodiscard]] std::size_t ResolveInpxPayloadWorkerWindow(const std::size_t maxWorkerCount) noexcept
{
    if (maxWorkerCount > 0)
    {
        return (std::min<std::size_t>)(maxWorkerCount, InpxWebReader::ScanSupport::GMaxScanWorkerCount);
    }

    return InpxWebReader::ScanSupport::ResolveScanWorkerCount(
        InpxWebReader::ScanSupport::EScanWorkerPolicy::UseAllAvailable,
        2);
}

[[nodiscard]] std::uint64_t ResolveInFlightPayloadBudget(
    const std::uint64_t maxSteadyStateMemoryBytes) noexcept
{
    // Parsing may temporarily decode text and extract a cover in addition to the
    // raw FB2 payload. Admit raw bytes against a deliberately smaller share of
    // the process budget instead of pretending to model allocator overhead.
    return (std::max<std::uint64_t>)(1, maxSteadyStateMemoryBytes / 16);
}

[[nodiscard]] std::uint64_t ResolveArchiveManifestBudget(
    const std::uint64_t maxSteadyStateMemoryBytes) noexcept
{
    const std::uint64_t plannedRecordAndManifestBudget = maxSteadyStateMemoryBytes / 2;
    return (std::min)(
        plannedRecordAndManifestBudget,
        (std::max<std::uint64_t>)(1, maxSteadyStateMemoryBytes / 16));
}

[[nodiscard]] std::uint64_t ResolvePlanningStateBudget(
    const std::uint64_t maxSteadyStateMemoryBytes) noexcept
{
    const std::uint64_t plannedRecordAndManifestBudget = maxSteadyStateMemoryBytes / 2;
    return plannedRecordAndManifestBudget
        - ResolveArchiveManifestBudget(maxSteadyStateMemoryBytes);
}

struct SScanPlanningLimits
{
    std::uint64_t MaxInpSourceBytes = 1;
    std::size_t MaxRecords = 1;
    std::size_t MaxSegments = 1;
    std::size_t MaxFilesystemEntries = 1;
};

[[nodiscard]] std::size_t ResolveCoarseCountLimit(
    const std::uint64_t budgetBytes,
    const std::uint64_t allowanceBytes,
    const std::size_t absoluteMaximum = (std::numeric_limits<std::size_t>::max)()) noexcept
{
    const std::uint64_t count = (std::max<std::uint64_t>)(1, budgetBytes / allowanceBytes);
    return static_cast<std::size_t>((std::min<std::uint64_t>)(count, absoluteMaximum));
}

[[nodiscard]] SScanPlanningLimits ResolveScanPlanningLimits(
    const std::uint64_t planningBudgetBytes) noexcept
{
    // These are deliberately coarse input/shape limits. They leave headroom for
    // strings and indexes without tying correctness to a standard-library layout.
    return {
        .MaxInpSourceBytes = (std::max<std::uint64_t>)(1, planningBudgetBytes / 2),
        .MaxRecords = ResolveCoarseCountLimit(planningBudgetBytes, 512),
        .MaxSegments = ResolveCoarseCountLimit(planningBudgetBytes, 4'096),
        .MaxFilesystemEntries = ResolveCoarseCountLimit(
            planningBudgetBytes,
            256,
            1'000'000)
    };
}

void RequireCollectionRoom(
    const std::size_t currentSize,
    const std::size_t maxSize,
    const std::string_view collectionLabel)
{
    if (currentSize >= maxSize)
    {
        throw std::runtime_error(
            "INPX " + std::string{collectionLabel} + " exceeds the configured scan limit of "
            + std::to_string(maxSize) + " entries.");
    }
}

[[nodiscard]] bool InsertOwnedString(
    std::set<std::string>& values,
    const std::string& value)
{
    return values.insert(value).second;
}

[[nodiscard]] bool InsertActiveLibId(
    std::set<std::string>& values,
    const std::string& value,
    const std::size_t maxRecords)
{
    if (values.contains(value))
    {
        return false;
    }
    RequireCollectionRoom(values.size(), maxRecords, "active LibId index");
    static_cast<void>(values.insert(value));
    return true;
}

[[nodiscard]] std::uint64_t ResolveCoverDecodeBudget(
    const std::uint64_t maxSteadyStateMemoryBytes,
    const std::size_t maxConcurrentCoverDecodes) noexcept
{
    constexpr std::uint64_t absoluteLimit = 64ull * 1024ull * 1024ull;
    const auto workerCount = static_cast<std::uint64_t>((std::max<std::size_t>)(1, maxConcurrentCoverDecodes));
    const auto aggregateCoverBudget = maxSteadyStateMemoryBytes
        - (maxSteadyStateMemoryBytes / 2)
        - ResolveInFlightPayloadBudget(maxSteadyStateMemoryBytes);
    return (std::min)(absoluteLimit, aggregateCoverBudget / workerCount);
}

[[nodiscard]] std::optional<InpxWebReader::ScanSupport::ECoverCacheResolution> PrepareInpxCoverForCache(
    InpxWebReader::Domain::SParsedBook& parsedBook,
    const InpxWebReader::Domain::ICoverImageProcessor* coverImageProcessor,
    const std::string_view sourceLabel,
    const std::uint64_t maxDecodedCoverBytes,
    const std::stop_token stopToken,
    InpxWebReader::ScanSupport::CScanPerfTracker& perf)
{
    if (!parsedBook.HasCover())
    {
        return std::nullopt;
    }

    auto timer = perf.MeasureStage(InpxWebReader::ScanSupport::CScanPerfTracker::EStage::Cover);
    auto cachedCover = InpxWebReader::ScanSupport::ResolveCoverCacheData(
        parsedBook,
        coverImageProcessor,
        sourceLabel,
        maxDecodedCoverBytes,
        stopToken);
    parsedBook.CoverExtension = std::move(cachedCover.Cover.Extension);
    parsedBook.CoverBytes = std::move(cachedCover.Cover.Bytes);
    return cachedCover.Resolution;
}

[[nodiscard]] SPreparedInpxPayload ParseInpxPayloadBytes(
    std::string payloadBytes,
    const Inpx::SInpxRecord& inpxRecord,
    const std::string& sourceLabel,
    const InpxWebReader::Domain::ICoverImageProcessor* coverImageProcessor,
    const bool prepareCoverForCache,
    const std::uint64_t maxDecodedCoverBytes,
    const std::stop_token stopToken,
    InpxWebReader::ScanSupport::CScanPerfTracker& perf)
{
    InpxWebReader::Domain::SParsedBook parsedBook;
    {
        auto timer = perf.MeasureStage(InpxWebReader::ScanSupport::CScanPerfTracker::EStage::Parse);
        parsedBook = InpxWebReader::Fb2Parsing::CFb2Parser{}.ParseBytes(
            std::move(payloadBytes),
            sourceLabel,
            {.MissingTitleFallbackUtf8 = inpxRecord.TitleUtf8.empty()
                ? std::nullopt
                : std::make_optional(inpxRecord.TitleUtf8),
                .MissingLanguageMayBeRecoveredByCaller = !inpxRecord.LanguageUtf8.empty(),
                .StopToken = stopToken});
    }
    std::optional<InpxWebReader::ScanSupport::ECoverCacheResolution> coverStorageResolution = std::nullopt;
    std::size_t coverWarningCount = 0;
    if (prepareCoverForCache)
    {
        coverStorageResolution = PrepareInpxCoverForCache(
            parsedBook,
            coverImageProcessor,
            sourceLabel,
            maxDecodedCoverBytes,
            stopToken,
            perf);
        if (coverStorageResolution == InpxWebReader::ScanSupport::ECoverCacheResolution::SkippedProcessorFailure)
        {
            parsedBook.CoverDiagnosticMessage = "cover-processing-failed-or-resource-limit";
            ++coverWarningCount;
        }
    }
    return {
        .ParsedBook = std::move(parsedBook),
        .CoverStorageResolution = coverStorageResolution,
        .CoverWarningCount = coverWarningCount
    };
}

void RemovePathWithWarningNoThrow(
    const std::filesystem::path& path,
    const std::string_view context) noexcept
{
    if (path.empty())
    {
        return;
    }

    const std::error_code error = InpxWebReader::Foundation::RemovePathNoThrow(path);
    if (error)
    {
        InpxWebReader::Logging::WarnIfInitialized(
            "INPX scan cleanup failed. context='{}' path='{}' error='{}'",
            context,
            InpxWebReader::Unicode::PathToUtf8(path),
            error.message());
    }
}

void WriteCoverBytesAtomically(
    const std::filesystem::path& path,
    const std::vector<std::byte>& bytes,
    const std::string_view scanId)
{
    if (!path.parent_path().empty())
    {
        std::filesystem::create_directories(path.parent_path());
    }

    std::filesystem::path temporaryPath = path;
    temporaryPath += ".tmp-" + std::string{scanId};
    try
    {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error("Failed to create temporary INPX cover cache file.");
        }

        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!output)
        {
            throw std::runtime_error("Failed to write temporary INPX cover cache file.");
        }

        output.close();
        if (!output)
        {
            throw std::runtime_error("Failed to finalize temporary INPX cover cache file.");
        }
        std::filesystem::rename(temporaryPath, path);
    }
    catch (...)
    {
        RemovePathWithWarningNoThrow(temporaryPath, "temporary INPX cover");
        throw;
    }
}

[[nodiscard]] std::filesystem::path BuildCachedCoverRelativePath(
    const std::filesystem::path& cacheRoot,
    const std::filesystem::path& coverPath)
{
    const auto relativePath = coverPath.lexically_relative(cacheRoot);
    if (!InpxWebReader::Foundation::IsSafeRelativePath(relativePath))
    {
        throw std::runtime_error("Generated INPX cover cache path escaped the cache root.");
    }

    return relativePath;
}

[[nodiscard]] SInpxCachedCoverResult CacheInpxCover(
    const std::filesystem::path& cacheRoot,
    const Inpx::SInpxRecord& inpxRecord,
    const InpxWebReader::Domain::SParsedBook& parsedBook,
    const std::optional<InpxWebReader::ScanSupport::ECoverCacheResolution>& coverStorageResolution,
    const std::string_view scanId,
    std::list<std::filesystem::path>& writtenCoverPaths)
{
    InpxWebReader::Application::CInpxCacheBootstrap::ValidateExistingCacheRoot(cacheRoot);
    const std::string sourceLabel = BuildInpxSourceLabel(inpxRecord);
    if (!parsedBook.CoverExtension.has_value()
        || parsedBook.CoverExtension->empty()
        || parsedBook.CoverBytes.empty())
    {
        SInpxCachedCoverResult result;
        if (parsedBook.CoverDiagnosticMessage.has_value())
        {
            result.WarningCount = 1;
            InpxWebReader::Logging::WarnIfInitialized(
                "cover: INPX not stored source='{}' reason='{}'",
                sourceLabel,
                *parsedBook.CoverDiagnosticMessage);
        }
        else
        {
            InpxWebReader::Logging::DebugIfInitialized(
                "cover: INPX absent source='{}' reason='source-has-no-cover'",
                sourceLabel);
        }
        return result;
    }

    const std::string_view coverBytesView{
        reinterpret_cast<const char*>(parsedBook.CoverBytes.data()),
        parsedBook.CoverBytes.size()};
    const std::string coverFingerprintUtf8 = Foundation::CSha256Fingerprint::ComputeBytes(coverBytesView);
    auto coverPath = InpxWebReader::StoragePlanning::CInpxCacheLayout::GetContentAddressedCoverPath(
        cacheRoot,
        coverFingerprintUtf8,
        parsedBook.CoverExtension.value());
    const std::filesystem::path* storedCoverPath = &coverPath;
    const auto coversDirectory =
        InpxWebReader::StoragePlanning::CInpxCacheLayout::Build(cacheRoot).CoversDirectory;
    const auto relativeToCovers = coverPath.lexically_relative(coversDirectory);
    static_cast<void>(InpxWebReader::SafePaths::TryResolvePathWithinRoot(
        coversDirectory,
        relativeToCovers,
        "Generated INPX cover cache path escaped the Covers directory.",
        "Generated INPX cover cache path could not be canonicalized."));

    const auto relativeCoverPath = BuildCachedCoverRelativePath(cacheRoot, coverPath);
    if (std::filesystem::exists(coverPath))
    {
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(coverPath))
            || !std::filesystem::is_regular_file(coverPath)
            || Foundation::CSha256Fingerprint::ComputeFile(coverPath) != coverFingerprintUtf8)
        {
            throw std::runtime_error("Existing content-addressed INPX cover does not match its SHA-256 path.");
        }
    }
    else
    {
        writtenCoverPaths.push_back(std::move(coverPath));
        storedCoverPath = &writtenCoverPaths.back();
        WriteCoverBytesAtomically(*storedCoverPath, parsedBook.CoverBytes, scanId);
    }

    const auto coverSizeBytes = static_cast<std::uint64_t>(parsedBook.CoverBytes.size());
    InpxWebReader::Logging::DebugIfInitialized(
        "cover: INPX cached source='{}' bookId={} reason='{}' path='{}' bytes={}",
        sourceLabel,
        inpxRecord.LibId,
        coverStorageResolution.has_value()
            ? InpxWebReader::ScanSupport::ToString(*coverStorageResolution)
            : std::string_view{"stored-unprepared"},
        InpxWebReader::Unicode::PathToUtf8(*storedCoverPath),
        coverSizeBytes);

    return {
        .RelativePathUtf8 = InpxWebReader::Unicode::PathToUtf8(relativeCoverPath),
        .Bytes = coverSizeBytes
    };
}

void UpdateBookCoverPath(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::int64_t bookId,
    const std::optional<std::string>& coverPathUtf8)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "UPDATE inpx_books SET cover_path = ? WHERE id = ?;");
    coverPathUtf8.has_value() ? statement.BindText(1, *coverPathUtf8) : statement.BindNull(1);
    statement.BindInt64(2, bookId);
    static_cast<void>(statement.Step());
}

void BindStoredBookMetadata(
    InpxWebReader::Sqlite::CSqliteStatement& statement,
    const InpxWebReader::Domain::SBookMetadata& metadata)
{
    const std::optional<std::string> normalizedIsbn = InpxWebReader::Domain::NormalizeIsbn(metadata.Isbn);
    statement.BindText(1, metadata.TitleUtf8);
    statement.BindText(2, InpxWebReader::Domain::NormalizeText(metadata.TitleUtf8));
    statement.BindText(3, metadata.Language);
    metadata.SeriesUtf8.has_value() ? statement.BindText(4, *metadata.SeriesUtf8) : statement.BindNull(4);
    metadata.SeriesIndex.has_value() ? statement.BindDouble(5, *metadata.SeriesIndex) : statement.BindNull(5);
    metadata.PublisherUtf8.has_value() ? statement.BindText(6, *metadata.PublisherUtf8) : statement.BindNull(6);
    metadata.Year.has_value() ? statement.BindInt(7, *metadata.Year) : statement.BindNull(7);
    normalizedIsbn.has_value() ? statement.BindText(8, *normalizedIsbn) : statement.BindNull(8);
    metadata.DescriptionUtf8.has_value() ? statement.BindText(9, *metadata.DescriptionUtf8) : statement.BindNull(9);
    metadata.Identifier.has_value() ? statement.BindText(10, *metadata.Identifier) : statement.BindNull(10);
}

[[nodiscard]] std::int64_t InsertInpxBook(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const Inpx::SInpxRecord& inpxRecord,
    const InpxWebReader::Domain::SParsedBook& parsedBook,
    const std::chrono::system_clock::time_point addedAtUtc,
    SInpxMetadataFallbackCounters& metadataFallbacks)
{
    const SInpxStoredMetadataResult metadataResult = BuildStoredMetadata(inpxRecord, parsedBook);
    metadataFallbacks = metadataResult.Fallbacks;
    const InpxWebReader::Domain::SBookMetadata& metadata = metadataResult.Metadata;

    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "INSERT INTO inpx_books "
        "(title, normalized_title, language, series, series_index, publisher, year, isbn, description, identifier, cover_path, added_at_utc) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    BindStoredBookMetadata(statement, metadata);
    statement.BindNull(11);
    statement.BindText(12, InpxWebReader::Sqlite::SerializeTimePoint(addedAtUtc));
    static_cast<void>(statement.Step());

    const std::int64_t bookId = connection.GetLastInsertRowId();
    ReplaceBookRelations(connection, bookId, metadata);
    InpxWebReader::SearchIndex::CSearchIndexMaintenance::InsertBook(connection, bookId, metadata);
    return bookId;
}

void UpdateInpxBook(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::int64_t bookId,
    const Inpx::SInpxRecord& inpxRecord,
    const InpxWebReader::Domain::SParsedBook& parsedBook,
    SInpxMetadataFallbackCounters& metadataFallbacks)
{
    const auto oldMetadata = ReadStoredMetadata(connection, bookId);
    if (!oldMetadata.has_value())
    {
        throw std::runtime_error("INPX book row is missing during update.");
    }

    const SInpxStoredMetadataResult metadataResult = BuildStoredMetadata(inpxRecord, parsedBook);
    metadataFallbacks = metadataResult.Fallbacks;
    const InpxWebReader::Domain::SBookMetadata& metadata = metadataResult.Metadata;

    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "UPDATE inpx_books "
        "SET title = ?, normalized_title = ?, language = ?, series = ?, series_index = ?, "
        "    publisher = ?, year = ?, isbn = ?, description = ?, identifier = ? "
        "WHERE id = ?;");
    BindStoredBookMetadata(statement, metadata);
    statement.BindInt64(11, bookId);
    static_cast<void>(statement.Step());

    ReplaceBookRelations(connection, bookId, metadata);
    InpxWebReader::SearchIndex::CSearchIndexMaintenance::RemoveBook(connection, bookId, *oldMetadata);
    InpxWebReader::SearchIndex::CSearchIndexMaintenance::InsertBook(connection, bookId, metadata);
}

void UpsertInpxLocation(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::int64_t sourceId,
    const std::int64_t segmentId,
    const std::int64_t bookId,
    const Inpx::SInpxRecord& inpxRecord,
    const InpxWebReader::Domain::EInpxBookAvailability availability,
    const std::string_view scanId)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "INSERT INTO inpx_book_locations "
        "(book_id, source_id, segment_id, lib_id, archive_name, entry_name, availability, "
        " present_in_segment, file_size_bytes, format, last_seen_scan_id) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, 1, ?, ?, ?) "
        "ON CONFLICT(book_id) DO UPDATE SET "
        "source_id = excluded.source_id, segment_id = excluded.segment_id, lib_id = excluded.lib_id, archive_name = excluded.archive_name, "
        "entry_name = excluded.entry_name, availability = excluded.availability, present_in_segment = 1, file_size_bytes = excluded.file_size_bytes, "
        "format = excluded.format, last_seen_scan_id = excluded.last_seen_scan_id;");
    statement.BindInt64(1, bookId);
    statement.BindInt64(2, sourceId);
    statement.BindInt64(3, segmentId);
    statement.BindText(4, inpxRecord.LibId);
    statement.BindText(5, inpxRecord.ArchiveNameUtf8);
    statement.BindText(6, inpxRecord.EntryNameUtf8);
    statement.BindText(7, InpxWebReader::Domain::ToString(availability));
    statement.BindInt64(8, static_cast<std::int64_t>(inpxRecord.FileSizeBytes));
    statement.BindText(9, Foundation::ToLowerAscii(inpxRecord.FileExtensionUtf8));
    statement.BindText(10, scanId);
    static_cast<void>(statement.Step());
}

void InsertPersistedWarning(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::int64_t sourceId,
    const std::string& scanId,
    const std::string& createdAtUtc,
    const SInpxScanWarningRecord& warning)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "INSERT INTO inpx_scan_warnings "
        "(source_id, scan_id, warning_code, archive_name, entry_name, message, created_at_utc) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);");
    statement.BindInt64(1, sourceId);
    statement.BindText(2, scanId);
    statement.BindText(3, warning.Code);
    warning.ArchiveNameUtf8.has_value() ? statement.BindText(4, *warning.ArchiveNameUtf8) : statement.BindNull(4);
    warning.EntryNameUtf8.has_value() ? statement.BindText(5, *warning.EntryNameUtf8) : statement.BindNull(5);
    statement.BindText(6, warning.MessageUtf8);
    statement.BindText(7, createdAtUtc);
    static_cast<void>(statement.Step());
}

void UpdateInpxSourceRow(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::int64_t sourceId,
    const InpxWebReader::Application::SInpxSourceInfo& source,
    const std::string& sourceFingerprintUtf8,
    const std::string& startedAtUtc,
    const std::string& completedAtUtc,
    const std::string& scanId)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "UPDATE inpx_sources "
        "SET display_name = ?, source_fingerprint = ?, last_scan_started_at_utc = ?, "
        "    last_scan_completed_at_utc = ?, last_seen_snapshot_id = ? "
        "WHERE id = ?;");
    statement.BindText(1, InpxWebReader::Unicode::PathToUtf8(source.InpxPath.filename()));
    statement.BindText(2, sourceFingerprintUtf8);
    statement.BindText(3, startedAtUtc);
    statement.BindText(4, completedAtUtc);
    statement.BindText(5, scanId);
    statement.BindInt64(6, sourceId);
    static_cast<void>(statement.Step());
}

void MarkInpxSourceScanStarted(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::int64_t sourceId,
    const std::string& startedAtUtc)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "UPDATE inpx_sources "
        "SET last_scan_started_at_utc = ?, last_scan_completed_at_utc = NULL "
        "WHERE id = ?;");
    statement.BindText(1, startedAtUtc);
    statement.BindInt64(2, sourceId);
    static_cast<void>(statement.Step());
}

enum class EInpxSegmentChange
{
    Unchanged,
    Added,
    Changed,
    Removed
};

struct SExistingInpxSegment
{
    std::int64_t Id = 0;
    std::string InpEntryNameUtf8;
    std::string ArchiveNameUtf8;
    std::string InpFingerprintUtf8;
    std::int64_t SegmentOrder = 0;
    Inpx::SInpxParseSummary Summary;
    std::string AvailabilityUtf8;
    bool RequiresArchive = false;
    std::optional<std::string> ResolvedArchivePathUtf8 = std::nullopt;
    InpxWebReader::Application::SInpxArchiveFileState ArchiveFileState;
    std::optional<std::string> ArchiveManifestFingerprintUtf8 = std::nullopt;
};

struct SInpxSegmentPlan
{
    EInpxSegmentChange Change = EInpxSegmentChange::Added;
    Inpx::SInpxSegmentPayload Payload;
    Inpx::SInpxParseSummary Summary;
    std::vector<Inpx::SInpxRecord> Records;
    std::vector<Inpx::SInpxWarning> Warnings;
    std::set<std::string> DeletedLibIds;
    std::int64_t SegmentOrder = 0;
    bool RequiresArchive = false;
    std::string ResolvedArchivePathUtf8;
    InpxWebReader::Application::SInpxArchiveFileState ArchiveFileState;
    std::string ArchiveManifestFingerprintUtf8;
    std::optional<std::int64_t> ExistingId = std::nullopt;
    bool ArchiveOpened = false;
};

enum class EInpxArchiveGuardPhase
{
    FileStateRead,
    ManifestRead,
    PayloadsValidated
};

struct SInpxArchiveScanGuard
{
    std::string ArchiveNameUtf8;
    std::string ResolvedArchivePathUtf8;
    InpxWebReader::Application::SInpxArchiveFileState FileState;
    std::string ManifestFingerprintUtf8;
    EInpxArchiveGuardPhase Phase = EInpxArchiveGuardPhase::FileStateRead;
};

using TExistingInpxSegments = std::map<std::string, SExistingInpxSegment>;
using TInpxArchiveScanGuards = std::map<std::string, SInpxArchiveScanGuard>;

[[nodiscard]] TExistingInpxSegments ReadExistingInpxSegments(
    const std::filesystem::path& databasePath,
    const std::size_t maxSegments)
{
    TExistingInpxSegments result;
    if (databasePath.empty())
    {
        return result;
    }

    const InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT id, inp_entry_name, archive_name, inp_fingerprint, segment_order, record_count, "
        "       active_record_count, deleted_record_count, availability, requires_archive, "
        "       resolved_archive_path, archive_file_size_bytes, archive_mtime_ticks, archive_manifest_fingerprint "
        "FROM inpx_segments WHERE source_id = ?;");
    statement.BindInt64(1, ReadInpxSourceId(connection));
    while (statement.Step())
    {
        RequireCollectionRoom(result.size(), maxSegments, "segment count");
        SExistingInpxSegment segment{
            .Id = statement.GetColumnInt64(0),
            .InpEntryNameUtf8 = statement.GetColumnText(1),
            .ArchiveNameUtf8 = statement.GetColumnText(2),
            .InpFingerprintUtf8 = statement.GetColumnText(3),
            .SegmentOrder = statement.GetColumnInt64(4),
            .Summary = {
                .InpEntryCount = 1,
                .TotalRecords = static_cast<std::size_t>(statement.GetColumnInt64(5)),
                .ParsedRecords = static_cast<std::size_t>(statement.GetColumnInt64(6))
                    + static_cast<std::size_t>(statement.GetColumnInt64(7)),
                .DeletedRecords = static_cast<std::size_t>(statement.GetColumnInt64(7))
            },
            .AvailabilityUtf8 = statement.GetColumnText(8),
            .RequiresArchive = statement.GetColumnInt(9) != 0,
            .ResolvedArchivePathUtf8 = statement.IsColumnNull(10)
                ? std::nullopt
                : std::make_optional(statement.GetColumnText(10)),
            .ArchiveFileState = {
                .FileSizeBytes = static_cast<std::uint64_t>(statement.GetColumnInt64(11)),
                .MtimeTicks = statement.GetColumnInt64(12)
            },
            .ArchiveManifestFingerprintUtf8 = statement.IsColumnNull(13)
                ? std::nullopt
                : std::make_optional(statement.GetColumnText(13))
        };
        const auto [_, inserted] = result.emplace(segment.InpEntryNameUtf8, std::move(segment));
        if (!inserted)
        {
            throw std::runtime_error("Duplicate persisted INPX segment name.");
        }
    }
    return result;
}

[[nodiscard]] std::set<std::string> ReadActiveInpxIdsForSegments(
    const std::filesystem::path& databasePath,
    const std::set<std::string>& segmentNamesUtf8,
    const std::size_t maxRecords)
{
    std::set<std::string> result;
    if (databasePath.empty() || segmentNamesUtf8.empty())
    {
        return result;
    }

    const InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT l.lib_id, s.inp_entry_name "
        "FROM inpx_book_locations l "
        "INNER JOIN inpx_segments s ON s.id = l.segment_id "
        "WHERE l.present_in_segment = 1;");
    while (statement.Step())
    {
        if (segmentNamesUtf8.contains(statement.GetColumnText(1)))
        {
            static_cast<void>(InsertActiveLibId(result, statement.GetColumnText(0), maxRecords));
        }
    }
    return result;
}

[[nodiscard]] std::size_t ReadChangedRowCount(
    const InpxWebReader::Sqlite::CSqliteConnection& connection)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT changes();");
    return statement.Step() ? static_cast<std::size_t>(statement.GetColumnInt64(0)) : 0;
}

class CInpxCatalogStreamWriter final
{
public:
    CInpxCatalogStreamWriter(
        const std::filesystem::path& databasePath,
        std::filesystem::path cacheRoot,
        const std::chrono::system_clock::time_point startedAtUtc,
        const std::function<void()>& throwIfCancelled,
        const std::uint64_t maxCoverCacheBytes)
        : m_databasePath(databasePath)
        , m_connection(databasePath)
        , m_cacheRoot(std::move(cacheRoot))
        , m_scanId(MakeScanId())
        , m_startedAtText(InpxWebReader::Sqlite::SerializeTimePoint(startedAtUtc))
        , m_maxCoverCacheBytes(maxCoverCacheBytes)
    {
        m_connection.Execute("PRAGMA wal_autocheckpoint = 4000;");
        m_connection.Execute("PRAGMA busy_timeout = 50;");
        m_sourceId = ReadInpxSourceId(m_connection);
        for (;;)
        {
            throwIfCancelled();
            try
            {
                m_transaction = std::make_unique<InpxWebReader::Sqlite::CSqliteTransaction>(m_connection);
                break;
            }
            catch (const std::runtime_error& exception)
            {
                if (std::string_view{exception.what()}.find("database is locked") == std::string_view::npos)
                {
                    throw;
                }
                throwIfCancelled();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        MarkInpxSourceScanStarted(m_connection, m_sourceId, m_startedAtText);
    }

    ~CInpxCatalogStreamWriter()
    {
        if (m_committed)
        {
            return;
        }

        for (const auto& path : m_writtenCoverPaths)
        {
            RemovePathWithWarningNoThrow(path, "rollback INPX cover");
        }
    }

    template <typename TThrowIfCancelled>
    [[nodiscard]] std::int64_t UpsertSegment(
        const SInpxSegmentPlan& plan,
        TThrowIfCancelled&& throwIfCancelled)
    {
        const bool removed = plan.Change == EInpxSegmentChange::Removed;
        InpxWebReader::Sqlite::CSqliteStatement statement(
            m_connection.GetNativeHandle(),
            "INSERT INTO inpx_segments "
            "(source_id, inp_entry_name, archive_name, inp_fingerprint, segment_order, record_count, "
            " active_record_count, deleted_record_count, availability, requires_archive, resolved_archive_path, archive_file_size_bytes, "
            " archive_mtime_ticks, archive_manifest_fingerprint, last_seen_scan_id) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(source_id, inp_entry_name) DO UPDATE SET "
            "archive_name = excluded.archive_name, inp_fingerprint = excluded.inp_fingerprint, "
            "segment_order = excluded.segment_order, record_count = excluded.record_count, "
            "active_record_count = excluded.active_record_count, "
            "deleted_record_count = excluded.deleted_record_count, availability = excluded.availability, "
            "requires_archive = excluded.requires_archive, "
            "resolved_archive_path = excluded.resolved_archive_path, "
            "archive_file_size_bytes = excluded.archive_file_size_bytes, archive_mtime_ticks = excluded.archive_mtime_ticks, "
            "archive_manifest_fingerprint = excluded.archive_manifest_fingerprint, last_seen_scan_id = excluded.last_seen_scan_id;");
        statement.BindInt64(1, m_sourceId);
        statement.BindText(2, plan.Payload.InpEntryNameUtf8);
        statement.BindText(3, plan.Payload.ArchiveNameUtf8);
        statement.BindText(4, plan.Payload.FingerprintUtf8);
        statement.BindInt64(5, plan.SegmentOrder);
        statement.BindInt64(6, static_cast<std::int64_t>(plan.Summary.TotalRecords));
        statement.BindInt64(
            7,
            static_cast<std::int64_t>(plan.Summary.ParsedRecords - plan.Summary.DeletedRecords));
        statement.BindInt64(8, static_cast<std::int64_t>(plan.Summary.DeletedRecords));
        statement.BindText(
            9,
            InpxWebReader::Domain::ToString(
                removed
                    ? InpxWebReader::Domain::EInpxBookAvailability::MissingFromIndex
                    : InpxWebReader::Domain::EInpxBookAvailability::Available));
        statement.BindInt(10, plan.RequiresArchive ? 1 : 0);
        plan.ResolvedArchivePathUtf8.empty()
            ? statement.BindNull(11)
            : statement.BindText(11, plan.ResolvedArchivePathUtf8);
        statement.BindInt64(12, static_cast<std::int64_t>(plan.ArchiveFileState.FileSizeBytes));
        statement.BindInt64(13, plan.ArchiveFileState.MtimeTicks);
        plan.ArchiveManifestFingerprintUtf8.empty()
            ? statement.BindNull(14)
            : statement.BindText(14, plan.ArchiveManifestFingerprintUtf8);
        statement.BindText(15, m_scanId);
        static_cast<void>(statement.Step());

        InpxWebReader::Sqlite::CSqliteStatement idStatement(
            m_connection.GetNativeHandle(),
            "SELECT id FROM inpx_segments WHERE source_id = ? AND inp_entry_name = ?;");
        idStatement.BindInt64(1, m_sourceId);
        idStatement.BindText(2, plan.Payload.InpEntryNameUtf8);
        if (!idStatement.Step())
        {
            throw std::runtime_error("INPX segment row is missing after upsert.");
        }
        const std::int64_t segmentId = idStatement.GetColumnInt64(0);
        if (plan.Change == EInpxSegmentChange::Unchanged)
        {
            return segmentId;
        }

        InpxWebReader::Sqlite::CSqliteStatement deleteStatement(
            m_connection.GetNativeHandle(),
            "DELETE FROM inpx_deletions WHERE segment_id = ?;");
        deleteStatement.BindInt64(1, segmentId);
        static_cast<void>(deleteStatement.Step());
        if (plan.Change == EInpxSegmentChange::Removed)
        {
            return segmentId;
        }

        InpxWebReader::Sqlite::CSqliteStatement insertStatement(
            m_connection.GetNativeHandle(),
            "INSERT INTO inpx_deletions(segment_id, lib_id) VALUES (?, ?);");
        for (const auto& libId : plan.DeletedLibIds)
        {
            throwIfCancelled();
            insertStatement.Reset();
            insertStatement.BindInt64(1, segmentId);
            insertStatement.BindText(2, libId);
            static_cast<void>(insertStatement.Step());
        }
        return segmentId;
    }

    [[nodiscard]] SInpxRecordWriteResult WriteParsedRecord(
        const std::int64_t segmentId,
        const Inpx::SInpxRecord& inpxRecord,
        const InpxWebReader::Domain::SParsedBook& parsedBook,
        const std::optional<InpxWebReader::ScanSupport::ECoverCacheResolution>& coverStorageResolution,
        const std::size_t coverPreparationWarningCount,
        InpxWebReader::ScanSupport::CScanPerfTracker& perf)
    {
        std::optional<SExistingInpxBook> existingBook = std::nullopt;
        {
            auto timer = perf.MeasureStage(InpxWebReader::ScanSupport::CScanPerfTracker::EStage::DbWriteWait);
            existingBook = ReadExistingInpxBook(m_connection, m_sourceId, inpxRecord.LibId);
        }
        const auto addedAtUtc = std::chrono::system_clock::now();
        SInpxRecordWriteResult result;
        result.ParserWarningCount = parsedBook.ParserWarningCount;
        result.CoverWarningCount = coverPreparationWarningCount;

        if (!existingBook.has_value())
        {
            SInpxMetadataFallbackCounters metadataFallbacks;
            const std::int64_t bookId = InsertInpxBook(
                m_connection,
                inpxRecord,
                parsedBook,
                addedAtUtc,
                metadataFallbacks);
            const auto coverResult = CacheInpxCover(
                m_cacheRoot,
                inpxRecord,
                parsedBook,
                coverStorageResolution,
                m_scanId,
                m_writtenCoverPaths);
            UpdateBookCoverPath(m_connection, bookId, coverResult.RelativePathUtf8);
            UpsertInpxLocation(
                m_connection,
                m_sourceId,
                segmentId,
                bookId,
                inpxRecord,
                InpxWebReader::Domain::EInpxBookAvailability::Available,
                m_scanId);
            InpxWebReader::BookDatabase::CCatalogStatisticsMaintenance::RecordInpxBookInserted(
                m_connection,
                m_databasePath,
                coverResult.RelativePathUtf8,
                coverResult.RelativePathUtf8.has_value()
                    ? std::make_optional(coverResult.Bytes)
                    : std::nullopt);
            ValidateCoverCacheBudget();
            result.Added = true;
            result.CachedCover = coverResult.RelativePathUtf8.has_value();
            result.CachedCoverBytes = coverResult.Bytes;
            result.CoverWarningCount += coverResult.WarningCount;
            result.MetadataFallbacks = metadataFallbacks;
            return result;
        }

        SInpxMetadataFallbackCounters metadataFallbacks;
        UpdateInpxBook(m_connection, existingBook->BookId, inpxRecord, parsedBook, metadataFallbacks);
        const auto coverResult = CacheInpxCover(
            m_cacheRoot,
            inpxRecord,
            parsedBook,
            coverStorageResolution,
            m_scanId,
            m_writtenCoverPaths);
        UpdateBookCoverPath(m_connection, existingBook->BookId, coverResult.RelativePathUtf8);
        UpsertInpxLocation(
            m_connection,
            m_sourceId,
            segmentId,
            existingBook->BookId,
            inpxRecord,
            InpxWebReader::Domain::EInpxBookAvailability::Available,
            m_scanId);
        InpxWebReader::BookDatabase::CCatalogStatisticsMaintenance::RecordCoverPathChanged(
            m_connection,
            m_databasePath,
            existingBook->CoverPathUtf8,
            coverResult.RelativePathUtf8,
            coverResult.RelativePathUtf8.has_value()
                ? std::make_optional(coverResult.Bytes)
                : std::nullopt);
        InpxWebReader::BookDatabase::CCatalogStatisticsMaintenance::RecordInpxBookAvailabilityChanged(
            m_connection,
            existingBook->AvailabilityUtf8,
            InpxWebReader::Domain::ToString(InpxWebReader::Domain::EInpxBookAvailability::Available));
        ValidateCoverCacheBudget();
        result.Updated = true;
        result.CachedCover = coverResult.RelativePathUtf8.has_value();
        result.CachedCoverBytes = coverResult.Bytes;
        result.CoverWarningCount += coverResult.WarningCount;
        result.MetadataFallbacks = metadataFallbacks;
        return result;
    }

    void WriteWarning(const SInpxScanWarningRecord& warning)
    {
        constexpr std::size_t GMaxPersistedWarningsPerScan = 1'000;
        if (m_persistedWarningCount >= GMaxPersistedWarningsPerScan)
        {
            return;
        }

        InsertPersistedWarning(
            m_connection,
            m_sourceId,
            m_scanId,
            InpxWebReader::Sqlite::SerializeTimePoint(std::chrono::system_clock::now()),
            warning);
        ++m_persistedWarningCount;
    }

    template <typename TThrowIfCancelled>
    [[nodiscard]] SInpxCatalogApplyResult Finish(
        const InpxWebReader::Application::SInpxSourceInfo& source,
        const std::string& sourceFingerprintUtf8,
        const std::vector<SInpxSegmentPlan>& plans,
        const TInpxArchiveScanGuards& archiveGuards,
        const std::uint64_t maxArchiveManifestMemoryBytes,
        const std::vector<std::int64_t>& changedSegmentIds,
        const CInpxScanJobService::SHooks& hooks,
        TThrowIfCancelled&& throwIfCancelled)
    {
        for (const std::int64_t segmentId : changedSegmentIds)
        {
            throwIfCancelled();
            InpxWebReader::Sqlite::CSqliteStatement statement(
                m_connection.GetNativeHandle(),
                "UPDATE inpx_book_locations "
                "SET present_in_segment = 0, last_seen_scan_id = ? "
                "WHERE segment_id = ? AND present_in_segment = 1 "
                "  AND (last_seen_scan_id IS NULL OR last_seen_scan_id <> ?);");
            statement.BindText(1, m_scanId);
            statement.BindInt64(2, segmentId);
            statement.BindText(3, m_scanId);
            static_cast<void>(statement.Step());
        }

        throwIfCancelled();
        InpxWebReader::Sqlite::CSqliteStatement markUnavailableStatement(
            m_connection.GetNativeHandle(),
            "UPDATE inpx_book_locations "
            "SET availability = ?, last_seen_scan_id = ? "
            "WHERE source_id = ? AND availability = ? AND ("
            "  present_in_segment = 0 "
            "  OR NOT EXISTS ("
            "    SELECT 1 FROM inpx_segments active_segment "
            "    WHERE active_segment.id = inpx_book_locations.segment_id "
            "      AND active_segment.availability = ?"
            "  ) "
            "  OR EXISTS ("
            "    SELECT 1 "
            "    FROM inpx_deletions deletion "
            "    INNER JOIN inpx_segments deletion_segment ON deletion_segment.id = deletion.segment_id "
            "    INNER JOIN inpx_segments active_segment ON active_segment.id = inpx_book_locations.segment_id "
            "    WHERE deletion.lib_id = inpx_book_locations.lib_id "
            "      AND deletion_segment.source_id = inpx_book_locations.source_id "
            "      AND deletion_segment.availability = ? "
            "      AND deletion_segment.segment_order > active_segment.segment_order"
            "  )"
            ");");
        const auto available = InpxWebReader::Domain::ToString(
            InpxWebReader::Domain::EInpxBookAvailability::Available);
        const auto missingFromIndex = InpxWebReader::Domain::ToString(
            InpxWebReader::Domain::EInpxBookAvailability::MissingFromIndex);
        markUnavailableStatement.BindText(1, missingFromIndex);
        markUnavailableStatement.BindText(2, m_scanId);
        markUnavailableStatement.BindInt64(3, m_sourceId);
        markUnavailableStatement.BindText(4, available);
        markUnavailableStatement.BindText(5, available);
        markUnavailableStatement.BindText(6, available);
        static_cast<void>(markUnavailableStatement.Step());
        const std::size_t markedUnavailable = ReadChangedRowCount(m_connection);

        throwIfCancelled();
        InpxWebReader::Sqlite::CSqliteStatement restoreAvailableStatement(
            m_connection.GetNativeHandle(),
            "UPDATE inpx_book_locations "
            "SET availability = ?, last_seen_scan_id = ? "
            "WHERE source_id = ? AND availability = ? "
            "  AND present_in_segment = 1 "
            "  AND EXISTS ("
            "    SELECT 1 FROM inpx_segments active_segment "
            "    WHERE active_segment.id = inpx_book_locations.segment_id "
            "      AND active_segment.availability = ?"
            "  ) "
            "  AND NOT EXISTS ("
            "    SELECT 1 "
            "    FROM inpx_deletions deletion "
            "    INNER JOIN inpx_segments deletion_segment ON deletion_segment.id = deletion.segment_id "
            "    INNER JOIN inpx_segments active_segment ON active_segment.id = inpx_book_locations.segment_id "
            "    WHERE deletion.lib_id = inpx_book_locations.lib_id "
            "      AND deletion_segment.source_id = inpx_book_locations.source_id "
            "      AND deletion_segment.availability = ? "
            "      AND deletion_segment.segment_order > active_segment.segment_order"
            "  );");
        restoreAvailableStatement.BindText(1, available);
        restoreAvailableStatement.BindText(2, m_scanId);
        restoreAvailableStatement.BindInt64(3, m_sourceId);
        restoreAvailableStatement.BindText(4, missingFromIndex);
        restoreAvailableStatement.BindText(5, available);
        restoreAvailableStatement.BindText(6, available);
        static_cast<void>(restoreAvailableStatement.Step());
        const std::size_t restoredAvailable = ReadChangedRowCount(m_connection);

        InpxWebReader::BookDatabase::CCatalogStatisticsMaintenance::AddUnavailableBooks(
            m_connection,
            static_cast<std::uint64_t>(markedUnavailable));
        InpxWebReader::BookDatabase::CCatalogStatisticsMaintenance::RemoveUnavailableBooks(
            m_connection,
            static_cast<std::uint64_t>(restoredAvailable));

        const auto completedAtUtc = std::chrono::system_clock::now();
        UpdateInpxSourceRow(
            m_connection,
            m_sourceId,
            source,
            sourceFingerprintUtf8,
            m_startedAtText,
            InpxWebReader::Sqlite::SerializeTimePoint(completedAtUtc),
            m_scanId);
        InpxWebReader::BookDatabase::CCatalogStatisticsMaintenance::RecomputeInpxSourceSize(
            m_connection,
            source.InpxPath,
            source.ArchiveRoot);
        PrunePersistedWarnings();
        throwIfCancelled();
        if (hooks.BeforeCatalogCommit)
        {
            hooks.BeforeCatalogCommit();
        }
        throwIfCancelled();
        ValidateArchiveGuards(
            source,
            plans,
            archiveGuards,
            maxArchiveManifestMemoryBytes,
            throwIfCancelled);
        throwIfCancelled();
        if (Foundation::CSha256Fingerprint::ComputeFile(source.InpxPath, throwIfCancelled)
            != sourceFingerprintUtf8)
        {
            throw std::runtime_error("INPX source changed while it was being scanned; retry the scan.");
        }
        throwIfCancelled();
        m_transaction->Commit();
        m_committed = true;
        CleanupUnreferencedCovers();

        return {
            .MarkedUnavailableRecords = markedUnavailable
        };
    }

private:
    void ValidateCoverCacheBudget() const
    {
        InpxWebReader::Sqlite::CSqliteStatement statement(
            m_connection.GetNativeHandle(),
            "SELECT cover_cache_size_bytes FROM catalog_statistics WHERE singleton = 1;");
        if (!statement.Step())
        {
            throw std::runtime_error("Catalog statistics singleton is missing during INPX scan.");
        }
        const std::int64_t coverCacheSizeBytes = statement.GetColumnInt64(0);
        if (coverCacheSizeBytes < 0
            || static_cast<std::uint64_t>(coverCacheSizeBytes) > m_maxCoverCacheBytes)
        {
            throw std::runtime_error(
                "INPX cover cache budget exceeded: projected referenced covers require "
                + std::to_string(coverCacheSizeBytes) + " bytes, limit is "
                + std::to_string(m_maxCoverCacheBytes) + " bytes.");
        }
    }

    template <typename TThrowIfCancelled>
    static void ValidateArchiveGuards(
        const InpxWebReader::Application::SInpxSourceInfo& source,
        const std::vector<SInpxSegmentPlan>& plans,
        const TInpxArchiveScanGuards& archiveGuards,
        const std::uint64_t maxArchiveManifestMemoryBytes,
        TThrowIfCancelled&& throwIfCancelled)
    {
        std::map<std::string_view, std::string_view> stateOnlyManifestExpectations;
        for (const auto& plan : plans)
        {
            throwIfCancelled();
            if (plan.Change == EInpxSegmentChange::Removed || !plan.RequiresArchive)
            {
                continue;
            }

            const auto guardIterator = archiveGuards.find(plan.ResolvedArchivePathUtf8);
            if (guardIterator == archiveGuards.end())
            {
                throw std::runtime_error(
                    "INPX archive guard is missing for segment '"
                    + plan.Payload.InpEntryNameUtf8 + "'.");
            }

            const auto& guard = guardIterator->second;
            if (plan.ArchiveFileState.FileSizeBytes != guard.FileState.FileSizeBytes
                || plan.ArchiveFileState.MtimeTicks != guard.FileState.MtimeTicks
                || plan.ArchiveManifestFingerprintUtf8.empty())
            {
                throw std::runtime_error(
                    "Conflicting cached archive guard for INP segment '"
                    + plan.Payload.InpEntryNameUtf8 + "'.");
            }

            if (guard.Phase != EInpxArchiveGuardPhase::FileStateRead)
            {
                if (plan.ArchiveManifestFingerprintUtf8 != guard.ManifestFingerprintUtf8)
                {
                    throw std::runtime_error(
                        "Conflicting cached archive manifest for INP segment '"
                        + plan.Payload.InpEntryNameUtf8 + "'.");
                }
                continue;
            }

            auto expectationIterator = stateOnlyManifestExpectations.find(plan.ResolvedArchivePathUtf8);
            if (expectationIterator == stateOnlyManifestExpectations.end())
            {
                expectationIterator = stateOnlyManifestExpectations.emplace(
                    std::string_view{plan.ResolvedArchivePathUtf8},
                    std::string_view{plan.ArchiveManifestFingerprintUtf8}).first;
            }
            else if (expectationIterator->second != plan.ArchiveManifestFingerprintUtf8)
            {
                throw std::runtime_error(
                    "Conflicting cached archive manifests for resolved archive '"
                    + plan.ResolvedArchivePathUtf8 + "'.");
            }
        }

        for (const auto& [resolvedArchivePathUtf8, guard] : archiveGuards)
        {
            throwIfCancelled();
            const auto archivePath = InpxWebReader::Application::ResolvePersistedInpxArchivePath(
                source,
                resolvedArchivePathUtf8);
            const auto currentState = InpxWebReader::Application::ReadInpxArchiveFileState(archivePath);
            if (currentState.FileSizeBytes != guard.FileState.FileSizeBytes
                || currentState.MtimeTicks != guard.FileState.MtimeTicks)
            {
                throw std::runtime_error(
                    "INPX archive '" + guard.ArchiveNameUtf8
                    + "' changed while it was being scanned; retry the scan.");
            }

            if (guard.Phase == EInpxArchiveGuardPhase::FileStateRead)
            {
                continue;
            }

            const InpxWebReader::Application::CInpxArchiveReader reader(archivePath);
            if (reader.ComputeManifestFingerprint(maxArchiveManifestMemoryBytes, throwIfCancelled)
                != guard.ManifestFingerprintUtf8)
            {
                throw std::runtime_error(
                    "INPX archive '" + guard.ArchiveNameUtf8
                    + "' changed while it was being scanned; retry the scan.");
            }
        }
    }

    void PrunePersistedWarnings()
    {
        InpxWebReader::Sqlite::CSqliteStatement statement(
            m_connection.GetNativeHandle(),
            "DELETE FROM inpx_scan_warnings "
            "WHERE source_id = ? AND scan_id NOT IN ("
            "  SELECT scan_id FROM inpx_scan_warnings WHERE source_id = ? "
            "  GROUP BY scan_id ORDER BY MAX(id) DESC LIMIT 10"
            ");");
        statement.BindInt64(1, m_sourceId);
        statement.BindInt64(2, m_sourceId);
        static_cast<void>(statement.Step());
    }

    [[nodiscard]] bool IsCoverReferenced(const std::filesystem::path& path) const
    {
        const auto relativePath = BuildCachedCoverRelativePath(m_cacheRoot, path);
        InpxWebReader::Sqlite::CSqliteStatement statement(
            m_connection.GetNativeHandle(),
            "SELECT EXISTS(SELECT 1 FROM inpx_books WHERE cover_path = ?);");
        statement.BindText(1, InpxWebReader::Unicode::PathToUtf8(relativePath));
        return statement.Step() && statement.GetColumnInt(0) != 0;
    }

    void CleanupUnreferencedCovers() noexcept
    {
        try
        {
            InpxWebReader::Application::CInpxCacheBootstrap::ValidateExistingCacheRoot(m_cacheRoot);
            const auto coversDirectory =
                InpxWebReader::StoragePlanning::CInpxCacheLayout::Build(m_cacheRoot).CoversDirectory;
            std::error_code error;
            if (!std::filesystem::exists(coversDirectory, error) || error)
            {
                return;
            }
            for (const auto& entry : std::filesystem::recursive_directory_iterator(coversDirectory))
            {
                if (entry.is_regular_file()
                    && InpxWebReader::StoragePlanning::CInpxCacheLayout::IsContentAddressedCoverPath(
                        m_cacheRoot,
                        entry.path())
                    && !IsCoverReferenced(entry.path()))
                {
                    RemovePathWithWarningNoThrow(entry.path(), "unreferenced INPX cover");
                }
            }
        }
        catch (const std::exception& exception)
        {
            InpxWebReader::Logging::WarnIfInitialized(
                "INPX cover garbage collection failed. error='{}'",
                exception.what());
        }
    }

    std::filesystem::path m_databasePath;
    InpxWebReader::Sqlite::CSqliteConnection m_connection;
    std::filesystem::path m_cacheRoot;
    std::int64_t m_sourceId = 0;
    std::string m_scanId;
    std::string m_startedAtText;
    std::unique_ptr<InpxWebReader::Sqlite::CSqliteTransaction> m_transaction;
    std::list<std::filesystem::path> m_writtenCoverPaths;
    std::size_t m_persistedWarningCount = 0;
    std::uint64_t m_maxCoverCacheBytes = 0;
    bool m_committed = false;
};

void PublishSnapshot(
    const std::shared_ptr<CInpxScanJobService::SJobRecord>& record,
    const std::function<void(SInpxScanJobSnapshot&)>& update)
{
    {
        const std::scoped_lock lock(record->Mutex);
        update(record->Snapshot);
    }

    record->Condition.notify_all();
}

[[nodiscard]] SInpxScanJobSnapshot CopySnapshot(
    const std::shared_ptr<CInpxScanJobService::SJobRecord>& record)
{
    const std::scoped_lock lock(record->Mutex);
    return record->Snapshot;
}


[[nodiscard]] SInpxScanJobResult RunScan(
    const InpxWebReader::Application::SInpxSourceInfo& source,
    const SInpxScanRequest& request,
    const std::optional<std::string>& expectedSourceFingerprintUtf8,
    const std::filesystem::path& workingDirectory,
    const std::filesystem::path& databasePath,
    const std::filesystem::path& cacheRoot,
    const std::size_t maxWorkerCount,
    const std::uint64_t maxCoverCacheBytes,
    const std::uint64_t maxSteadyStateMemoryBytes,
    std::stop_token stopToken,
    const std::shared_ptr<CInpxScanJobService::SJobRecord>& record,
    const CInpxScanJobService::SHooks& hooks,
    const InpxWebReader::Domain::ICoverImageProcessor* coverImageProcessor)
{
    const auto modeLabel = BuildModeLabel(request.Mode);
    const auto scanStartedAtUtc = std::chrono::system_clock::now();
    const auto perfStartedAt = std::chrono::steady_clock::now();
    const TInpxScanJobId jobId = CopySnapshot(record).JobId;
    const std::size_t maxInFlightPayloads = ResolveInpxPayloadWorkerWindow(maxWorkerCount);
    const std::uint64_t maxInFlightPayloadBytes = ResolveInFlightPayloadBudget(
        maxSteadyStateMemoryBytes);
    const std::uint64_t maxArchiveManifestMemoryBytes = ResolveArchiveManifestBudget(
        maxSteadyStateMemoryBytes);
    const std::uint64_t maxPlanningStateBytes = ResolvePlanningStateBudget(
        maxSteadyStateMemoryBytes);
    const SScanPlanningLimits planningLimits = ResolveScanPlanningLimits(maxPlanningStateBytes);
    const std::uint64_t maxDecodedCoverBytes = ResolveCoverDecodeBudget(
        maxSteadyStateMemoryBytes,
        maxInFlightPayloads);
    InpxWebReader::ScanSupport::CScanPerfTracker perf(
        jobId,
        {.Added = "added", .Updated = "updated", .Failed = "failed"});
    InpxWebReader::ScanSupport::CScanPerfSummaryScope perfSummary(perf, perfStartedAt);
    InpxWebReader::ScanSupport::CScanArchivePerfScope archivePerf(perf, jobId);
    BS::thread_pool<> payloadWorkerPool(maxInFlightPayloads);
    std::deque<SInFlightParsedRecord> inFlightParsedRecords;
    std::size_t scanWarningCount = 0;
    SInpxCatalogApplyResult applyResult;

    InpxWebReader::Logging::InfoIfInitialized(
        "INPX {} started. jobId={} source='{}' archiveRoot='{}' payloadWorkers={} "
        "planningInpLimitBytes={} planningRecordLimit={} planningSegmentLimit={} "
        "archiveFallbackEntryLimit={} archiveManifestBudgetBytes={} "
        "inFlightPayloadBudgetBytes={} coverDecodeBudgetBytes={} "
        "coverCacheBudgetBytes={}",
        modeLabel,
        jobId,
        InpxWebReader::Unicode::PathToUtf8(source.InpxPath),
        InpxWebReader::Unicode::PathToUtf8(source.ArchiveRoot),
        maxInFlightPayloads,
        planningLimits.MaxInpSourceBytes,
        planningLimits.MaxRecords,
        planningLimits.MaxSegments,
        planningLimits.MaxFilesystemEntries,
        maxArchiveManifestMemoryBytes,
        maxInFlightPayloadBytes,
        maxDecodedCoverBytes,
        maxCoverCacheBytes);

    const auto throwIfCancelled = [&]() {
        if (!stopToken.stop_requested())
        {
            return;
        }
        PublishSnapshot(record, [&](SInpxScanJobSnapshot& snapshot) {
            snapshot.Status = EInpxScanJobStatus::Cancelling;
            snapshot.Message = "Cancelling INPX " + modeLabel + '.';
        });
        throw CInpxScanCancelled();
    };
    const auto sourceFingerprintCheckpoint = [&]() {
        if (hooks.OnSourceFingerprintCheckpoint)
        {
            hooks.OnSourceFingerprintCheckpoint();
        }
        throwIfCancelled();
    };
    const auto inpParserCheckpoint = [&]() {
        if (hooks.OnInpParserCheckpoint)
        {
            hooks.OnInpParserCheckpoint();
        }
        throwIfCancelled();
    };
    const auto archiveManifestCheckpoint = [&]() {
        if (hooks.OnArchiveManifestCheckpoint)
        {
            hooks.OnArchiveManifestCheckpoint();
        }
        throwIfCancelled();
    };

    PublishSnapshot(record, [&](SInpxScanJobSnapshot& snapshot) {
        snapshot.Status = EInpxScanJobStatus::Running;
        snapshot.Message = "Creating a stable INPX snapshot for " + modeLabel + '.';
    });
    const std::string sourceFingerprintUtf8 = expectedSourceFingerprintUtf8.has_value()
        ? *expectedSourceFingerprintUtf8
        : Foundation::CSha256Fingerprint::ComputeFile(
            source.InpxPath,
            sourceFingerprintCheckpoint);
    const auto snapshotPath = workingDirectory / "source-snapshot.inpx";
    std::filesystem::copy_file(source.InpxPath, snapshotPath, std::filesystem::copy_options::overwrite_existing);
    if (Foundation::CSha256Fingerprint::ComputeFile(snapshotPath, sourceFingerprintCheckpoint) != sourceFingerprintUtf8)
    {
        throw std::runtime_error("INPX source changed while its scan snapshot was being created; retry the scan.");
    }
    throwIfCancelled();

    PublishSnapshot(record, [&](SInpxScanJobSnapshot& snapshot) {
        snapshot.Message = "Planning changed INPX segments for " + modeLabel + '.';
    });
    TExistingInpxSegments existingSegments = ReadExistingInpxSegments(
        databasePath,
        planningLimits.MaxSegments);
    std::vector<SInpxSegmentPlan> plans;
    TInpxArchiveScanGuards archiveGuards;
    std::set<std::string> currentSegmentNamesUtf8;
    const Inpx::CInpxParser inpxParser;
    std::size_t plannedRecordCount = 0;
    std::size_t retainedWarningCount = 0;
    constexpr std::size_t GMaxRetainedPlanningWarnings = 1'000;
    std::size_t currentSegmentCount = 0;
    {
        StoragePlanning::CInpxArchiveRootIndex archiveRootIndex(
            source.ArchiveRoot,
            "INPX archive root could not be canonicalized.",
            {.Checkpoint = throwIfCancelled,
             .MaxFilesystemEntries = planningLimits.MaxFilesystemEntries,
             .OnFallbackSnapshotBuild = hooks.OnArchiveFallbackSnapshotBuild,
             .OnFilesystemEntryVisited = hooks.OnArchiveRootEntryVisited});
        currentSegmentCount = inpxParser.ReadSegments(
            snapshotPath,
            [&](Inpx::SInpxSegmentPayload&& payload) {
                throwIfCancelled();
                RequireCollectionRoom(plans.size(), planningLimits.MaxSegments, "segment count");
                SInpxSegmentPlan plan{.Payload = std::move(payload),
                                      .SegmentOrder = static_cast<std::int64_t>(plans.size())};
                if (!InsertOwnedString(currentSegmentNamesUtf8, plan.Payload.InpEntryNameUtf8))
                {
                    throw std::runtime_error("Duplicate INP entry name '" + plan.Payload.InpEntryNameUtf8
                                             + "' makes the INPX snapshot ambiguous.");
                }
                const auto existingIterator = existingSegments.find(plan.Payload.InpEntryNameUtf8);
                if (existingIterator != existingSegments.end())
                {
                    plan.ExistingId = existingIterator->second.Id;
                }

                const auto parseSegment = [&]() {
                    plan.Summary = inpxParser.ParseSegment(
                        plan.Payload,
                        [&](Inpx::SInpxRecord&& inpxRecord) {
                            RequireCollectionRoom(
                                plannedRecordCount,
                                planningLimits.MaxRecords,
                                "retained record count");
                            ++plannedRecordCount;
                            plan.Records.push_back(std::move(inpxRecord));
                            const auto& storedRecord = plan.Records.back();
                            if (storedRecord.Deleted)
                            {
                                static_cast<void>(
                                    InsertOwnedString(plan.DeletedLibIds, storedRecord.LibId));
                            }
                            else if (Foundation::ToLowerAscii(storedRecord.FileExtensionUtf8) == "fb2")
                            {
                                plan.RequiresArchive = true;
                            }
                        },
                        [&](const Inpx::SInpxWarning& warning) {
                            if (warning.RecordSkipped)
                            {
                                throw std::runtime_error("Invalid INPX record in '" + warning.InpEntryNameUtf8
                                                         + "' at line " + std::to_string(warning.LineNumber) + ": "
                                                         + warning.MessageUtf8);
                            }
                            if (retainedWarningCount < GMaxRetainedPlanningWarnings)
                            {
                                plan.Warnings.push_back(warning);
                                ++retainedWarningCount;
                            }
                        },
                        inpParserCheckpoint);
                };
                const auto readArchiveState = [&]() {
                    try
                    {
                        const auto resolvedArchive = InpxWebReader::Application::ResolvePortableInpxArchivePath(
                            archiveRootIndex, plan.Payload.ArchiveNameUtf8);
                        plan.ResolvedArchivePathUtf8 = resolvedArchive.RelativePathUtf8;
                        auto guardIterator = archiveGuards.find(plan.ResolvedArchivePathUtf8);
                        if (guardIterator == archiveGuards.end())
                        {
                            RequireCollectionRoom(
                                archiveGuards.size(),
                                planningLimits.MaxSegments,
                                "archive guard count");
                            SInpxArchiveScanGuard guard{.ArchiveNameUtf8 = plan.Payload.ArchiveNameUtf8,
                                                        .ResolvedArchivePathUtf8 = plan.ResolvedArchivePathUtf8,
                                                        .FileState =
                                                            InpxWebReader::Application::ReadInpxArchiveFileState(
                                                                resolvedArchive.AbsolutePath)};
                            const auto [insertedIterator, inserted] =
                                archiveGuards.emplace(plan.ResolvedArchivePathUtf8, std::move(guard));
                            if (!inserted)
                            {
                                throw std::runtime_error("Duplicate INPX archive guard path.");
                            }
                            guardIterator = insertedIterator;
                        }
                        plan.ArchiveFileState = guardIterator->second.FileState;
                    }
                    catch (const std::exception& exception)
                    {
                        throw std::runtime_error("INPX archive '" + plan.Payload.ArchiveNameUtf8
                                                 + "' is missing or inaccessible: " + exception.what());
                    }
                };
                const auto readArchiveManifest = [&]() {
                    try
                    {
                        auto& guard = archiveGuards.at(plan.ResolvedArchivePathUtf8);
                        if (guard.Phase == EInpxArchiveGuardPhase::FileStateRead)
                        {
                            const InpxWebReader::Application::CInpxArchiveReader reader(
                                InpxWebReader::Application::ResolvePersistedInpxArchivePath(
                                    source, plan.ResolvedArchivePathUtf8));
                            guard.ManifestFingerprintUtf8 = reader.ComputeManifestFingerprint(
                                maxArchiveManifestMemoryBytes, archiveManifestCheckpoint);
                            guard.Phase = EInpxArchiveGuardPhase::ManifestRead;
                            plan.ArchiveOpened = true;
                        }
                        plan.ArchiveManifestFingerprintUtf8 = guard.ManifestFingerprintUtf8;
                    }
                    catch (const CInpxScanCancelled&)
                    {
                        throw;
                    }
                    catch (const std::exception& exception)
                    {
                        throw std::runtime_error("INPX archive '" + plan.Payload.ArchiveNameUtf8
                                                 + "' is unavailable or damaged: " + exception.what());
                    }
                };
                const auto validateArchivePayloads = [&]() {
                    try
                    {
                        auto& guard = archiveGuards.at(plan.ResolvedArchivePathUtf8);
                        if (guard.Phase == EInpxArchiveGuardPhase::PayloadsValidated)
                        {
                            return;
                        }
                        if (guard.Phase != EInpxArchiveGuardPhase::ManifestRead)
                        {
                            throw std::runtime_error("archive manifest was not read before payload validation");
                        }
                        const InpxWebReader::Application::CInpxArchiveReader reader(
                            InpxWebReader::Application::ResolvePersistedInpxArchivePath(source,
                                                                                         plan.ResolvedArchivePathUtf8));
                        auto timer =
                            perf.MeasureStage(InpxWebReader::ScanSupport::CScanPerfTracker::EStage::ZipExtract);
                        const auto manifest =
                            reader.ReadValidatedManifest(maxArchiveManifestMemoryBytes, archiveManifestCheckpoint);
                        if (manifest.FingerprintUtf8 != guard.ManifestFingerprintUtf8)
                        {
                            throw std::runtime_error(
                                "archive manifest changed while its payloads were being validated");
                        }
                        PublishSnapshot(record, [&](SInpxScanJobSnapshot& snapshot) {
                            snapshot.ArchiveBytesRead += manifest.PayloadBytesValidated;
                        });
                        guard.Phase = EInpxArchiveGuardPhase::PayloadsValidated;
                    }
                    catch (const CInpxScanCancelled&)
                    {
                        throw;
                    }
                    catch (const std::exception& exception)
                    {
                        throw std::runtime_error("INPX archive '" + plan.Payload.ArchiveNameUtf8
                                                 + "' is unavailable or damaged: " + exception.what());
                    }
                };

                const bool canReuseExisting =
                    request.Mode == EInpxScanMode::Rescan && existingIterator != existingSegments.end()
                    && existingIterator->second.InpFingerprintUtf8 == plan.Payload.FingerprintUtf8
                    && InpxWebReader::Domain::IsInpxBookAvailable(existingIterator->second.AvailabilityUtf8);
                if (canReuseExisting && !existingIterator->second.RequiresArchive)
                {
                    plan.Change = EInpxSegmentChange::Unchanged;
                    plan.Summary = existingIterator->second.Summary;
                }
                else if (canReuseExisting)
                {
                    const auto& existing = existingIterator->second;
                    if (!existing.ResolvedArchivePathUtf8.has_value()
                        || !existing.ArchiveManifestFingerprintUtf8.has_value())
                    {
                        throw std::runtime_error("Cached archive guard is missing for INP segment '"
                                                 + plan.Payload.InpEntryNameUtf8 + "'.");
                    }
                    plan.RequiresArchive = true;
                    readArchiveState();
                    const bool sameArchiveStat =
                        *existing.ResolvedArchivePathUtf8 == plan.ResolvedArchivePathUtf8
                        && existing.ArchiveFileState.FileSizeBytes == plan.ArchiveFileState.FileSizeBytes
                        && existing.ArchiveFileState.MtimeTicks == plan.ArchiveFileState.MtimeTicks;
                    if (sameArchiveStat)
                    {
                        plan.Change = EInpxSegmentChange::Unchanged;
                        plan.Summary = existing.Summary;
                        plan.ArchiveManifestFingerprintUtf8 = *existing.ArchiveManifestFingerprintUtf8;
                    }
                    else
                    {
                        readArchiveManifest();
                        if (*existing.ArchiveManifestFingerprintUtf8 == plan.ArchiveManifestFingerprintUtf8)
                        {
                            validateArchivePayloads();
                            plan.Change = EInpxSegmentChange::Unchanged;
                            plan.Summary = existing.Summary;
                        }
                        else
                        {
                            plan.Change = EInpxSegmentChange::Changed;
                            parseSegment();
                        }
                    }
                }
                else
                {
                    plan.Change = existingIterator == existingSegments.end() ? EInpxSegmentChange::Added
                                                                             : EInpxSegmentChange::Changed;
                    parseSegment();
                    if (plan.RequiresArchive)
                    {
                        readArchiveState();
                        readArchiveManifest();
                    }
                }
                std::string{}.swap(plan.Payload.Bytes);
                plans.push_back(std::move(plan));
            },
            inpParserCheckpoint,
            planningLimits.MaxInpSourceBytes);
    }
    if (currentSegmentCount == 0)
    {
        throw std::runtime_error("INPX snapshot does not contain any .inp segments.");
    }

    for (const auto& [segmentNameUtf8, existing] : existingSegments)
    {
        if (currentSegmentNamesUtf8.contains(segmentNameUtf8))
        {
            continue;
        }
        SInpxSegmentPlan removedPlan{.Change = EInpxSegmentChange::Removed,
                                     .Payload = {.InpEntryNameUtf8 = existing.InpEntryNameUtf8,
                                                 .ArchiveNameUtf8 = existing.ArchiveNameUtf8,
                                                 .FingerprintUtf8 = existing.InpFingerprintUtf8},
                                     .Summary = existing.Summary,
                                     .SegmentOrder = existing.SegmentOrder,
                                     .RequiresArchive = existing.RequiresArchive,
                                     .ResolvedArchivePathUtf8 = existing.ResolvedArchivePathUtf8.value_or(""),
                                     .ArchiveFileState = existing.ArchiveFileState,
                                     .ArchiveManifestFingerprintUtf8 =
                                         existing.ArchiveManifestFingerprintUtf8.value_or(""),
                                     .ExistingId = existing.Id};
        RequireCollectionRoom(plans.size(), planningLimits.MaxSegments, "segment count");
        plans.push_back(std::move(removedPlan));
    }

    std::size_t totalRecords = 0;
    std::size_t deletedRecords = 0;
    std::set<std::string> unchangedSegmentNamesUtf8;
    for (const auto& plan : plans)
    {
        if (plan.Change != EInpxSegmentChange::Removed)
        {
            if (plan.Summary.TotalRecords > planningLimits.MaxRecords - totalRecords)
            {
                throw std::runtime_error(
                    "INPX record count exceeds the configured scan limit of "
                    + std::to_string(planningLimits.MaxRecords) + " records.");
            }
            totalRecords += plan.Summary.TotalRecords;
            deletedRecords += plan.Summary.DeletedRecords;
        }
        if (plan.Change == EInpxSegmentChange::Unchanged)
        {
            static_cast<void>(InsertOwnedString(
                unchangedSegmentNamesUtf8,
                plan.Payload.InpEntryNameUtf8));
        }
    }
    PublishSnapshot(record, [&](SInpxScanJobSnapshot& snapshot) {
        snapshot.TotalRecords = totalRecords;
        snapshot.SegmentsTotal = plans.size();
        for (const auto& plan : plans)
        {
            if (plan.ArchiveOpened)
            {
                ++snapshot.ArchivesOpened;
            }
            switch (plan.Change)
            {
                case EInpxSegmentChange::Unchanged:
                    ++snapshot.SegmentsUnchanged;
                    if (plan.RequiresArchive)
                    {
                        ++snapshot.ArchivesSkipped;
                    }
                    snapshot.ScannedRecords += plan.Summary.TotalRecords;
                    snapshot.ReusedRecords += plan.Summary.TotalRecords;
                    break;
                case EInpxSegmentChange::Added:
                    ++snapshot.SegmentsAdded;
                    break;
                case EInpxSegmentChange::Changed:
                    ++snapshot.SegmentsChanged;
                    break;
                case EInpxSegmentChange::Removed:
                    ++snapshot.SegmentsRemoved;
                    break;
            }
        }
        snapshot.Percent = ComputePercent(snapshot.ScannedRecords, snapshot.TotalRecords);
        snapshot.Message = "Scanning changed INPX segments for " + modeLabel + '.';
    });

    std::unique_ptr<CInpxCatalogStreamWriter> catalogWriter;
    if (!databasePath.empty())
    {
        catalogWriter = std::make_unique<CInpxCatalogStreamWriter>(
            databasePath,
            cacheRoot,
            scanStartedAtUtc,
            throwIfCancelled,
            maxCoverCacheBytes);
    }
    std::vector<std::int64_t> segmentIds;
    segmentIds.resize(plans.size(), 0);
    std::vector<std::int64_t> changedSegmentIds;
    if (catalogWriter != nullptr)
    {
        for (std::size_t index = 0; index < plans.size(); ++index)
        {
            throwIfCancelled();
            segmentIds[index] = catalogWriter->UpsertSegment(plans[index], throwIfCancelled);
            if (plans[index].Change == EInpxSegmentChange::Added
                || plans[index].Change == EInpxSegmentChange::Changed)
            {
                changedSegmentIds.push_back(segmentIds[index]);
            }
        }
    }

    const auto publishWarning = [&](const SInpxScanWarningRecord& warning, const bool countAsSkipped) {
        ++scanWarningCount;
        LogInpxScanWarningNoThrow(jobId, warning, countAsSkipped);
        if (catalogWriter != nullptr)
        {
            catalogWriter->WriteWarning(warning);
        }
        PublishSnapshot(record, [&](SInpxScanJobSnapshot& snapshot) {
            if (countAsSkipped)
            {
                ++snapshot.SkippedRecords;
            }
            if (snapshot.Warnings.size() < request.WarningLimit)
            {
                std::string warningMessage = BuildWarningMessage(
                    warning.ArchiveNameUtf8,
                    warning.EntryNameUtf8,
                    warning.MessageUtf8);
                snapshot.Warnings.push_back(std::move(warningMessage));
            }
        });
    };
    for (const auto& plan : plans)
    {
        for (const auto& warning : plan.Warnings)
        {
            publishWarning({
                .Code = "inpx-parse-warning",
                .ArchiveNameUtf8 = warning.InpEntryNameUtf8,
                .MessageUtf8 = warning.MessageUtf8
            }, false);
        }
    }

    std::set<std::string> processedActiveIds = ReadActiveInpxIdsForSegments(
        databasePath,
        unchangedSegmentNamesUtf8,
        planningLimits.MaxRecords);
    std::vector<std::size_t> executionPlanIndices;
    executionPlanIndices.reserve(plans.size());
    for (std::size_t planIndex = 0; planIndex < plans.size(); ++planIndex)
    {
        throwIfCancelled();
        if (plans[planIndex].Change == EInpxSegmentChange::Added
            || plans[planIndex].Change == EInpxSegmentChange::Changed)
        {
            executionPlanIndices.push_back(planIndex);
        }
    }
    std::size_t executionPlanComparisonCount = 0;
    std::ranges::sort(
        executionPlanIndices,
        [&](const std::size_t leftIndex, const std::size_t rightIndex) {
            if (++executionPlanComparisonCount % GExecutionPlanSortCheckpointInterval == 0)
            {
                throwIfCancelled();
            }
            const auto& leftPath = plans[leftIndex].ResolvedArchivePathUtf8;
            const auto& rightPath = plans[rightIndex].ResolvedArchivePathUtf8;
            return leftPath == rightPath
                ? leftIndex < rightIndex
                : leftPath < rightPath;
        });
    throwIfCancelled();
    CInpxScanProgressLogger progressLogger(jobId, std::chrono::steady_clock::now());
    std::unique_ptr<InpxWebReader::Application::CInpxArchiveReader> currentArchiveReader;
    std::string_view currentArchivePathUtf8;
    std::uint64_t inFlightPayloadBytes = 0;

    const auto takeParsedRecord = [&](const bool preferReady) {
        if (preferReady)
        {
            const auto ready = std::find_if(
                inFlightParsedRecords.begin(),
                inFlightParsedRecords.end(),
                [](SInFlightParsedRecord& task) {
                    return task.PreparedPayload.wait_for(std::chrono::milliseconds(0))
                        == std::future_status::ready;
                });
            if (ready != inFlightParsedRecords.end())
            {
                SInFlightParsedRecord task = std::move(*ready);
                inFlightParsedRecords.erase(ready);
                return task;
            }
        }
        SInFlightParsedRecord task = std::move(inFlightParsedRecords.front());
        inFlightParsedRecords.pop_front();
        return task;
    };

    const auto drainParsedRecord = [&](const bool preferReady) {
        SInFlightParsedRecord task = takeParsedRecord(preferReady);
        const Inpx::SInpxRecord& inpxRecord = *task.InpxRecord;
        const auto releasePayloadReservation = [&]() {
            inFlightPayloadBytes = task.ReservedPayloadBytes <= inFlightPayloadBytes
                ? inFlightPayloadBytes - task.ReservedPayloadBytes
                : 0;
        };
        try
        {
            const SPreparedInpxPayload& preparedPayload = task.PreparedPayload.get();
            throwIfCancelled();
            SInpxRecordWriteResult writeResult;
            writeResult.ParserWarningCount = preparedPayload.ParsedBook.ParserWarningCount;
            writeResult.CoverWarningCount = preparedPayload.CoverWarningCount;
            if (catalogWriter == nullptr && preparedPayload.ParsedBook.CoverDiagnosticMessage.has_value())
            {
                ++writeResult.CoverWarningCount;
            }
            if (catalogWriter != nullptr)
            {
                writeResult = catalogWriter->WriteParsedRecord(
                    task.SegmentId,
                    inpxRecord,
                    preparedPayload.ParsedBook,
                    preparedPayload.CoverStorageResolution,
                    preparedPayload.CoverWarningCount,
                    perf);
            }
            PublishSnapshot(record, [&](SInpxScanJobSnapshot& snapshot) {
                ++snapshot.ParsedFb2Records;
                if (writeResult.Added)
                {
                    ++snapshot.AddedRecords;
                    ++applyResult.AddedRecords;
                }
                if (writeResult.Updated)
                {
                    ++snapshot.UpdatedRecords;
                    ++applyResult.UpdatedRecords;
                }
                if (writeResult.CachedCover)
                {
                    ++applyResult.CachedCoverRecords;
                    applyResult.CachedCoverBytes += writeResult.CachedCoverBytes;
                }
                applyResult.ParserWarningCount += writeResult.ParserWarningCount;
                applyResult.CoverWarningCount += writeResult.CoverWarningCount;
                applyResult.MetadataFallbacks.TitleFallbackCount += writeResult.MetadataFallbacks.TitleFallbackCount;
                applyResult.MetadataFallbacks.AuthorFallbackCount += writeResult.MetadataFallbacks.AuthorFallbackCount;
                applyResult.MetadataFallbacks.LanguageFallbackCount += writeResult.MetadataFallbacks.LanguageFallbackCount;
            });
            perf.OnBookProcessed(writeResult.Added ? 1 : 0, writeResult.Updated ? 1 : 0, 0);
            perf.NoteOutlierIfSlow(
                BuildInpxSourceLabel(inpxRecord),
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - task.StartedAt));
            releasePayloadReservation();
        }
        catch (const CInpxScanCancelled&)
        {
            releasePayloadReservation();
            throw;
        }
        catch (const std::exception& exception)
        {
            releasePayloadReservation();
            throwIfCancelled();
            publishWarning({
                .Code = "payload-load-failed",
                .ArchiveNameUtf8 = inpxRecord.ArchiveNameUtf8,
                .EntryNameUtf8 = inpxRecord.EntryNameUtf8,
                .MessageUtf8 = exception.what()
            }, true);
            perf.OnBookProcessed(0, 0, 1);
            throw std::runtime_error(
                "INPX payload '" + BuildInpxSourceLabel(inpxRecord)
                + "' could not be indexed atomically: " + exception.what());
        }
    };

    const auto drainRemainingPayloadWork = [&]() noexcept {
        while (!inFlightParsedRecords.empty())
        {
            SInFlightParsedRecord task = takeParsedRecord(false);
            try
            {
                static_cast<void>(task.PreparedPayload.get());
            }
            catch (...)
            {
                (void)0;
            }
            inFlightPayloadBytes = task.ReservedPayloadBytes <= inFlightPayloadBytes
                ? inFlightPayloadBytes - task.ReservedPayloadBytes
                : 0;
        }
    };

    try
    {
        for (const std::size_t planIndex : executionPlanIndices)
        {
            auto& plan = plans[planIndex];
            throwIfCancelled();
            for (auto& inpxRecord : plan.Records)
            {
                throwIfCancelled();
                PublishSnapshot(record, [&](SInpxScanJobSnapshot& snapshot) {
                    ++snapshot.ScannedRecords;
                    snapshot.Percent = ComputePercent(snapshot.ScannedRecords, snapshot.TotalRecords);
                    snapshot.CurrentArchiveNameUtf8 = inpxRecord.ArchiveNameUtf8;
                    snapshot.CurrentEntryNameUtf8 = inpxRecord.EntryNameUtf8;
                    snapshot.Message = "Indexed " + std::to_string(snapshot.ScannedRecords) + " of "
                        + std::to_string(snapshot.TotalRecords) + " INPX records.";
                });
                std::optional<SInpxScanProgressLogView> progressLogView;
                std::optional<std::chrono::steady_clock::time_point> progressLogAt;
                std::size_t sourceWarningCount = 0;
                {
                    const std::scoped_lock lock(record->Mutex);
                    progressLogAt = progressLogger.ClaimIfDue(record->Snapshot.ScannedRecords);
                    if (progressLogAt.has_value())
                    {
                        progressLogView = SInpxScanProgressLogView{
                            .Percent = record->Snapshot.Percent,
                            .TotalRecords = record->Snapshot.TotalRecords,
                            .ScannedRecords = record->Snapshot.ScannedRecords,
                            .ParsedFb2Records = record->Snapshot.ParsedFb2Records,
                            .AddedRecords = record->Snapshot.AddedRecords,
                            .UpdatedRecords = record->Snapshot.UpdatedRecords,
                            .MarkedUnavailableRecords = record->Snapshot.MarkedUnavailableRecords,
                            .SkippedRecords = record->Snapshot.SkippedRecords,
                            .CurrentArchiveNameUtf8 = record->Snapshot.CurrentArchiveNameUtf8,
                            .CurrentEntryNameUtf8 = record->Snapshot.CurrentEntryNameUtf8
                        };
                        sourceWarningCount = record->Snapshot.Warnings.size();
                    }
                }
                if (progressLogView.has_value())
                {
                    if (hooks.AfterProgressLogViewCaptured)
                    {
                        hooks.AfterProgressLogViewCaptured(sourceWarningCount);
                    }
                    progressLogger.Log(
                        *progressLogView,
                        scanWarningCount,
                        inFlightParsedRecords.size(),
                        *progressLogAt);
                }
                if (hooks.AfterRecordProcessed)
                {
                    hooks.AfterRecordProcessed();
                }
                if (inpxRecord.Deleted)
                {
                    continue;
                }
                if (Foundation::ToLowerAscii(inpxRecord.FileExtensionUtf8) != "fb2")
                {
                    publishWarning({
                        .Code = "unsupported-format",
                        .ArchiveNameUtf8 = inpxRecord.ArchiveNameUtf8,
                        .EntryNameUtf8 = inpxRecord.EntryNameUtf8,
                        .MessageUtf8 = "Unsupported INPX payload format '"
                            + inpxRecord.FileExtensionUtf8 + "'."
                    }, true);
                    continue;
                }
                if (!InsertActiveLibId(
                        processedActiveIds,
                        inpxRecord.LibId,
                        planningLimits.MaxRecords))
                {
                    throw std::runtime_error(
                        "Duplicate active LibId '" + inpxRecord.LibId
                        + "' makes the INPX snapshot ambiguous.");
                }
                while (inFlightParsedRecords.size() >= maxInFlightPayloads)
                {
                    drainParsedRecord(true);
                    throwIfCancelled();
                }
                if (currentArchivePathUtf8 != plan.ResolvedArchivePathUtf8)
                {
                    while (!inFlightParsedRecords.empty())
                    {
                        drainParsedRecord(true);
                    }
                    archivePerf.FinishCurrentArchive();
                    currentArchiveReader.reset();
                    const auto archivePath = InpxWebReader::Application::ResolvePersistedInpxArchivePath(
                        source,
                        plan.ResolvedArchivePathUtf8);
                    archivePerf.BeginArchive(archivePath);
                    InpxWebReader::Logging::InfoIfInitialized(
                        "INPX scan archive started. jobId={} archive='{}'",
                        jobId,
                        InpxWebReader::Unicode::PathToUtf8(archivePath));
                    currentArchiveReader =
                        std::make_unique<InpxWebReader::Application::CInpxArchiveReader>(
                            archivePath);
                    if (hooks.AfterExecutionArchiveOpened)
                    {
                        hooks.AfterExecutionArchiveOpened();
                    }
                    if (currentArchiveReader->ComputeManifestFingerprint(
                            maxArchiveManifestMemoryBytes,
                            archiveManifestCheckpoint)
                        != plan.ArchiveManifestFingerprintUtf8)
                    {
                        throw std::runtime_error(
                            "INPX archive '" + plan.Payload.ArchiveNameUtf8
                            + "' changed before its payloads were read; retry the scan.");
                    }
                    currentArchivePathUtf8 = plan.ResolvedArchivePathUtf8;
                }

                const std::uint64_t actualSizeBytes =
                    currentArchiveReader->ReadEntrySize(inpxRecord.EntryNameUtf8);
                if (inpxRecord.FileSizeBytes != 0 && actualSizeBytes != inpxRecord.FileSizeBytes)
                {
                    throw std::runtime_error(
                        "INPX payload size mismatch for '" + BuildInpxSourceLabel(inpxRecord)
                        + "': INPX declares " + std::to_string(inpxRecord.FileSizeBytes)
                        + " bytes but ZIP contains " + std::to_string(actualSizeBytes) + " bytes.");
                }
                if (actualSizeBytes > maxInFlightPayloadBytes)
                {
                    throw std::runtime_error(
                        "INPX payload '" + BuildInpxSourceLabel(inpxRecord) + "' requires "
                        + std::to_string(actualSizeBytes)
                        + " raw bytes, exceeding the configured in-flight payload limit of "
                        + std::to_string(maxInFlightPayloadBytes) + " bytes.");
                }
                while (!inFlightParsedRecords.empty()
                    && inFlightPayloadBytes > maxInFlightPayloadBytes - actualSizeBytes)
                {
                    drainParsedRecord(true);
                    throwIfCancelled();
                }
                std::string payloadBytes;
                {
                    auto timer = perf.MeasureStage(InpxWebReader::ScanSupport::CScanPerfTracker::EStage::ZipExtract);
                    payloadBytes = currentArchiveReader->ReadEntryBytes(
                        inpxRecord.EntryNameUtf8,
                        throwIfCancelled);
                }
                PublishSnapshot(record, [&](SInpxScanJobSnapshot& snapshot) {
                    snapshot.ArchiveBytesRead += actualSizeBytes;
                });
                const auto payloadStartedAt = std::chrono::steady_clock::now();
                const bool prepareCoverForCache = catalogWriter != nullptr;
                inpxRecord.FileSizeBytes = actualSizeBytes;
                inFlightParsedRecords.push_back({
                    .SegmentId = segmentIds[planIndex],
                    .InpxRecord = &inpxRecord,
                    .PreparedPayload = payloadWorkerPool.submit_task(
                        [payloadBytes = std::move(payloadBytes),
                            recordForWorker = &inpxRecord,
                            coverImageProcessor,
                            prepareCoverForCache,
                            maxDecodedCoverBytes,
                            stopToken,
                            &perf]() mutable {
                            const std::string sourceLabel = BuildInpxSourceLabel(*recordForWorker);
                            return ParseInpxPayloadBytes(
                                std::move(payloadBytes),
                                *recordForWorker,
                                sourceLabel,
                                coverImageProcessor,
                                prepareCoverForCache,
                                maxDecodedCoverBytes,
                                stopToken,
                                perf);
                        }).share(),
                    .StartedAt = payloadStartedAt,
                    .ReservedPayloadBytes = actualSizeBytes
                });
                inFlightPayloadBytes += actualSizeBytes;
            }
        }
        while (!inFlightParsedRecords.empty())
        {
            drainParsedRecord(true);
            throwIfCancelled();
        }
    }
    catch (...)
    {
        drainRemainingPayloadWork();
        throw;
    }
    archivePerf.FinishCurrentArchive();

    if (catalogWriter != nullptr)
    {
        PublishSnapshot(record, [&](SInpxScanJobSnapshot& snapshot) {
            snapshot.Message = "Publishing INPX " + modeLabel + " atomically.";
        });
        const auto finishResult = catalogWriter->Finish(
            source,
            sourceFingerprintUtf8,
            plans,
            archiveGuards,
            maxArchiveManifestMemoryBytes,
            changedSegmentIds,
            hooks,
            throwIfCancelled);
        applyResult.MarkedUnavailableRecords = finishResult.MarkedUnavailableRecords;
        PublishSnapshot(record, [&](SInpxScanJobSnapshot& snapshot) {
            snapshot.MarkedUnavailableRecords = applyResult.MarkedUnavailableRecords;
        });
    }

    SInpxScanJobResult result;
    result.Snapshot = CopySnapshot(record);
    result.Snapshot.Status = EInpxScanJobStatus::Completed;
    result.Snapshot.Percent = 100;
    result.Snapshot.Message = databasePath.empty()
        ? "INPX " + modeLabel + " planning completed."
        : "INPX " + modeLabel + " completed.";
    result.ScanResult = SInpxScanResult{
        .WarningCount = scanWarningCount,
        .ParserWarningCount = applyResult.ParserWarningCount,
        .CoverWarningCount = applyResult.CoverWarningCount,
        .MetadataFallbackCount = applyResult.MetadataFallbacks.Total(),
        .TitleFallbackCount = applyResult.MetadataFallbacks.TitleFallbackCount,
        .AuthorFallbackCount = applyResult.MetadataFallbacks.AuthorFallbackCount,
        .LanguageFallbackCount = applyResult.MetadataFallbacks.LanguageFallbackCount
    };
    perfSummary.LogNow();
    InpxWebReader::Logging::InfoIfInitialized(
        "INPX {} completed. jobId={} total={} scanned={} reused={} parsedFb2={} added={} updated={} "
        "unavailable={} deleted={} skipped={} scanWarnings={} parserWarnings={} coverWarnings={} "
        "metadataFallbacks={} titleFallbacks={} authorFallbacks={} languageFallbacks={} "
        "covers={} coverBytes={} segments={}/{}/{}/{} archivesSkipped={} archivesOpened={} archiveBytesRead={}",
        modeLabel,
        jobId,
        result.Snapshot.TotalRecords,
        result.Snapshot.ScannedRecords,
        result.Snapshot.ReusedRecords,
        result.Snapshot.ParsedFb2Records,
        result.Snapshot.AddedRecords,
        result.Snapshot.UpdatedRecords,
        result.Snapshot.MarkedUnavailableRecords,
        deletedRecords,
        result.Snapshot.SkippedRecords,
        result.ScanResult->WarningCount,
        result.ScanResult->ParserWarningCount,
        result.ScanResult->CoverWarningCount,
        result.ScanResult->MetadataFallbackCount,
        result.ScanResult->TitleFallbackCount,
        result.ScanResult->AuthorFallbackCount,
        result.ScanResult->LanguageFallbackCount,
        applyResult.CachedCoverRecords,
        applyResult.CachedCoverBytes,
        result.Snapshot.SegmentsUnchanged,
        result.Snapshot.SegmentsAdded,
        result.Snapshot.SegmentsChanged,
        result.Snapshot.SegmentsRemoved,
        result.Snapshot.ArchivesSkipped,
        result.Snapshot.ArchivesOpened,
        result.Snapshot.ArchiveBytesRead);
    return result;
}

} // namespace

CInpxScanJobService::CInpxScanJobService(
    std::filesystem::path runtimeWorkspaceRoot,
    SHooks hooks,
    std::filesystem::path databasePath,
    std::filesystem::path cacheRoot,
    const InpxWebReader::Domain::ICoverImageProcessor* const coverImageProcessor,
    const std::size_t maxWorkerCount,
    const std::uint64_t maxCoverCacheBytes,
    const std::uint64_t maxSteadyStateMemoryBytes)
    : m_runtimeWorkspaceRoot(std::move(runtimeWorkspaceRoot))
    , m_databasePath(std::move(databasePath))
    , m_cacheRoot(std::move(cacheRoot))
    , m_coverImageProcessor(coverImageProcessor)
    , m_maxWorkerCount(maxWorkerCount)
    , m_maxCoverCacheBytes(maxCoverCacheBytes)
    , m_maxSteadyStateMemoryBytes(maxSteadyStateMemoryBytes)
    , m_hooks(std::move(hooks))
{
    if (m_runtimeWorkspaceRoot.empty())
    {
        throw std::invalid_argument("INPX scan workspace root is required.");
    }
    if (m_maxCoverCacheBytes == 0 || m_maxSteadyStateMemoryBytes == 0)
    {
        throw std::invalid_argument("INPX scan resource budgets must be positive.");
    }

    std::error_code createError;
    std::filesystem::create_directories(m_runtimeWorkspaceRoot, createError);
    if (createError)
    {
        InpxWebReader::Logging::WarnIfInitialized(
            "INPX scan workspace root could not be created. path='{}' error='{}'",
            InpxWebReader::Unicode::PathToUtf8(m_runtimeWorkspaceRoot),
            createError.message());
        return;
    }

    std::error_code enumerateError;
    for (const auto& entry : std::filesystem::directory_iterator(m_runtimeWorkspaceRoot, enumerateError))
    {
        if (!entry.is_directory())
        {
            continue;
        }

        const auto directoryName = entry.path().filename().string();
        const bool isJobWorkspace = !directoryName.empty()
            && std::all_of(directoryName.begin(), directoryName.end(), [](const unsigned char c) {
                return std::isdigit(c) != 0;
            });
        if (!isJobWorkspace)
        {
            continue;
        }

        std::error_code removeError;
        std::filesystem::remove_all(entry.path(), removeError);
        if (removeError)
        {
            InpxWebReader::Logging::WarnIfInitialized(
                "INPX stale scan workspace cleanup failed. path='{}' error='{}'",
                InpxWebReader::Unicode::PathToUtf8(entry.path()),
                removeError.message());
        }
    }

    if (enumerateError)
    {
        InpxWebReader::Logging::WarnIfInitialized(
            "INPX stale scan workspace enumeration failed. path='{}' error='{}'",
            InpxWebReader::Unicode::PathToUtf8(m_runtimeWorkspaceRoot),
            enumerateError.message());
    }
}

CInpxScanJobService::~CInpxScanJobService()
{
    std::vector<std::shared_ptr<SJobRecord>> records;
    {
        const std::scoped_lock lock(m_jobsMutex);
        records.reserve(m_jobs.size());
        for (const auto& [_, record] : m_jobs)
        {
            records.push_back(record);
        }
    }

    for (const auto& record : records)
    {
        if (record->Worker.joinable())
        {
            record->Worker.request_stop();
            record->Worker.join();
        }

        std::error_code removeError;
        std::filesystem::remove_all(record->WorkingDirectory, removeError);
        if (removeError)
        {
            InpxWebReader::Logging::WarnIfInitialized(
                "INPX scan workspace cleanup failed. path='{}' error='{}'",
                InpxWebReader::Unicode::PathToUtf8(record->WorkingDirectory),
                removeError.message());
        }
    }
}

TInpxScanJobId CInpxScanJobService::Start(
    const InpxWebReader::Application::SInpxSourceInfo& source,
    const SInpxScanRequest& request,
    std::optional<std::string> expectedSourceFingerprintUtf8)
{
    auto record = std::make_shared<SJobRecord>();
    TInpxScanJobId jobId = 0;
    {
        const std::scoped_lock lock(m_jobsMutex);
        for (const auto& [_, existingRecord] : m_jobs)
        {
            const std::scoped_lock recordLock(existingRecord->Mutex);
            if (!existingRecord->Snapshot.IsTerminal())
            {
                throw std::runtime_error("An INPX scan is already running.");
            }
        }

        jobId = m_nextJobId++;
        record->Snapshot.JobId = jobId;
        m_jobs.emplace(jobId, record);
    }

    const auto jobWorkingDirectory = m_runtimeWorkspaceRoot / std::to_string(jobId);
    record->WorkingDirectory = jobWorkingDirectory;
    std::error_code createJobWorkspaceError;
    std::filesystem::create_directories(jobWorkingDirectory, createJobWorkspaceError);
    if (createJobWorkspaceError)
    {
        {
            const std::scoped_lock lock(m_jobsMutex);
            m_jobs.erase(jobId);
        }
        throw std::runtime_error(
            "INPX scan workspace could not be created: " + createJobWorkspaceError.message());
    }

    InpxWebReader::Logging::InfoIfInitialized(
        "INPX scan job queued. jobId={} mode='{}' source='{}' archiveRoot='{}'",
        jobId,
        BuildModeLabel(request.Mode),
        InpxWebReader::Unicode::PathToUtf8(source.InpxPath),
        InpxWebReader::Unicode::PathToUtf8(source.ArchiveRoot));

    try
    {
        if (m_hooks.BeforeWorkerStart)
        {
            m_hooks.BeforeWorkerStart();
        }

        record->Worker = std::jthread([source,
                                          request,
                                          expectedSourceFingerprintUtf8 = std::move(expectedSourceFingerprintUtf8),
                                          jobWorkingDirectory,
                                          databasePath = m_databasePath,
                                          cacheRoot = m_cacheRoot,
                                          maxWorkerCount = m_maxWorkerCount,
                                          maxCoverCacheBytes = m_maxCoverCacheBytes,
                                          maxSteadyStateMemoryBytes = m_maxSteadyStateMemoryBytes,
                                          coverImageProcessor = m_coverImageProcessor,
                                          jobId,
                                          record,
                                          hooks = m_hooks](std::stop_token stopToken) {
            SInpxScanJobResult result;

            try
            {
                result = RunScan(
                    source,
                    request,
                    expectedSourceFingerprintUtf8,
                    jobWorkingDirectory,
                    databasePath,
                    cacheRoot,
                    maxWorkerCount,
                    maxCoverCacheBytes,
                    maxSteadyStateMemoryBytes,
                    stopToken,
                    record,
                    hooks,
                    coverImageProcessor);
            }
            catch (const CInpxScanCancelled&)
            {
                InpxWebReader::Logging::InfoIfInitialized(
                    "INPX scan cancelled. jobId={} mode='{}'",
                    jobId,
                    BuildModeLabel(request.Mode));
                const std::scoped_lock lock(record->Mutex);
                result.Snapshot = record->Snapshot;
                result.Snapshot.Status = EInpxScanJobStatus::Cancelled;
                result.Snapshot.Message = "INPX scan cancelled.";
                result.Error = InpxWebReader::Domain::SDomainError{
                    .Code = InpxWebReader::Domain::EDomainErrorCode::Cancellation,
                    .Message = "INPX scan cancelled."
                };
            }
            catch (const std::exception& ex)
            {
                InpxWebReader::Logging::ErrorIfInitialized(
                    "INPX scan failed. jobId={} mode='{}' error='{}'",
                    jobId,
                    BuildModeLabel(request.Mode),
                    ex.what());
                const std::scoped_lock lock(record->Mutex);
                result.Snapshot = record->Snapshot;
                result.Snapshot.Status = EInpxScanJobStatus::Failed;
                result.Snapshot.Message = ex.what();
                result.Error = InpxWebReader::Domain::SDomainError{
                    .Code = InpxWebReader::Domain::EDomainErrorCode::ParserFailure,
                    .Message = ex.what()
                };
            }
            catch (...)
            {
                constexpr std::string_view errorMessage =
                    "INPX scan failed with a non-standard exception.";
                InpxWebReader::Logging::ErrorIfInitialized(
                    "INPX scan failed. jobId={} mode='{}' error='non-standard exception'",
                    jobId,
                    BuildModeLabel(request.Mode));
                const std::scoped_lock lock(record->Mutex);
                result.Snapshot = record->Snapshot;
                result.Snapshot.Status = EInpxScanJobStatus::Failed;
                result.Snapshot.Message = errorMessage;
                result.Error = InpxWebReader::Domain::SDomainError{
                    .Code = InpxWebReader::Domain::EDomainErrorCode::ParserFailure,
                    .Message = std::string{errorMessage}
                };
            }

            {
                const std::scoped_lock lock(record->Mutex);
                record->Snapshot = result.Snapshot;
                record->Result = result;
            }
            record->Condition.notify_all();
        });
    }
    catch (...)
    {
        {
            const std::scoped_lock lock(m_jobsMutex);
            const auto iterator = m_jobs.find(jobId);
            if (iterator != m_jobs.end() && iterator->second == record)
            {
                m_jobs.erase(iterator);
            }
        }

        std::error_code removeError;
        std::filesystem::remove_all(jobWorkingDirectory, removeError);
        if (removeError)
        {
            InpxWebReader::Logging::WarnIfInitialized(
                "INPX scan workspace cleanup after worker start failure failed. path='{}' error='{}'",
                InpxWebReader::Unicode::PathToUtf8(jobWorkingDirectory),
                removeError.message());
        }
        throw;
    }

    return jobId;
}

std::optional<SInpxScanJobSnapshot> CInpxScanJobService::TryGetSnapshot(const TInpxScanJobId jobId) const
{
    const auto record = TryGetRecord(jobId);
    if (!record)
    {
        return std::nullopt;
    }

    const std::scoped_lock lock(record->Mutex);
    return record->Snapshot;
}

std::optional<SInpxScanJobResult> CInpxScanJobService::TryGetResult(const TInpxScanJobId jobId) const
{
    const auto record = TryGetRecord(jobId);
    if (!record)
    {
        return std::nullopt;
    }

    const std::scoped_lock lock(record->Mutex);
    return record->Result;
}

bool CInpxScanJobService::Cancel(const TInpxScanJobId jobId) const
{
    const auto record = TryGetRecord(jobId);
    if (!record)
    {
        return false;
    }

    const std::scoped_lock lock(record->Mutex);
    if (record->Snapshot.IsTerminal())
    {
        return false;
    }

    record->Snapshot.Status = EInpxScanJobStatus::Cancelling;
    record->Snapshot.Message = "Cancelling INPX scan.";
    record->Worker.request_stop();
    record->Condition.notify_all();
    return true;
}

bool CInpxScanJobService::Wait(const TInpxScanJobId jobId, const std::chrono::milliseconds timeout) const
{
    const auto record = TryGetRecord(jobId);
    if (!record)
    {
        return false;
    }

    std::unique_lock lock(record->Mutex);
    return record->Condition.wait_for(lock, timeout, [&record] {
        return record->Snapshot.IsTerminal();
    });
}

bool CInpxScanJobService::Remove(const TInpxScanJobId jobId)
{
    std::shared_ptr<SJobRecord> record;
    {
        const std::scoped_lock lock(m_jobsMutex);
        const auto iterator = m_jobs.find(jobId);
        if (iterator == m_jobs.end())
        {
            return false;
        }

        record = iterator->second;
        {
            const std::scoped_lock recordLock(record->Mutex);
            if (!record->Result.has_value())
            {
                return false;
            }
        }

        m_jobs.erase(iterator);
    }

    if (record->Worker.joinable())
    {
        record->Worker.join();
    }

    std::error_code removeError;
    std::filesystem::remove_all(record->WorkingDirectory, removeError);
    if (removeError)
    {
        InpxWebReader::Logging::WarnIfInitialized(
            "INPX scan workspace cleanup failed. path='{}' error='{}'",
            InpxWebReader::Unicode::PathToUtf8(record->WorkingDirectory),
            removeError.message());
    }

    return true;
}

std::shared_ptr<CInpxScanJobService::SJobRecord> CInpxScanJobService::TryGetRecord(
    const TInpxScanJobId jobId) const
{
    const std::scoped_lock lock(m_jobsMutex);
    const auto iterator = m_jobs.find(jobId);
    return iterator == m_jobs.end() ? nullptr : iterator->second;
}

} // namespace InpxWebReader::ApplicationJobs
