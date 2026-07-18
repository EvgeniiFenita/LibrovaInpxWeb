#include "Database/SqliteGenreHelpers.hpp"

#include <unordered_set>

#include "Database/SqliteEntityHelpers.hpp"
#include "Database/SqliteStatement.hpp"
#include "Domain/MetadataNormalization.hpp"

namespace InpxWebReader::BookDatabase {

std::int64_t CSqliteGenreHelpers::ResolveGenreId(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    std::string_view normalizedName,
    std::string_view displayName)
{
    return InpxWebReader::Sqlite::ResolveEntityId(connection, "genres", normalizedName, displayName);
}

void CSqliteGenreHelpers::InsertGenres(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    std::int64_t bookId,
    const std::vector<std::string>& genres)
{
    std::unordered_set<std::string> inserted;

    for (const std::string& genre : genres)
    {
        const std::string normalized = InpxWebReader::Domain::NormalizeText(genre);

        if (normalized.empty() || !inserted.insert(normalized).second)
        {
            continue;
        }

        const std::int64_t genreId = ResolveGenreId(connection, normalized, genre);

        InpxWebReader::Sqlite::CSqliteStatement linkStatement(
            connection.GetNativeHandle(),
            "INSERT OR IGNORE INTO book_genres (book_id, genre_id) VALUES (?, ?);");
        linkStatement.BindInt64(1, bookId);
        linkStatement.BindInt64(2, genreId);
        static_cast<void>(linkStatement.Step());
    }
}

} // namespace InpxWebReader::BookDatabase
