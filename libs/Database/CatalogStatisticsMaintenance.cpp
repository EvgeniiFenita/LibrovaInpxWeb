#include "Database/CatalogStatisticsMaintenance.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <set>
#include <stdexcept>

#include "Database/SqliteStatement.hpp"
#include "Database/SqliteTimePoint.hpp"
#include "Domain/InpxBookAvailability.hpp"
#include "Foundation/FileSystemUtils.hpp"
#include "Foundation/Logging.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "Storage/InpxCacheLayout.hpp"
#include "Storage/PathSafety.hpp"

namespace InpxWebReader::BookDatabase {
namespace {

struct SInpxSourceSnapshot
{
    std::uint64_t TotalSizeBytes = 0;
};

using TSteadyTimePoint = std::chrono::steady_clock::time_point;

[[nodiscard]] auto ElapsedMilliseconds(const TSteadyTimePoint startedAt) noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt).count();
}

[[nodiscard]] std::int64_t ToSqliteInt64(const std::uint64_t value)
{
    if (value > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
    {
        throw std::overflow_error("Catalog statistics value exceeds SQLite int64 range.");
    }

    return static_cast<std::int64_t>(value);
}

[[nodiscard]] std::string SerializeNow()
{
    return InpxWebReader::Sqlite::SerializeTimePoint(std::chrono::system_clock::now());
}

void EnsureStatisticsRow(const InpxWebReader::Sqlite::CSqliteConnection& connection)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "INSERT INTO catalog_statistics "
        "(singleton, book_count, unavailable_book_count, "
        " cover_cache_size_bytes, inpx_source_size_bytes, updated_at_utc) "
        "VALUES (1, 0, 0, 0, 0, ?) "
        "ON CONFLICT(singleton) DO NOTHING;");
    statement.BindText(1, SerializeNow());
    static_cast<void>(statement.Step());
}

void AddStatisticsDelta(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::int64_t bookCountDelta,
    const std::int64_t unavailableBookCountDelta,
    const std::int64_t coverCacheSizeDelta,
    const std::int64_t inpxSourceSizeDelta)
{
    EnsureStatisticsRow(connection);

    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "UPDATE catalog_statistics SET "
        "book_count = book_count + ?, "
        "unavailable_book_count = unavailable_book_count + ?, "
        "cover_cache_size_bytes = cover_cache_size_bytes + ?, "
        "inpx_source_size_bytes = inpx_source_size_bytes + ?, "
        "updated_at_utc = ? "
        "WHERE singleton = 1;");
    statement.BindInt64(1, bookCountDelta);
    statement.BindInt64(2, unavailableBookCountDelta);
    statement.BindInt64(3, coverCacheSizeDelta);
    statement.BindInt64(4, inpxSourceSizeDelta);
    statement.BindText(5, SerializeNow());
    static_cast<void>(statement.Step());
}

struct SNormalizedCoverPath
{
    std::string StoredPathUtf8;
    std::filesystem::path RelativeToCovers;
};

[[nodiscard]] SNormalizedCoverPath NormalizeCoverPath(const std::string_view storedPathUtf8)
{
    const std::filesystem::path storedPath =
        InpxWebReader::Unicode::PathFromUtf8(storedPathUtf8).lexically_normal();
    if (!InpxWebReader::Foundation::IsSafeRelativePath(storedPath))
    {
        throw std::runtime_error("Stored cover path is not a safe relative cache path.");
    }

    const auto coversPrefix = InpxWebReader::StoragePlanning::CInpxCacheLayout::Build(
        std::filesystem::path{}).CoversDirectory;
    const auto relativeCoverPath = storedPath.lexically_relative(coversPrefix);
    if (relativeCoverPath.empty()
        || relativeCoverPath == "."
        || !InpxWebReader::Foundation::IsSafeRelativePath(relativeCoverPath))
    {
        throw std::runtime_error("Stored cover path must reference a file under the Covers directory.");
    }

    return {
        .StoredPathUtf8 = InpxWebReader::Unicode::PathToUtf8(coversPrefix / relativeCoverPath),
        .RelativeToCovers = relativeCoverPath
    };
}

[[nodiscard]] std::optional<std::string> TryNormalizeCoverPath(const std::string_view storedPathUtf8)
{
    try
    {
        return NormalizeCoverPath(storedPathUtf8).StoredPathUtf8;
    }
    catch (const std::exception& ex)
    {
        InpxWebReader::Logging::WarnIfInitialized(
            "Skipping stored cover path '{}' while maintaining catalog statistics: {}",
            storedPathUtf8,
            ex.what());
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::uint64_t> GetCoverSize(
    const std::filesystem::path& cacheRoot,
    const std::string& normalizedCoverPathUtf8,
    const std::optional<std::uint64_t> knownSizeBytes)
{
    try
    {
        const auto normalizedCoverPath = NormalizeCoverPath(normalizedCoverPathUtf8);
        const auto layout = InpxWebReader::StoragePlanning::CInpxCacheLayout::Build(cacheRoot);
        const auto coverPath = InpxWebReader::SafePaths::TryResolvePathWithinRoot(
            layout.CoversDirectory,
            normalizedCoverPath.RelativeToCovers,
            "Stored cover path must stay under the Covers directory.",
            "Stored cover path could not be canonicalized.");
        if (!coverPath.has_value())
        {
            return std::nullopt;
        }

        std::error_code errorCode;
        if (!std::filesystem::is_regular_file(*coverPath, errorCode) || errorCode)
        {
            return std::nullopt;
        }

        if (knownSizeBytes.has_value())
        {
            return knownSizeBytes;
        }

        return CCatalogStatisticsMaintenance::GetFileSizeOrZero(*coverPath);
    }
    catch (const std::exception& ex)
    {
        InpxWebReader::Logging::WarnIfInitialized(
            "Skipping stored cover path '{}' while maintaining catalog statistics: {}",
            normalizedCoverPathUtf8,
            ex.what());
        return std::nullopt;
    }
}

void IncrementCoverReference(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::filesystem::path& cacheRoot,
    const std::optional<std::string>& coverPathUtf8,
    const std::optional<std::uint64_t> knownSizeBytes)
{
    if (!coverPathUtf8.has_value() || coverPathUtf8->empty())
    {
        return;
    }

    const auto normalizedCoverPathUtf8 = TryNormalizeCoverPath(*coverPathUtf8);
    if (!normalizedCoverPathUtf8.has_value())
    {
        return;
    }

    {
        InpxWebReader::Sqlite::CSqliteStatement selectStatement(
            connection.GetNativeHandle(),
            "SELECT reference_count FROM catalog_stat_cover_files WHERE cover_path = ?;");
        selectStatement.BindText(1, *normalizedCoverPathUtf8);
        if (selectStatement.Step())
        {
            InpxWebReader::Sqlite::CSqliteStatement updateStatement(
                connection.GetNativeHandle(),
                "UPDATE catalog_stat_cover_files "
                "SET reference_count = reference_count + 1 "
                "WHERE cover_path = ?;");
            updateStatement.BindText(1, *normalizedCoverPathUtf8);
            static_cast<void>(updateStatement.Step());
            return;
        }
    }

    const auto coverSize = GetCoverSize(cacheRoot, *normalizedCoverPathUtf8, knownSizeBytes);
    if (!coverSize.has_value())
    {
        return;
    }

    InpxWebReader::Sqlite::CSqliteStatement insertStatement(
        connection.GetNativeHandle(),
        "INSERT INTO catalog_stat_cover_files (cover_path, reference_count, size_bytes) "
        "VALUES (?, 1, ?);");
    insertStatement.BindText(1, *normalizedCoverPathUtf8);
    insertStatement.BindInt64(2, ToSqliteInt64(*coverSize));
    static_cast<void>(insertStatement.Step());
    AddStatisticsDelta(connection, 0, 0, ToSqliteInt64(*coverSize), 0);
}

[[nodiscard]] std::uint64_t CountStoredCoverReferences(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::string& normalizedCoverPathUtf8)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT COUNT(*) FROM inpx_books WHERE cover_path = ?;");
    statement.BindText(1, normalizedCoverPathUtf8);
    if (!statement.Step())
    {
        return 0;
    }

    return static_cast<std::uint64_t>(statement.GetColumnInt64(0));
}

void RefreshCoverReferenceSize(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::filesystem::path& cacheRoot,
    const std::optional<std::string>& coverPathUtf8,
    const std::optional<std::uint64_t> knownSizeBytes)
{
    if (!coverPathUtf8.has_value() || coverPathUtf8->empty())
    {
        return;
    }

    const auto normalizedCoverPathUtf8 = TryNormalizeCoverPath(*coverPathUtf8);
    if (!normalizedCoverPathUtf8.has_value())
    {
        return;
    }

    const auto coverSize = GetCoverSize(cacheRoot, *normalizedCoverPathUtf8, knownSizeBytes);
    if (!coverSize.has_value())
    {
        return;
    }

    InpxWebReader::Sqlite::CSqliteStatement selectStatement(
        connection.GetNativeHandle(),
        "SELECT size_bytes FROM catalog_stat_cover_files WHERE cover_path = ?;");
    selectStatement.BindText(1, *normalizedCoverPathUtf8);
    if (selectStatement.Step())
    {
        const std::int64_t oldSizeBytes = selectStatement.GetColumnInt64(0);
        const std::int64_t newSizeBytes = ToSqliteInt64(*coverSize);
        if (oldSizeBytes == newSizeBytes)
        {
            return;
        }

        InpxWebReader::Sqlite::CSqliteStatement updateStatement(
            connection.GetNativeHandle(),
            "UPDATE catalog_stat_cover_files "
            "SET size_bytes = ? "
            "WHERE cover_path = ?;");
        updateStatement.BindInt64(1, newSizeBytes);
        updateStatement.BindText(2, *normalizedCoverPathUtf8);
        static_cast<void>(updateStatement.Step());
        AddStatisticsDelta(connection, 0, 0, newSizeBytes - oldSizeBytes, 0);
        return;
    }

    const std::uint64_t referenceCount = CountStoredCoverReferences(connection, *normalizedCoverPathUtf8);
    if (referenceCount == 0)
    {
        return;
    }

    InpxWebReader::Sqlite::CSqliteStatement insertStatement(
        connection.GetNativeHandle(),
        "INSERT INTO catalog_stat_cover_files (cover_path, reference_count, size_bytes) "
        "VALUES (?, ?, ?);");
    insertStatement.BindText(1, *normalizedCoverPathUtf8);
    insertStatement.BindInt64(2, ToSqliteInt64(referenceCount));
    insertStatement.BindInt64(3, ToSqliteInt64(*coverSize));
    static_cast<void>(insertStatement.Step());
    AddStatisticsDelta(connection, 0, 0, ToSqliteInt64(*coverSize), 0);
}

void DecrementCoverReference(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::optional<std::string>& coverPathUtf8)
{
    if (!coverPathUtf8.has_value() || coverPathUtf8->empty())
    {
        return;
    }

    const auto normalizedCoverPathUtf8 = TryNormalizeCoverPath(*coverPathUtf8);
    if (!normalizedCoverPathUtf8.has_value())
    {
        return;
    }

    InpxWebReader::Sqlite::CSqliteStatement selectStatement(
        connection.GetNativeHandle(),
        "SELECT reference_count, size_bytes FROM catalog_stat_cover_files WHERE cover_path = ?;");
    selectStatement.BindText(1, *normalizedCoverPathUtf8);
    if (!selectStatement.Step())
    {
        return;
    }

    const std::int64_t referenceCount = selectStatement.GetColumnInt64(0);
    const std::int64_t sizeBytes = selectStatement.GetColumnInt64(1);
    if (referenceCount > 1)
    {
        InpxWebReader::Sqlite::CSqliteStatement updateStatement(
            connection.GetNativeHandle(),
            "UPDATE catalog_stat_cover_files "
            "SET reference_count = reference_count - 1 "
            "WHERE cover_path = ?;");
        updateStatement.BindText(1, *normalizedCoverPathUtf8);
        static_cast<void>(updateStatement.Step());
        return;
    }

    InpxWebReader::Sqlite::CSqliteStatement deleteStatement(
        connection.GetNativeHandle(),
        "DELETE FROM catalog_stat_cover_files WHERE cover_path = ?;");
    deleteStatement.BindText(1, *normalizedCoverPathUtf8);
    static_cast<void>(deleteStatement.Step());
    AddStatisticsDelta(connection, 0, 0, -sizeBytes, 0);
}

void SetInpxSourceSize(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::uint64_t sizeBytes)
{
    EnsureStatisticsRow(connection);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "UPDATE catalog_statistics "
        "SET inpx_source_size_bytes = ?, updated_at_utc = ? "
        "WHERE singleton = 1;");
    statement.BindInt64(1, ToSqliteInt64(sizeBytes));
    statement.BindText(2, SerializeNow());
    static_cast<void>(statement.Step());
}

void AddRegularFileToSnapshot(
    SInpxSourceSnapshot& snapshot,
    std::set<std::filesystem::path>& countedPaths,
    const std::filesystem::path& path)
{
    std::error_code errorCode;
    if (!std::filesystem::is_regular_file(path, errorCode) || errorCode)
    {
        return;
    }

    const auto canonicalPath = std::filesystem::weakly_canonical(path, errorCode);
    if (errorCode)
    {
        return;
    }

    const auto normalizedPath = canonicalPath.lexically_normal();
    if (!countedPaths.insert(normalizedPath).second)
    {
        return;
    }

    snapshot.TotalSizeBytes += CCatalogStatisticsMaintenance::GetFileSizeOrZero(normalizedPath);
}

[[nodiscard]] std::optional<std::filesystem::path> TryResolvePersistedArchivePath(
    const std::filesystem::path& archiveRoot,
    const std::string_view relativeArchivePathUtf8)
{
    try
    {
        const auto relativePath = InpxWebReader::Unicode::PathFromUtf8(
            relativeArchivePathUtf8).lexically_normal();
        if (!InpxWebReader::Foundation::IsSafeRelativePath(relativePath))
        {
            throw std::runtime_error("Persisted INPX archive path is unsafe.");
        }
        return InpxWebReader::SafePaths::TryResolvePathWithinRoot(
            archiveRoot,
            relativePath,
            "Persisted INPX archive path is unsafe.",
            "INPX archive root could not be canonicalized while maintaining statistics.");
    }
    catch (const std::exception& ex)
    {
        InpxWebReader::Logging::WarnIfInitialized(
            "Skipping persisted INPX archive path '{}' while maintaining catalog statistics: {}",
            relativeArchivePathUtf8,
            ex.what());
    }

    return std::nullopt;
}

[[nodiscard]] SInpxSourceSnapshot CollectInpxSourceSnapshot(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::filesystem::path& inpxPath,
    const std::filesystem::path& archiveRoot)
{
    InpxWebReader::Sqlite::CSqliteStatement sourceStatement(
        connection.GetNativeHandle(),
        "SELECT id "
        "FROM inpx_sources "
        "ORDER BY id LIMIT 1;");

    if (!sourceStatement.Step())
    {
        return {};
    }

    SInpxSourceSnapshot snapshot;
    std::set<std::filesystem::path> countedPaths;
    const std::int64_t sourceId = sourceStatement.GetColumnInt64(0);
    AddRegularFileToSnapshot(snapshot, countedPaths, inpxPath);

    if (archiveRoot.empty())
    {
        return snapshot;
    }

    InpxWebReader::Sqlite::CSqliteStatement archiveStatement(
        connection.GetNativeHandle(),
        "SELECT DISTINCT s.resolved_archive_path "
        "FROM inpx_book_locations l "
        "INNER JOIN inpx_segments s ON s.id = l.segment_id "
        "WHERE l.source_id = ? AND s.resolved_archive_path IS NOT NULL;");
    archiveStatement.BindInt64(1, sourceId);

    while (archiveStatement.Step())
    {
        const std::string relativeArchivePathUtf8 = archiveStatement.GetColumnText(0);
        const auto archivePath = TryResolvePersistedArchivePath(
            archiveRoot,
            relativeArchivePathUtf8);
        if (!archivePath.has_value())
        {
            continue;
        }

        AddRegularFileToSnapshot(snapshot, countedPaths, *archivePath);
    }

    return snapshot;
}

[[nodiscard]] bool IsUnavailable(const std::string_view availabilityUtf8)
{
    return !availabilityUtf8.empty() && !InpxWebReader::Domain::IsInpxBookAvailable(availabilityUtf8);
}

} // namespace

std::filesystem::path CCatalogStatisticsMaintenance::ResolveCacheRoot(const std::filesystem::path& databasePath)
{
    return InpxWebReader::StoragePlanning::CInpxCacheLayout::ResolveCacheRootFromDatabasePath(databasePath);
}

std::uint64_t CCatalogStatisticsMaintenance::GetFileSizeOrZero(const std::filesystem::path& path)
{
    std::error_code errorCode;
    const std::uintmax_t size = std::filesystem::file_size(path, errorCode);

    if (errorCode)
    {
        return 0;
    }

    return static_cast<std::uint64_t>(size);
}

std::uint64_t CCatalogStatisticsMaintenance::GetDatabaseFootprintSize(
    const std::filesystem::path& databasePath)
{
    std::filesystem::path walPath = databasePath;
    walPath += "-wal";
    return GetFileSizeOrZero(databasePath) + GetFileSizeOrZero(walPath);
}

InpxWebReader::Domain::IBookQueryRepository::SCatalogStatistics CCatalogStatisticsMaintenance::Read(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::filesystem::path& databasePath)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT book_count, unavailable_book_count, "
        "       cover_cache_size_bytes, inpx_source_size_bytes "
        "FROM catalog_statistics WHERE singleton = 1;");
    if (!statement.Step())
    {
        throw std::runtime_error("Catalog statistics singleton is missing.");
    }

    const std::uint64_t bookCount = static_cast<std::uint64_t>(statement.GetColumnInt64(0));
    const std::uint64_t unavailableBookCount = static_cast<std::uint64_t>(statement.GetColumnInt64(1));
    const std::uint64_t coverCacheSizeBytes = static_cast<std::uint64_t>(statement.GetColumnInt64(2));
    const std::uint64_t inpxSourceSizeBytes = static_cast<std::uint64_t>(statement.GetColumnInt64(3));
    const std::uint64_t databaseSizeBytes = GetDatabaseFootprintSize(databasePath);

    return {
        .BookCount = bookCount,
        .UnavailableBookCount = unavailableBookCount,
        .InpxSourceSizeBytes = inpxSourceSizeBytes,
        .CoverCacheSizeBytes = coverCacheSizeBytes,
        .DatabaseSizeBytes = databaseSizeBytes,
        .TotalCatalogSizeBytes = coverCacheSizeBytes + inpxSourceSizeBytes + databaseSizeBytes
    };
}

void CCatalogStatisticsMaintenance::RecordInpxBookInserted(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::filesystem::path& databasePath,
    const std::optional<std::string>& coverPathUtf8,
    const std::optional<std::uint64_t> knownCoverSizeBytes)
{
    AddStatisticsDelta(connection, 1, 0, 0, 0);
    IncrementCoverReference(
        connection,
        ResolveCacheRoot(databasePath),
        coverPathUtf8,
        knownCoverSizeBytes);
}

void CCatalogStatisticsMaintenance::RecordCoverPathChanged(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::filesystem::path& databasePath,
    const std::optional<std::string>& oldCoverPathUtf8,
    const std::optional<std::string>& newCoverPathUtf8,
    const std::optional<std::uint64_t> knownNewCoverSizeBytes)
{
    const auto oldNormalizedCoverPathUtf8 =
        oldCoverPathUtf8.has_value() ? TryNormalizeCoverPath(*oldCoverPathUtf8) : std::nullopt;
    const auto newNormalizedCoverPathUtf8 =
        newCoverPathUtf8.has_value() ? TryNormalizeCoverPath(*newCoverPathUtf8) : std::nullopt;

    if (oldNormalizedCoverPathUtf8 == newNormalizedCoverPathUtf8)
    {
        RefreshCoverReferenceSize(
            connection,
            ResolveCacheRoot(databasePath),
            newNormalizedCoverPathUtf8,
            knownNewCoverSizeBytes);
        return;
    }

    DecrementCoverReference(connection, oldNormalizedCoverPathUtf8);
    IncrementCoverReference(
        connection,
        ResolveCacheRoot(databasePath),
        newNormalizedCoverPathUtf8,
        knownNewCoverSizeBytes);
}

void CCatalogStatisticsMaintenance::RecordInpxBookAvailabilityChanged(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::string_view oldAvailabilityUtf8,
    const std::string_view newAvailabilityUtf8)
{
    const bool wasUnavailable = IsUnavailable(oldAvailabilityUtf8);
    const bool isUnavailable = IsUnavailable(newAvailabilityUtf8);
    if (wasUnavailable == isUnavailable)
    {
        return;
    }

    AddStatisticsDelta(connection, 0, isUnavailable ? 1 : -1, 0, 0);
}

void CCatalogStatisticsMaintenance::AddUnavailableBooks(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::uint64_t count)
{
    if (count == 0)
    {
        return;
    }

    AddStatisticsDelta(connection, 0, ToSqliteInt64(count), 0, 0);
}

void CCatalogStatisticsMaintenance::RemoveUnavailableBooks(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::uint64_t count)
{
    if (count == 0)
    {
        return;
    }

    AddStatisticsDelta(connection, 0, -ToSqliteInt64(count), 0, 0);
}

void CCatalogStatisticsMaintenance::RecomputeInpxSourceSize(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::filesystem::path& inpxPath,
    const std::filesystem::path& archiveRoot)
{
    const auto startedAt = std::chrono::steady_clock::now();
    const auto snapshot = CollectInpxSourceSnapshot(connection, inpxPath, archiveRoot);
    SetInpxSourceSize(connection, snapshot.TotalSizeBytes);
    InpxWebReader::Logging::InfoIfInitialized(
        "INPX source size recomputed: sourceBytes={} elapsed={}ms",
        snapshot.TotalSizeBytes,
        ElapsedMilliseconds(startedAt));
}

} // namespace InpxWebReader::BookDatabase
