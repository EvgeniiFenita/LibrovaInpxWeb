#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "Domain/MetadataNormalization.hpp"
#include "Database/SqliteConnection.hpp"
#include "Database/SqliteStatement.hpp"

namespace InpxWebReader::BookDatabase {

class CSqliteGenreHelpers final
{
public:
    // Upserts a genre record and returns its rowid.
    // The genre is keyed on normalizedName; if it already exists the existing
    // record is kept unchanged and only the id is returned.
    [[nodiscard]] static std::int64_t ResolveGenreId(
        const InpxWebReader::Sqlite::CSqliteConnection& connection,
        std::string_view normalizedName,
        std::string_view displayName);

    // Inserts genres into book_genres for the given book id.
    // Duplicate display names (normalized) within the same call are silently
    // skipped so the function is safe to call multiple times (idempotent per row).
    static void InsertGenres(
        const InpxWebReader::Sqlite::CSqliteConnection& connection,
        std::int64_t bookId,
        const std::vector<std::string>& genres);
};

} // namespace InpxWebReader::BookDatabase
