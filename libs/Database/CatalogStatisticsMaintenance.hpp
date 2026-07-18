#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "Database/SqliteConnection.hpp"
#include "Domain/BookRepository.hpp"

namespace InpxWebReader::BookDatabase {

class CCatalogStatisticsMaintenance final
{
public:
    [[nodiscard]] static std::filesystem::path ResolveCacheRoot(
        const std::filesystem::path& databasePath);
    [[nodiscard]] static std::uint64_t GetFileSizeOrZero(const std::filesystem::path& path);
    [[nodiscard]] static std::uint64_t GetDatabaseFootprintSize(
        const std::filesystem::path& databasePath);

    [[nodiscard]] static InpxWebReader::Domain::IBookQueryRepository::SCatalogStatistics Read(
        const InpxWebReader::Sqlite::CSqliteConnection& connection,
        const std::filesystem::path& databasePath);

    static void RecordInpxBookInserted(
        const InpxWebReader::Sqlite::CSqliteConnection& connection,
        const std::filesystem::path& databasePath,
        const std::optional<std::string>& coverPathUtf8,
        std::optional<std::uint64_t> knownCoverSizeBytes);

    static void RecordCoverPathChanged(
        const InpxWebReader::Sqlite::CSqliteConnection& connection,
        const std::filesystem::path& databasePath,
        const std::optional<std::string>& oldCoverPathUtf8,
        const std::optional<std::string>& newCoverPathUtf8,
        std::optional<std::uint64_t> knownNewCoverSizeBytes);

    static void RecordInpxBookAvailabilityChanged(
        const InpxWebReader::Sqlite::CSqliteConnection& connection,
        std::string_view oldAvailabilityUtf8,
        std::string_view newAvailabilityUtf8);

    static void AddUnavailableBooks(
        const InpxWebReader::Sqlite::CSqliteConnection& connection,
        std::uint64_t count);

    static void RemoveUnavailableBooks(
        const InpxWebReader::Sqlite::CSqliteConnection& connection,
        std::uint64_t count);

    static void RecomputeInpxSourceSize(
        const InpxWebReader::Sqlite::CSqliteConnection& connection,
        const std::filesystem::path& inpxPath,
        const std::filesystem::path& archiveRoot);
};

} // namespace InpxWebReader::BookDatabase
