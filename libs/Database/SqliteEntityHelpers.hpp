#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Database/SqliteConnection.hpp"

namespace InpxWebReader::Sqlite {

// Builds an SQL IN-clause placeholder like "(?, ?, ?)" for `count` parameters.
[[nodiscard]] std::string BuildIdInClause(std::size_t count);

// INSERT INTO <tableName> (normalized_name, display_name) ON CONFLICT DO NOTHING,
// then SELECT id FROM <tableName> WHERE normalized_name = ?
// Throws on failure.
[[nodiscard]] std::int64_t ResolveEntityId(
    const CSqliteConnection& connection,
    std::string_view tableName,
    std::string_view normalizedName,
    std::string_view displayName);

// Execute `sql` (with a single ? for bookId) and return all column-0 text values.
[[nodiscard]] std::vector<std::string> ReadRelatedEntityNames(
    const CSqliteConnection& connection,
    std::string_view sql,
    std::int64_t bookId);

// Execute `sqlTemplate` (with {} placeholder replaced by an IN clause) and return
// a map from book_id (column 0) to a list of display names (column 1).
[[nodiscard]] std::unordered_map<std::int64_t, std::vector<std::string>> ReadRelatedEntityNamesBatch(
    const CSqliteConnection& connection,
    std::string_view sqlTemplate,
    const std::vector<std::int64_t>& bookIds);

} // namespace InpxWebReader::Sqlite
