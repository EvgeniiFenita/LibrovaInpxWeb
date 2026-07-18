#include "Database/SearchIndexMaintenance.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "Database/SqliteStatement.hpp"
#include "Domain/MetadataNormalization.hpp"

namespace InpxWebReader::SearchIndex {
namespace {

struct SNormalizedSearchDocument
{
    std::string Title;
    std::string Authors;
    std::string Tags;
    std::string Genres;
    std::string Description;
};

std::string JoinNormalizedText(
    const std::vector<std::string>& values,
    const bool sortValues)
{
    std::vector<std::string> normalizedValues;
    normalizedValues.reserve(values.size());

    for (const std::string& value : values)
    {
        const std::string normalized = InpxWebReader::Domain::NormalizeText(value);
        if (!normalized.empty())
        {
            normalizedValues.push_back(normalized);
        }
    }

    if (sortValues)
    {
        std::ranges::sort(normalizedValues);
    }

    std::string joined;
    for (const std::string& normalized : normalizedValues)
    {
        if (!joined.empty())
        {
            joined.push_back(' ');
        }

        joined.append(normalized);
    }

    return joined;
}

std::string BuildNormalizedDescription(const std::optional<std::string>& description)
{
    if (!description.has_value())
    {
        return {};
    }

    return InpxWebReader::Domain::NormalizeText(*description);
}

[[nodiscard]] SNormalizedSearchDocument BuildNormalizedSearchDocument(const InpxWebReader::Domain::SBookMetadata& metadata)
{
    return {
        .Title = InpxWebReader::Domain::NormalizeText(metadata.TitleUtf8),
        .Authors = JoinNormalizedText(metadata.AuthorsUtf8, false),
        .Tags = JoinNormalizedText(metadata.TagsUtf8, true),
        .Genres = JoinNormalizedText(metadata.GenresUtf8, true),
        .Description = BuildNormalizedDescription(metadata.DescriptionUtf8),
    };
}

void BindSearchDocument(
    InpxWebReader::Sqlite::CSqliteStatement& statement,
    const std::int64_t bookId,
    const SNormalizedSearchDocument& document)
{
    statement.BindInt64(1, bookId);
    statement.BindText(2, document.Title);
    statement.BindText(3, document.Authors);
    statement.BindText(4, document.Tags);
    statement.BindText(5, document.Genres);
    statement.BindText(6, document.Description);
}

void ExecuteDeleteCommand(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::int64_t bookId,
    const InpxWebReader::Domain::SBookMetadata& metadata)
{
    // Contentless FTS5 delete rows must bind the exact same normalized document
    // shape as the original insert row. If insert/delete drift apart, the old
    // tokens survive in search_index even after the book row is removed.
    const SNormalizedSearchDocument document = BuildNormalizedSearchDocument(metadata);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "INSERT INTO search_index(search_index, rowid, title, authors, tags, genres, description) VALUES('delete', ?, ?, ?, ?, ?, ?);");
    BindSearchDocument(statement, bookId, document);
    static_cast<void>(statement.Step());
}

} // namespace

void CSearchIndexMaintenance::InsertBook(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::int64_t bookId,
    const InpxWebReader::Domain::SBookMetadata& metadata)
{
    const SNormalizedSearchDocument document = BuildNormalizedSearchDocument(metadata);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "INSERT INTO search_index(rowid, title, authors, tags, genres, description) VALUES(?, ?, ?, ?, ?, ?);");
    BindSearchDocument(statement, bookId, document);
    static_cast<void>(statement.Step());
}

void CSearchIndexMaintenance::RemoveBook(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::int64_t bookId,
    const InpxWebReader::Domain::SBookMetadata& metadata)
{
    ExecuteDeleteCommand(connection, bookId, metadata);
}

} // namespace InpxWebReader::SearchIndex
