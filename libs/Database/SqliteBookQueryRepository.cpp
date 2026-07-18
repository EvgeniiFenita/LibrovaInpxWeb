#include "Database/SqliteBookQueryRepository.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sqlite3.h>
#include <sys/random.h>

#include "Database/CatalogStatisticsMaintenance.hpp"
#include "Database/SqliteConnection.hpp"
#include "Database/SqliteEntityHelpers.hpp"
#include "Database/SqliteStatement.hpp"
#include "Database/SqliteTimePoint.hpp"
#include "Database/SqliteTransaction.hpp"
#include "Domain/BookFormat.hpp"
#include "Domain/DomainError.hpp"
#include "Domain/InpxBookAvailability.hpp"
#include "Domain/MetadataNormalization.hpp"

namespace InpxWebReader::BookDatabase {
namespace {

void AppendColumnScope(std::string& scope, const std::string_view column, bool& first)
{
    if (!first)
    {
        scope.push_back(' ');
    }
    scope.append(column);
    first = false;
}

[[nodiscard]] std::runtime_error BuildSqliteError(
    sqlite3* connection,
    const std::string_view context)
{
    return std::runtime_error(
        std::string{context} + ": " + (connection != nullptr ? sqlite3_errmsg(connection) : "unknown SQLite error"));
}

[[nodiscard]] fts5_api* GetFts5Api(sqlite3* connection)
{
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(connection, "SELECT fts5(?1);", -1, &statement, nullptr) != SQLITE_OK)
    {
        throw BuildSqliteError(connection, "Failed to access the FTS5 tokenizer API");
    }

    fts5_api* api = nullptr;
    const int bindResult = sqlite3_bind_pointer(
        statement,
        1,
        static_cast<void*>(&api),
        "fts5_api_ptr",
        nullptr);
    const int stepResult = bindResult == SQLITE_OK ? sqlite3_step(statement) : bindResult;
    const int finalizeResult = sqlite3_finalize(statement);
    if ((stepResult != SQLITE_ROW && stepResult != SQLITE_DONE) || finalizeResult != SQLITE_OK || api == nullptr)
    {
        throw BuildSqliteError(connection, "Failed to initialize the FTS5 tokenizer API");
    }
    return api;
}

struct STokenizeContext
{
    std::vector<std::string> Tokens;
    std::exception_ptr Error;
};

int CollectFtsToken(
    void* context,
    int,
    const char* token,
    const int tokenLength,
    int,
    int) noexcept
{
    auto& tokenizeContext = *static_cast<STokenizeContext*>(context);
    try
    {
        if (tokenLength > 0)
        {
            tokenizeContext.Tokens.emplace_back(token, static_cast<std::size_t>(tokenLength));
        }
        return SQLITE_OK;
    }
    catch (...)
    {
        tokenizeContext.Error = std::current_exception();
        return SQLITE_NOMEM;
    }
}

[[nodiscard]] std::vector<std::string> TokenizeFtsQuery(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::string& textUtf8)
{
    if (textUtf8.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        throw std::invalid_argument("Search text exceeds the FTS5 input limit.");
    }

    fts5_api* api = GetFts5Api(connection.GetNativeHandle());
    void* tokenizerContext = nullptr;
    fts5_tokenizer tokenizer{};
    if (api->xFindTokenizer(api, "unicode61", &tokenizerContext, &tokenizer) != SQLITE_OK)
    {
        throw BuildSqliteError(connection.GetNativeHandle(), "Failed to resolve the unicode61 FTS5 tokenizer");
    }

    Fts5Tokenizer* tokenizerInstance = nullptr;
    if (tokenizer.xCreate(tokenizerContext, nullptr, 0, &tokenizerInstance) != SQLITE_OK
        || tokenizerInstance == nullptr)
    {
        throw BuildSqliteError(connection.GetNativeHandle(), "Failed to create the unicode61 FTS5 tokenizer");
    }

    STokenizeContext result;
    const int tokenizeResult = tokenizer.xTokenize(
        tokenizerInstance,
        &result,
        FTS5_TOKENIZE_QUERY,
        textUtf8.data(),
        static_cast<int>(textUtf8.size()),
        CollectFtsToken);
    tokenizer.xDelete(tokenizerInstance);
    if (result.Error)
    {
        std::rethrow_exception(result.Error);
    }
    if (tokenizeResult != SQLITE_OK)
    {
        throw BuildSqliteError(connection.GetNativeHandle(), "Failed to tokenize the FTS5 search query");
    }
    return result.Tokens;
}

void AppendQuotedFtsToken(std::string& target, const std::string_view token)
{
    target.push_back('"');
    for (const char character : token)
    {
        target.push_back(character);
        if (character == '"')
        {
            target.push_back('"');
        }
    }
    target += "\"*";
}

[[nodiscard]] std::string BuildFtsQuery(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const InpxWebReader::Domain::SSearchQuery& query)
{
    if (!query.HasText())
    {
        return {};
    }

    InpxWebReader::Domain::SSearchFieldScope fields = query.SearchFields;
    if (!fields.HasAnyField())
    {
        fields = {};
    }

    std::string scope = "{";
    bool first = true;
    if (fields.Title)
    {
        AppendColumnScope(scope, "title", first);
    }
    if (fields.Authors)
    {
        AppendColumnScope(scope, "authors", first);
    }
    if (fields.Description)
    {
        AppendColumnScope(scope, "description", first);
    }
    scope += "} : ";

    const auto tokens = TokenizeFtsQuery(
        connection,
        InpxWebReader::Domain::NormalizeText(query.TextUtf8));
    if (tokens.empty())
    {
        return scope + "\"\"";
    }

    std::string result;
    for (const auto& token : tokens)
    {
        if (!result.empty())
        {
            result.push_back(' ');
        }
        result += scope;
        AppendQuotedFtsToken(result, token);
    }
    return result;
}

struct SFilteredFrom
{
    std::string Sql;
    std::vector<std::string> TextParameters;
};

void AppendFilters(
    SFilteredFrom& filtered,
    const InpxWebReader::Domain::SSearchQuery& query,
    const bool includeLanguages,
    const bool includeGenres,
    const std::string& ftsQuery)
{
    if (!ftsQuery.empty())
    {
        filtered.Sql += "AND search_index MATCH ? ";
        filtered.TextParameters.push_back(ftsQuery);
    }
    if (includeLanguages && !query.Languages.empty())
    {
        filtered.Sql += "AND b.language IN ";
        filtered.Sql += InpxWebReader::Sqlite::BuildIdInClause(query.Languages.size());
        filtered.Sql.push_back(' ');
        filtered.TextParameters.insert(
            filtered.TextParameters.end(),
            query.Languages.begin(),
            query.Languages.end());
    }
    if (includeGenres && !query.GenresUtf8.empty())
    {
        filtered.Sql += "AND EXISTS (SELECT 1 FROM book_genres bg_filter "
                        "INNER JOIN genres g_filter ON g_filter.id = bg_filter.genre_id "
                        "WHERE bg_filter.book_id = b.id AND g_filter.normalized_name IN ";
        filtered.Sql += InpxWebReader::Sqlite::BuildIdInClause(query.GenresUtf8.size());
        filtered.Sql += ") ";
        for (const auto& genre : query.GenresUtf8)
        {
            filtered.TextParameters.push_back(InpxWebReader::Domain::NormalizeText(genre));
        }
    }
}

[[nodiscard]] int BindTextParameters(
    InpxWebReader::Sqlite::CSqliteStatement& statement,
    const std::vector<std::string>& parameters)
{
    int index = 1;
    for (const std::string& parameter : parameters)
    {
        statement.BindText(index++, parameter);
    }
    return index;
}

void AppendSort(std::string& sql, const InpxWebReader::Domain::SSearchQuery& query)
{
    const std::string_view direction =
        query.SortDirection == InpxWebReader::Domain::ESortDirection::Descending ? "DESC" : "ASC";
    switch (query.SortBy.value_or(InpxWebReader::Domain::EBookSort::Title))
    {
    case InpxWebReader::Domain::EBookSort::Title:
        sql += std::format("ORDER BY b.normalized_title {}, b.title ASC, b.id ASC ", direction);
        break;
    case InpxWebReader::Domain::EBookSort::Author:
        sql += std::format(
            "ORDER BY author_sort.normalized_name {}, "
            "b.normalized_title ASC, b.id ASC ",
            direction);
        break;
    case InpxWebReader::Domain::EBookSort::DateAdded:
        sql += std::format("ORDER BY b.added_at_utc {}, b.normalized_title ASC, b.id ASC ", direction);
        break;
    }
}

[[nodiscard]] std::unordered_map<std::int64_t, std::vector<std::string>> ReadAuthors(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::vector<std::int64_t>& ids)
{
    return InpxWebReader::Sqlite::ReadRelatedEntityNamesBatch(
        connection,
        "SELECT ba.book_id, a.display_name FROM book_authors ba "
        "INNER JOIN authors a ON a.id = ba.author_id WHERE ba.book_id IN {} "
        "ORDER BY ba.book_id, ba.author_order;",
        ids);
}

[[nodiscard]] std::unordered_map<std::int64_t, std::vector<std::string>> ReadTags(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::vector<std::int64_t>& ids)
{
    return InpxWebReader::Sqlite::ReadRelatedEntityNamesBatch(
        connection,
        "SELECT bt.book_id, t.display_name FROM book_tags bt "
        "INNER JOIN tags t ON t.id = bt.tag_id WHERE bt.book_id IN {} "
        "ORDER BY bt.book_id, t.display_name COLLATE NOCASE;",
        ids);
}

[[nodiscard]] std::unordered_map<std::int64_t, std::vector<std::string>> ReadGenres(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::vector<std::int64_t>& ids)
{
    return InpxWebReader::Sqlite::ReadRelatedEntityNamesBatch(
        connection,
        "SELECT bg.book_id, g.display_name FROM book_genres bg "
        "INNER JOIN genres g ON g.id = bg.genre_id WHERE bg.book_id IN {} "
        "ORDER BY bg.book_id, g.display_name COLLATE NOCASE;",
        ids);
}

[[nodiscard]] std::vector<InpxWebReader::Domain::SBook> ReadBooks(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::vector<std::int64_t>& ids)
{
    if (ids.empty())
    {
        return {};
    }

    const auto authors = ReadAuthors(connection, ids);
    const auto tags = ReadTags(connection, ids);
    const auto genres = ReadGenres(connection, ids);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        std::format(
            "SELECT b.id, b.title, b.language, b.series, b.series_index, b.publisher, b.year, "
            "b.isbn, b.description, b.identifier, b.cover_path, b.added_at_utc, "
            "l.format, l.file_size_bytes, l.availability FROM inpx_books b "
            "INNER JOIN inpx_book_locations l ON l.book_id = b.id WHERE b.id IN {};",
            InpxWebReader::Sqlite::BuildIdInClause(ids.size())));
    for (int index = 0; index < static_cast<int>(ids.size()); ++index)
    {
        statement.BindInt64(index + 1, ids[static_cast<std::size_t>(index)]);
    }

    std::unordered_map<std::int64_t, InpxWebReader::Domain::SBook> byId;
    while (statement.Step())
    {
        const auto format = InpxWebReader::Domain::TryParseBookFormat(statement.GetColumnText(12));
        if (!format.has_value())
        {
            throw std::runtime_error("Stored INPX book format is invalid.");
        }

        InpxWebReader::Domain::SBook book;
        const std::int64_t id = statement.GetColumnInt64(0);
        book.Id = InpxWebReader::Domain::SBookId{id};
        book.Metadata.TitleUtf8 = statement.GetColumnText(1);
        book.Metadata.Language = statement.GetColumnText(2);
        book.Metadata.SeriesUtf8 = statement.IsColumnNull(3) ? std::nullopt : std::make_optional(statement.GetColumnText(3));
        book.Metadata.SeriesIndex = statement.IsColumnNull(4) ? std::nullopt : std::make_optional(statement.GetColumnDouble(4));
        book.Metadata.PublisherUtf8 = statement.IsColumnNull(5) ? std::nullopt : std::make_optional(statement.GetColumnText(5));
        book.Metadata.Year = statement.IsColumnNull(6) ? std::nullopt : std::make_optional(statement.GetColumnInt(6));
        book.Metadata.Isbn = statement.IsColumnNull(7) ? std::nullopt : std::make_optional(statement.GetColumnText(7));
        book.Metadata.DescriptionUtf8 = statement.IsColumnNull(8) ? std::nullopt : std::make_optional(statement.GetColumnText(8));
        book.Metadata.Identifier = statement.IsColumnNull(9) ? std::nullopt : std::make_optional(statement.GetColumnText(9));
        book.CoverPathUtf8 = statement.IsColumnNull(10) ? std::nullopt : std::make_optional(statement.GetColumnText(10));
        book.AddedAtUtc = InpxWebReader::Sqlite::ParseTimePoint(statement.GetColumnText(11));
        book.File.Format = *format;
        book.File.SizeBytes = static_cast<std::uintmax_t>(statement.GetColumnInt64(13));
        book.IsAvailable = InpxWebReader::Domain::IsInpxBookAvailable(statement.GetColumnText(14));
        if (const auto iterator = authors.find(id); iterator != authors.end())
        {
            book.Metadata.AuthorsUtf8 = iterator->second;
        }
        if (const auto iterator = tags.find(id); iterator != tags.end())
        {
            book.Metadata.TagsUtf8 = iterator->second;
        }
        if (const auto iterator = genres.find(id); iterator != genres.end())
        {
            book.Metadata.GenresUtf8 = iterator->second;
        }
        byId.emplace(id, std::move(book));
    }

    std::vector<InpxWebReader::Domain::SBook> result;
    result.reserve(ids.size());
    for (const std::int64_t id : ids)
    {
        if (auto iterator = byId.find(id); iterator != byId.end())
        {
            result.push_back(std::move(iterator->second));
        }
    }
    return result;
}

[[nodiscard]] SFilteredFrom BuildFilteredFrom(
    const InpxWebReader::Domain::SSearchQuery& query,
    const std::string& ftsQuery,
    const bool includeLanguages,
    const bool includeGenres,
    const bool includeAuthorSort = false)
{
    SFilteredFrom filtered{
        .Sql = " FROM inpx_books b "
               "INNER JOIN inpx_book_locations l_catalog ON l_catalog.book_id = b.id "
    };
    if (!ftsQuery.empty())
    {
        filtered.Sql += "INNER JOIN search_index ON search_index.rowid = b.id ";
    }
    if (includeAuthorSort)
    {
        filtered.Sql +=
            "LEFT JOIN ("
            "SELECT ba.book_id, MIN(a.normalized_name) AS normalized_name "
            "FROM book_authors ba "
            "INNER JOIN authors a ON a.id = ba.author_id "
            "GROUP BY ba.book_id"
            ") author_sort ON author_sort.book_id = b.id ";
    }
    filtered.Sql += "WHERE 1 = 1 ";
    AppendFilters(filtered, query, includeLanguages, includeGenres, ftsQuery);
    return filtered;
}

[[nodiscard]] bool UsesAuthorSort(const InpxWebReader::Domain::SSearchQuery& query) noexcept
{
    return query.SortBy.value_or(InpxWebReader::Domain::EBookSort::Title)
        == InpxWebReader::Domain::EBookSort::Author;
}

[[nodiscard]] std::int64_t ToSqlitePaginationValue(
    const std::size_t value,
    const std::string_view fieldName)
{
    constexpr auto maxSqliteValue = static_cast<std::uintmax_t>((std::numeric_limits<std::int64_t>::max)());
    if (static_cast<std::uintmax_t>(value) > maxSqliteValue)
    {
        throw std::invalid_argument(std::string{fieldName} + " exceeds the SQLite int64 range.");
    }
    return static_cast<std::int64_t>(value);
}

[[nodiscard]] std::vector<InpxWebReader::Domain::SBook> SearchBooks(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const InpxWebReader::Domain::SSearchQuery& query,
    const std::string& ftsQuery)
{
    const SFilteredFrom filtered = BuildFilteredFrom(
        query,
        ftsQuery,
        true,
        true,
        UsesAuthorSort(query));
    std::string sql = "SELECT b.id" + filtered.Sql;
    AppendSort(sql, query);
    sql += "LIMIT ? OFFSET ?;";
    InpxWebReader::Sqlite::CSqliteStatement statement(connection.GetNativeHandle(), sql);
    int index = BindTextParameters(statement, filtered.TextParameters);
    statement.BindInt64(index++, ToSqlitePaginationValue(query.Limit, "Search limit"));
    statement.BindInt64(index, ToSqlitePaginationValue(query.Offset, "Search offset"));
    std::vector<std::int64_t> ids;
    while (statement.Step())
    {
        ids.push_back(statement.GetColumnInt64(0));
    }
    return ReadBooks(connection, ids);
}

[[nodiscard]] std::vector<std::int64_t> SearchBookIds(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const InpxWebReader::Domain::SSearchQuery& query,
    const std::string& ftsQuery,
    const std::size_t expectedCount)
{
    const SFilteredFrom filtered = BuildFilteredFrom(
        query,
        ftsQuery,
        true,
        true,
        UsesAuthorSort(query));
    std::string sql = "SELECT b.id" + filtered.Sql;
    AppendSort(sql, query);
    sql.push_back(';');
    InpxWebReader::Sqlite::CSqliteStatement statement(connection.GetNativeHandle(), sql);
    static_cast<void>(BindTextParameters(statement, filtered.TextParameters));

    std::vector<std::int64_t> ids;
    ids.reserve(expectedCount);
    while (statement.Step())
    {
        ids.push_back(statement.GetColumnInt64(0));
    }
    if (ids.size() != expectedCount)
    {
        throw std::runtime_error("Catalog search count changed inside one read transaction.");
    }
    return ids;
}

[[nodiscard]] std::uint64_t CountSearchResults(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const InpxWebReader::Domain::SSearchQuery& query,
    const std::string& ftsQuery)
{
    const SFilteredFrom filtered = BuildFilteredFrom(query, ftsQuery, true, true);
    const std::string sql = "SELECT COUNT(*)" + filtered.Sql + ";";
    InpxWebReader::Sqlite::CSqliteStatement statement(connection.GetNativeHandle(), sql);
    static_cast<void>(BindTextParameters(statement, filtered.TextParameters));
    return statement.Step() ? static_cast<std::uint64_t>(statement.GetColumnInt64(0)) : 0;
}

[[nodiscard]] std::vector<InpxWebReader::Domain::SFacetItem> ListAvailableLanguages(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const InpxWebReader::Domain::SSearchQuery& query,
    const std::string& ftsQuery)
{
    const SFilteredFrom filtered = BuildFilteredFrom(query, ftsQuery, false, true);
    std::string sql = "SELECT b.language, COUNT(*)" + filtered.Sql;
    sql += "AND b.language <> '' GROUP BY b.language ORDER BY b.language COLLATE NOCASE;";
    InpxWebReader::Sqlite::CSqliteStatement statement(connection.GetNativeHandle(), sql);
    static_cast<void>(BindTextParameters(statement, filtered.TextParameters));
    std::vector<InpxWebReader::Domain::SFacetItem> result;
    while (statement.Step())
    {
        result.push_back({statement.GetColumnText(0), static_cast<std::uint32_t>(statement.GetColumnInt64(1))});
    }
    return result;
}

[[nodiscard]] std::vector<InpxWebReader::Domain::SFacetItem> ListAvailableGenres(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const InpxWebReader::Domain::SSearchQuery& query,
    const std::string& ftsQuery)
{
    SFilteredFrom filtered{
        .Sql = "SELECT g.display_name, COUNT(DISTINCT b.id) FROM inpx_books b "
               "INNER JOIN inpx_book_locations l_catalog ON l_catalog.book_id = b.id "
               "INNER JOIN book_genres bg ON bg.book_id = b.id "
               "INNER JOIN genres g ON g.id = bg.genre_id "
    };
    if (!ftsQuery.empty())
    {
        filtered.Sql += "INNER JOIN search_index ON search_index.rowid = b.id ";
    }
    filtered.Sql += "WHERE 1 = 1 ";
    AppendFilters(filtered, query, true, false, ftsQuery);
    filtered.Sql += "GROUP BY g.id, g.display_name ORDER BY g.display_name COLLATE NOCASE;";
    InpxWebReader::Sqlite::CSqliteStatement statement(connection.GetNativeHandle(), filtered.Sql);
    static_cast<void>(BindTextParameters(statement, filtered.TextParameters));
    std::vector<InpxWebReader::Domain::SFacetItem> result;
    while (statement.Step())
    {
        result.push_back({statement.GetColumnText(0), static_cast<std::uint32_t>(statement.GetColumnInt64(1))});
    }
    return result;
}

struct SCatalogIdentity
{
    std::string SourceFingerprintUtf8;
    std::string SnapshotIdUtf8;

    [[nodiscard]] bool operator==(const SCatalogIdentity&) const noexcept = default;
};

[[nodiscard]] SCatalogIdentity ReadCatalogIdentity(
    const InpxWebReader::Sqlite::CSqliteConnection& connection)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT source_fingerprint, last_seen_snapshot_id FROM inpx_sources WHERE id = 1;");
    if (!statement.Step())
    {
        throw std::runtime_error("INPX cache source metadata is missing.");
    }
    return {
        .SourceFingerprintUtf8 = statement.GetColumnText(0),
        .SnapshotIdUtf8 = statement.IsColumnNull(1) ? std::string{} : statement.GetColumnText(1)
    };
}

[[nodiscard]] SCatalogIdentity ReadCurrentCatalogIdentity(
    const std::filesystem::path& databasePath)
{
    const InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    return ReadCatalogIdentity(connection);
}

[[noreturn]] void ThrowCatalogSnapshotChanged()
{
    throw InpxWebReader::Domain::CDomainException(
        InpxWebReader::Domain::EDomainErrorCode::CatalogSnapshotChanged,
        "The catalog changed while materializing the cursor.");
}

void ValidateCurrentCatalogIdentity(
    const std::filesystem::path& databasePath,
    const SCatalogIdentity& expectedIdentity)
{
    if (ReadCurrentCatalogIdentity(databasePath) != expectedIdentity)
    {
        ThrowCatalogSnapshotChanged();
    }
}

} // namespace

struct CSqliteBookQueryRepository::SSearchSessionState
{
    struct SCriteria
    {
        std::string FtsQueryUtf8;
        std::vector<std::string> Languages;
        std::vector<std::string> GenresUtf8;
        InpxWebReader::Domain::EBookSort SortBy = InpxWebReader::Domain::EBookSort::Title;
        InpxWebReader::Domain::ESortDirection SortDirection =
            InpxWebReader::Domain::ESortDirection::Ascending;

        [[nodiscard]] bool operator==(const SCriteria&) const noexcept = default;
    };

    struct SSession
    {
        std::vector<std::int64_t> BookIds;
        SCriteria Criteria;
        std::string SourceFingerprintUtf8;
        std::string CatalogSnapshotIdUtf8;
        std::size_t FirstContinuationPosition = 0;
        std::chrono::steady_clock::time_point LastAccess;
    };

    struct SReservation
    {
        std::string GenerationKey;
        std::size_t IdCount = 0;
    };

    [[nodiscard]] static constexpr std::size_t ResolvePerSessionIdCapacity(
        const std::size_t aggregateIdCapacity) noexcept
    {
        const std::size_t reservedForOtherSessions = aggregateIdCapacity / 4
            + (aggregateIdCapacity % 4 == 0 ? 0 : 1);
        return aggregateIdCapacity - reservedForOtherSessions;
    }

    explicit SSearchSessionState(const std::size_t aggregateIdCapacity)
        : AggregateIdCapacity(aggregateIdCapacity)
        , PerSessionIdCapacity(ResolvePerSessionIdCapacity(AggregateIdCapacity))
    {
    }

    std::mutex Mutex;
    std::unordered_map<std::string, std::shared_ptr<SSession>> Sessions;
    std::map<std::uint64_t, SReservation> Reservations;
    std::string CurrentGenerationKey;
    std::size_t AggregateIdCapacity = 0;
    std::size_t PerSessionIdCapacity = 0;
    std::size_t UsedIdCount = 0;
    std::size_t ReservedIdCount = 0;
    std::uint64_t NextReservationId = 1;
};

namespace {

constexpr std::size_t GMaxSearchSessions = 8;
constexpr auto GSearchSessionTtl = std::chrono::minutes{10};
// Book ids dominate cursor-session storage. Allow twice their stored size as a
// coarse envelope for the vector and bounded session metadata.
constexpr std::uint64_t GSearchSessionBookIdAllowanceBytes = 16;

struct SSearchSessionReservation
{
    std::uint64_t Id = 0;
    std::string GenerationKey;
    std::size_t IdCount = 0;
};

[[nodiscard]] std::vector<std::string> CanonicalizeValues(
    std::vector<std::string> values,
    const bool normalize)
{
    if (normalize)
    {
        for (auto& value : values)
        {
            value = InpxWebReader::Domain::NormalizeText(value);
        }
    }
    std::ranges::sort(values);
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

[[nodiscard]] CSqliteBookQueryRepository::SSearchSessionState::SCriteria BuildSessionCriteria(
    const InpxWebReader::Domain::SSearchQuery& query,
    const std::string& ftsQuery)
{
    return {
        .FtsQueryUtf8 = ftsQuery,
        .Languages = CanonicalizeValues(query.Languages, false),
        .GenresUtf8 = CanonicalizeValues(query.GenresUtf8, true),
        .SortBy = query.SortBy.value_or(InpxWebReader::Domain::EBookSort::Title),
        .SortDirection = query.SortDirection.value_or(
            InpxWebReader::Domain::ESortDirection::Ascending)
    };
}

[[nodiscard]] bool TryAddSize(
    const std::size_t left,
    const std::size_t right,
    std::size_t& result) noexcept
{
    if (right > (std::numeric_limits<std::size_t>::max)() - left)
    {
        return false;
    }
    result = left + right;
    return true;
}

void RemoveSessionLocked(
    CSqliteBookQueryRepository::SSearchSessionState& state,
    const std::unordered_map<
        std::string,
        std::shared_ptr<CSqliteBookQueryRepository::SSearchSessionState::SSession>>::iterator& iterator)
{
    const std::size_t idCount = iterator->second->BookIds.size();
    if (idCount > state.UsedIdCount)
    {
        std::terminate();
    }
    state.UsedIdCount -= idCount;
    state.Sessions.erase(iterator);
}

void PurgeSessionsLocked(
    CSqliteBookQueryRepository::SSearchSessionState& state,
    const std::chrono::steady_clock::time_point now)
{
    for (auto iterator = state.Sessions.begin(); iterator != state.Sessions.end();)
    {
        auto& session = *iterator->second;
        const bool expired = now - session.LastAccess >= GSearchSessionTtl;
        if (!expired)
        {
            ++iterator;
            continue;
        }
        const auto current = iterator++;
        RemoveSessionLocked(state, current);
    }
}

[[nodiscard]] std::string BuildCatalogGenerationKey(const SCatalogIdentity& identity)
{
    std::string result = std::to_string(identity.SourceFingerprintUtf8.size());
    result.push_back(':');
    result.append(identity.SourceFingerprintUtf8);
    result.append(identity.SnapshotIdUtf8);
    return result;
}

void RemoveAllSessionsLocked(CSqliteBookQueryRepository::SSearchSessionState& state)
{
    for (auto iterator = state.Sessions.begin(); iterator != state.Sessions.end();)
    {
        const auto current = iterator++;
        RemoveSessionLocked(state, current);
    }
}

void ActivateSearchSessionGenerationLocked(
    CSqliteBookQueryRepository::SSearchSessionState& state,
    const std::string& generationKey)
{
    if (state.CurrentGenerationKey == generationKey)
    {
        return;
    }

    // A generation switch is the only linear reservation operation. Requests
    // within one generation keep map lookup/insert logarithmic instead of
    // repeatedly scanning all concurrent first-page reservations.
    state.CurrentGenerationKey = generationKey;
    RemoveAllSessionsLocked(state);
    state.Reservations.clear();
    state.ReservedIdCount = 0;
}

[[nodiscard]] bool RemoveReservationLocked(
    CSqliteBookQueryRepository::SSearchSessionState& state,
    const SSearchSessionReservation& reservation) noexcept
{
    const auto iterator = state.Reservations.find(reservation.Id);
    if (iterator == state.Reservations.end())
    {
        return false;
    }
    if (iterator->second.GenerationKey != reservation.GenerationKey
        || iterator->second.IdCount != reservation.IdCount
        || reservation.IdCount > state.ReservedIdCount)
    {
        std::terminate();
    }
    state.ReservedIdCount -= reservation.IdCount;
    state.Reservations.erase(iterator);
    return true;
}

[[nodiscard]] std::uint64_t NextReservationIdLocked(
    CSqliteBookQueryRepository::SSearchSessionState& state)
{
    const std::uint64_t result = state.NextReservationId;
    if (result == 0 || state.Reservations.contains(result))
    {
        throw std::runtime_error("Catalog cursor reservation id space is exhausted.");
    }
    state.NextReservationId = result == (std::numeric_limits<std::uint64_t>::max)()
        ? 1
        : result + 1;
    return result;
}

void EvictLeastRecentlyUsedLocked(CSqliteBookQueryRepository::SSearchSessionState& state)
{
    const auto iterator = std::ranges::min_element(
        state.Sessions,
        {},
        [](const auto& entry) { return entry.second->LastAccess; });
    if (iterator != state.Sessions.end())
    {
        RemoveSessionLocked(state, iterator);
    }
}

[[nodiscard]] std::string MakeUnpredictableSessionId()
{
    std::array<unsigned char, 16> bytes{};
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const ssize_t readCount = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (readCount > 0)
        {
            offset += static_cast<std::size_t>(readCount);
            continue;
        }
        if (readCount < 0 && errno == EINTR)
        {
            continue;
        }
        throw std::runtime_error("Could not generate a catalog cursor session id.");
    }
    constexpr std::string_view hexDigits = "0123456789abcdef";
    std::string result(bytes.size() * 2, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        const unsigned char byte = bytes[index];
        result[index * 2] = hexDigits[byte >> 4U];
        result[index * 2 + 1] = hexDigits[byte & 0x0FU];
    }
    return result;
}

[[nodiscard]] SSearchSessionReservation ReserveSearchSessionIds(
    CSqliteBookQueryRepository::SSearchSessionState& state,
    const std::filesystem::path& databasePath,
    const SCatalogIdentity& identity,
    const std::size_t idCount)
{
    const std::string generationKey = BuildCatalogGenerationKey(identity);
    std::scoped_lock lock(state.Mutex);
    ValidateCurrentCatalogIdentity(databasePath, identity);
    PurgeSessionsLocked(state, std::chrono::steady_clock::now());
    ActivateSearchSessionGenerationLocked(state, generationKey);
    if (idCount > state.PerSessionIdCapacity)
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::CatalogCursorCapacityExceeded,
            "The catalog result contains too many book ids for the configured cursor capacity.");
    }

    std::size_t occupied = 0;
    if (!TryAddSize(state.UsedIdCount, state.ReservedIdCount, occupied))
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::CatalogCursorCapacityExceeded,
            "The catalog cursor book-id capacity is exhausted.");
    }
    std::size_t required = 0;
    if (!TryAddSize(occupied, idCount, required))
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::CatalogCursorCapacityExceeded,
            "The catalog cursor book-id capacity is exhausted.");
    }
    while (!state.Sessions.empty()
        && (state.Sessions.size() >= GMaxSearchSessions
            || required > state.AggregateIdCapacity))
    {
        EvictLeastRecentlyUsedLocked(state);
        if (!TryAddSize(state.UsedIdCount, state.ReservedIdCount, occupied)
            || !TryAddSize(occupied, idCount, required))
        {
            throw InpxWebReader::Domain::CDomainException(
                InpxWebReader::Domain::EDomainErrorCode::CatalogCursorCapacityExceeded,
                "The catalog cursor book-id capacity is exhausted.");
        }
    }
    if (required > state.AggregateIdCapacity)
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::CatalogCursorCapacityExceeded,
            "The catalog cursor book-id capacity is exhausted.");
    }
    std::size_t updatedReservedIdCount = 0;
    if (!TryAddSize(state.ReservedIdCount, idCount, updatedReservedIdCount))
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::CatalogCursorCapacityExceeded,
            "The catalog cursor book-id capacity is exhausted.");
    }

    SSearchSessionReservation reservation{
        .Id = NextReservationIdLocked(state),
        .GenerationKey = generationKey,
        .IdCount = idCount
    };
    const bool inserted = state.Reservations.emplace(
        reservation.Id,
        CSqliteBookQueryRepository::SSearchSessionState::SReservation{
            .GenerationKey = reservation.GenerationKey,
            .IdCount = reservation.IdCount
        }).second;
    if (!inserted)
    {
        throw std::runtime_error("Catalog cursor reservation id collision.");
    }
    state.ReservedIdCount = updatedReservedIdCount;
    return reservation;
}

void ReleaseSearchSessionReservation(
    CSqliteBookQueryRepository::SSearchSessionState& state,
    const SSearchSessionReservation& reservation) noexcept
{
    std::scoped_lock lock(state.Mutex);
    static_cast<void>(RemoveReservationLocked(state, reservation));
}

[[nodiscard]] InpxWebReader::Domain::SSearchCursor PublishSearchSession(
    CSqliteBookQueryRepository::SSearchSessionState& state,
    const std::filesystem::path& databasePath,
    std::vector<std::int64_t> ids,
    CSqliteBookQueryRepository::SSearchSessionState::SCriteria criteria,
    const SCatalogIdentity& identity,
    const std::size_t nextPosition,
    const SSearchSessionReservation& reservation)
{
    const std::string generationKey = BuildCatalogGenerationKey(identity);
    auto session = std::make_shared<CSqliteBookQueryRepository::SSearchSessionState::SSession>();
    session->BookIds = std::move(ids);
    session->Criteria = std::move(criteria);
    session->SourceFingerprintUtf8 = identity.SourceFingerprintUtf8;
    session->CatalogSnapshotIdUtf8 = identity.SnapshotIdUtf8;
    session->FirstContinuationPosition = nextPosition;
    session->LastAccess = std::chrono::steady_clock::now();
    const std::size_t sessionIdCount = session->BookIds.size();

    const std::string sessionId = MakeUnpredictableSessionId();
    InpxWebReader::Domain::SSearchCursor cursor{
        .SessionIdUtf8 = sessionId,
        .SourceFingerprintUtf8 = identity.SourceFingerprintUtf8,
        .CatalogSnapshotIdUtf8 = identity.SnapshotIdUtf8,
        .Position = nextPosition
    };

    std::scoped_lock lock(state.Mutex);
    ValidateCurrentCatalogIdentity(databasePath, identity);
    PurgeSessionsLocked(state, session->LastAccess);
    ActivateSearchSessionGenerationLocked(state, generationKey);
    if (sessionIdCount > state.PerSessionIdCapacity)
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::CatalogCursorCapacityExceeded,
            "The catalog result contains too many book ids for the configured cursor capacity.");
    }

    const auto reservationIterator = state.Reservations.find(reservation.Id);
    if (reservationIterator == state.Reservations.end()
        || reservation.GenerationKey != generationKey)
    {
        ThrowCatalogSnapshotChanged();
    }
    if (reservationIterator->second.GenerationKey != reservation.GenerationKey
        || reservationIterator->second.IdCount != reservation.IdCount
        || reservation.IdCount > state.ReservedIdCount)
    {
        throw std::logic_error("Catalog cursor reservation accounting is invalid.");
    }
    if (sessionIdCount != reservation.IdCount)
    {
        throw std::logic_error("Catalog cursor reservation does not match the materialized book-id count.");
    }
    const std::size_t otherReservations = state.ReservedIdCount - reservation.IdCount;
    std::size_t occupiedIdCount = 0;
    std::size_t requiredIdCount = 0;
    if (!TryAddSize(state.UsedIdCount, otherReservations, occupiedIdCount)
        || !TryAddSize(occupiedIdCount, sessionIdCount, requiredIdCount))
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::CatalogCursorCapacityExceeded,
            "The catalog cursor book-id capacity is exhausted.");
    }
    while (!state.Sessions.empty()
        && (state.Sessions.size() >= GMaxSearchSessions
            || requiredIdCount > state.AggregateIdCapacity))
    {
        EvictLeastRecentlyUsedLocked(state);
        if (!TryAddSize(state.UsedIdCount, otherReservations, occupiedIdCount)
            || !TryAddSize(occupiedIdCount, sessionIdCount, requiredIdCount))
        {
            throw InpxWebReader::Domain::CDomainException(
                InpxWebReader::Domain::EDomainErrorCode::CatalogCursorCapacityExceeded,
                "The catalog cursor book-id capacity is exhausted.");
        }
    }
    if (requiredIdCount > state.AggregateIdCapacity)
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::CatalogCursorCapacityExceeded,
            "The catalog cursor book-id capacity is exhausted.");
    }

    if (state.Sessions.contains(sessionId))
    {
        throw std::runtime_error("Catalog cursor session id collision.");
    }

    // Build every potentially allocating return value before publishing. Once
    // emplace succeeds, the remaining accounting mutations cannot throw.
    state.Sessions.emplace(sessionId, session);
    if (!RemoveReservationLocked(state, reservation))
    {
        std::terminate();
    }
    state.UsedIdCount += sessionIdCount;
    return cursor;
}

void RemoveSearchSession(
    CSqliteBookQueryRepository::SSearchSessionState& state,
    const std::string& sessionId)
{
    std::scoped_lock lock(state.Mutex);
    const auto iterator = state.Sessions.find(sessionId);
    if (iterator != state.Sessions.end())
    {
        RemoveSessionLocked(state, iterator);
    }
}

struct SContinuationSlice
{
    std::vector<std::int64_t> Ids;
    std::optional<InpxWebReader::Domain::SSearchCursor> NextCursor;
};

[[nodiscard]] SContinuationSlice ReadSearchSessionSlice(
    CSqliteBookQueryRepository::SSearchSessionState& state,
    const InpxWebReader::Domain::SSearchQuery& query,
    const CSqliteBookQueryRepository::SSearchSessionState::SCriteria& criteria,
    const SCatalogIdentity& identity)
{
    if (!query.Cursor.has_value())
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::CatalogCursorInvalid,
            "The catalog cursor is missing.");
    }
    const auto& cursor = query.Cursor.value();
    if (!cursor.IsValid())
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::CatalogCursorInvalid,
            "The catalog cursor is invalid.");
    }
    if (cursor.SourceFingerprintUtf8 != identity.SourceFingerprintUtf8
        || cursor.CatalogSnapshotIdUtf8 != identity.SnapshotIdUtf8)
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::CatalogSnapshotChanged,
            "The catalog changed while loading the next page.");
    }

    SContinuationSlice result;
    {
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock(state.Mutex);
        PurgeSessionsLocked(state, now);
        const auto iterator = state.Sessions.find(cursor.SessionIdUtf8);
        if (iterator == state.Sessions.end())
        {
            throw InpxWebReader::Domain::CDomainException(
                InpxWebReader::Domain::EDomainErrorCode::CatalogCursorExpired,
                "The catalog cursor expired; restart pagination from the first page.");
        }
        auto& session = *iterator->second;
        if (session.SourceFingerprintUtf8 != identity.SourceFingerprintUtf8
            || session.CatalogSnapshotIdUtf8 != identity.SnapshotIdUtf8)
        {
            RemoveSessionLocked(state, iterator);
            throw InpxWebReader::Domain::CDomainException(
                InpxWebReader::Domain::EDomainErrorCode::CatalogSnapshotChanged,
                "The catalog changed while loading the next page.");
        }
        if (session.Criteria != criteria
            || cursor.Position < session.FirstContinuationPosition
            || cursor.Position >= session.BookIds.size())
        {
            throw InpxWebReader::Domain::CDomainException(
                InpxWebReader::Domain::EDomainErrorCode::CatalogCursorInvalid,
                "The catalog cursor does not match this query.");
        }

        const std::size_t remaining = session.BookIds.size() - cursor.Position;
        const std::size_t count = (std::min)(query.Limit, remaining);
        const std::size_t end = cursor.Position + count;
        result.Ids.assign(
            session.BookIds.begin() + static_cast<std::ptrdiff_t>(cursor.Position),
            session.BookIds.begin() + static_cast<std::ptrdiff_t>(end));
        if (end < session.BookIds.size())
        {
            result.NextCursor = InpxWebReader::Domain::SSearchCursor{
                .SessionIdUtf8 = cursor.SessionIdUtf8,
                .SourceFingerprintUtf8 = cursor.SourceFingerprintUtf8,
                .CatalogSnapshotIdUtf8 = cursor.CatalogSnapshotIdUtf8,
                .Position = end
            };
        }
        session.LastAccess = now;
    }
    return result;
}

[[nodiscard]] std::size_t ResolveSearchSessionIdCapacity(const std::uint64_t budgetBytes)
{
    if (budgetBytes == 0 || budgetBytes > (std::numeric_limits<std::size_t>::max)())
    {
        throw std::invalid_argument("Catalog cursor memory budget must fit size_t and be positive.");
    }
    return static_cast<std::size_t>(budgetBytes / GSearchSessionBookIdAllowanceBytes);
}

} // namespace

CSqliteBookQueryRepository::CSqliteBookQueryRepository(
    std::filesystem::path databasePath,
    const std::uint64_t searchSessionMemoryBudgetBytes,
    SHooks hooks)
    : m_databasePath(std::move(databasePath))
    , m_searchSessions(std::make_shared<SSearchSessionState>(
          ResolveSearchSessionIdCapacity(searchSessionMemoryBudgetBytes)))
    , m_hooks(std::move(hooks))
{
}

std::vector<InpxWebReader::Domain::SBook> CSqliteBookQueryRepository::Search(
    const InpxWebReader::Domain::SSearchQuery& query) const
{
    InpxWebReader::Sqlite::CSqliteConnection connection(m_databasePath);
    InpxWebReader::Sqlite::CSqliteTransaction transaction(
        connection,
        InpxWebReader::Sqlite::ESqliteTransactionMode::Deferred);
    auto result = SearchBooks(connection, query, BuildFtsQuery(connection, query));
    transaction.Commit();
    return result;
}

std::optional<InpxWebReader::Domain::SBook> CSqliteBookQueryRepository::GetById(
    const InpxWebReader::Domain::SBookId id) const
{
    InpxWebReader::Sqlite::CSqliteConnection connection(m_databasePath);
    InpxWebReader::Sqlite::CSqliteTransaction transaction(
        connection,
        InpxWebReader::Sqlite::ESqliteTransactionMode::Deferred);
    auto books = ReadBooks(connection, {id.Value});
    transaction.Commit();
    if (books.empty())
    {
        return std::nullopt;
    }
    return std::move(books.front());
}

std::uint64_t CSqliteBookQueryRepository::CountSearchResults(
    const InpxWebReader::Domain::SSearchQuery& query) const
{
    InpxWebReader::Sqlite::CSqliteConnection connection(m_databasePath);
    return BookDatabase::CountSearchResults(connection, query, BuildFtsQuery(connection, query));
}

std::vector<InpxWebReader::Domain::SFacetItem> CSqliteBookQueryRepository::ListAvailableLanguages(
    const InpxWebReader::Domain::SSearchQuery& query) const
{
    InpxWebReader::Sqlite::CSqliteConnection connection(m_databasePath);
    return BookDatabase::ListAvailableLanguages(connection, query, BuildFtsQuery(connection, query));
}

std::vector<InpxWebReader::Domain::SFacetItem> CSqliteBookQueryRepository::ListAvailableGenres(
    const InpxWebReader::Domain::SSearchQuery& query) const
{
    InpxWebReader::Sqlite::CSqliteConnection connection(m_databasePath);
    return BookDatabase::ListAvailableGenres(connection, query, BuildFtsQuery(connection, query));
}

InpxWebReader::Domain::IBookQueryRepository::SCatalogStatistics
CSqliteBookQueryRepository::GetCatalogStatistics() const
{
    InpxWebReader::Sqlite::CSqliteConnection connection(m_databasePath);
    return CCatalogStatisticsMaintenance::Read(connection, m_databasePath);
}

InpxWebReader::Domain::IBookQueryRepository::SSearchPage CSqliteBookQueryRepository::SearchPage(
    const InpxWebReader::Domain::SSearchQuery& query,
    const bool includeLanguageFacets,
    const bool includeGenreFacets) const
{
    InpxWebReader::Sqlite::CSqliteConnection connection(m_databasePath);
    InpxWebReader::Sqlite::CSqliteTransaction transaction(
        connection,
        InpxWebReader::Sqlite::ESqliteTransactionMode::Deferred);
    const auto identity = ReadCatalogIdentity(connection);
    const std::string ftsQuery = BuildFtsQuery(connection, query);
    const auto criteria = BuildSessionCriteria(query, ftsQuery);

    if (query.Cursor.has_value())
    {
        const auto slice = ReadSearchSessionSlice(*m_searchSessions, query, criteria, identity);
        auto books = ReadBooks(connection, slice.Ids);
        if (books.size() != slice.Ids.size())
        {
            throw InpxWebReader::Domain::CDomainException(
                InpxWebReader::Domain::EDomainErrorCode::IntegrityIssue,
                "A catalog cursor referenced missing book rows.");
        }
        auto result = InpxWebReader::Domain::IBookQueryRepository::SSearchPage{
            .Books = std::move(books),
            .TotalCount = std::nullopt,
            .SourceFingerprintUtf8 = identity.SourceFingerprintUtf8,
            .CatalogSnapshotIdUtf8 = identity.SnapshotIdUtf8,
            .NextCursor = slice.NextCursor
        };
        transaction.Commit();
        return result;
    }

    auto languageQuery = query;
    languageQuery.Languages.clear();
    auto genreQuery = query;
    genreQuery.GenresUtf8.clear();
    const std::uint64_t totalCount = BookDatabase::CountSearchResults(connection, query, ftsQuery);
    if (totalCount > (std::numeric_limits<std::size_t>::max)())
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::CatalogCursorCapacityExceeded,
            "The catalog result cannot be represented by this server.");
    }
    const std::size_t totalSize = static_cast<std::size_t>(totalCount);
    const std::size_t pageStart = (std::min)(query.Offset, totalSize);
    const std::size_t pageLength = (std::min)(query.Limit, totalSize - pageStart);
    const std::size_t pageEnd = pageStart + pageLength;

    auto result = InpxWebReader::Domain::IBookQueryRepository::SSearchPage{
        .TotalCount = totalCount,
        .AvailableLanguages = includeLanguageFacets
            ? BookDatabase::ListAvailableLanguages(connection, languageQuery, ftsQuery)
            : std::vector<InpxWebReader::Domain::SFacetItem>{},
        .AvailableGenres = includeGenreFacets
            ? BookDatabase::ListAvailableGenres(connection, genreQuery, ftsQuery)
            : std::vector<InpxWebReader::Domain::SFacetItem>{},
        .SourceFingerprintUtf8 = identity.SourceFingerprintUtf8,
        .CatalogSnapshotIdUtf8 = identity.SnapshotIdUtf8
    };

    if (pageEnd >= totalSize)
    {
        result.Books = SearchBooks(connection, query, ftsQuery);
        transaction.Commit();
        return result;
    }

    const auto reservation = ReserveSearchSessionIds(
        *m_searchSessions,
        m_databasePath,
        identity,
        totalSize);
    bool reservationActive = true;
    try
    {
        auto ids = SearchBookIds(connection, query, ftsQuery, totalSize);
        std::vector<std::int64_t> pageIds(
            ids.begin() + static_cast<std::ptrdiff_t>(pageStart),
            ids.begin() + static_cast<std::ptrdiff_t>(pageEnd));
        result.Books = ReadBooks(connection, pageIds);
        if (result.Books.size() != pageIds.size())
        {
            throw InpxWebReader::Domain::CDomainException(
                InpxWebReader::Domain::EDomainErrorCode::IntegrityIssue,
                "A materialized catalog cursor referenced missing book rows.");
        }
        transaction.Commit();
        if (m_hooks.BeforeSearchSessionPublication)
        {
            m_hooks.BeforeSearchSessionPublication();
        }

        auto cursor = PublishSearchSession(
            *m_searchSessions,
            m_databasePath,
            std::move(ids),
            criteria,
            identity,
            pageEnd,
            reservation);
        reservationActive = false;
        try
        {
            ValidateCurrentCatalogIdentity(m_databasePath, identity);
        }
        catch (...)
        {
            RemoveSearchSession(*m_searchSessions, cursor.SessionIdUtf8);
            throw;
        }
        result.NextCursor = std::move(cursor);
    }
    catch (...)
    {
        if (reservationActive)
        {
            ReleaseSearchSessionReservation(*m_searchSessions, reservation);
        }
        throw;
    }
    return result;
}

} // namespace InpxWebReader::BookDatabase
