#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include "Database/CatalogStatisticsMaintenance.hpp"
#include "Database/SchemaInitializer.hpp"
#include "Database/SearchIndexMaintenance.hpp"
#include "Database/SqliteBookQueryRepository.hpp"
#include "Database/SqliteConnection.hpp"
#include "Database/SqliteGenreHelpers.hpp"
#include "Database/SqliteStatement.hpp"
#include "Domain/MetadataNormalization.hpp"
#include "Domain/DomainError.hpp"
#include "TestWorkspace.hpp"

namespace {

void InsertBook(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::int64_t id,
    const InpxWebReader::Domain::SBookMetadata& metadata,
    const std::string& libId,
    const std::string& availability)
{
    InpxWebReader::Sqlite::CSqliteStatement book(
        connection.GetNativeHandle(),
        "INSERT INTO inpx_books(id, title, normalized_title, language, description, cover_path, added_at_utc) "
        "VALUES(?, ?, ?, ?, ?, NULL, '2026-01-02T03:04:05Z');");
    book.BindInt64(1, id);
    book.BindText(2, metadata.TitleUtf8);
    book.BindText(3, InpxWebReader::Domain::NormalizeText(metadata.TitleUtf8));
    book.BindText(4, metadata.Language);
    metadata.DescriptionUtf8.has_value() ? book.BindText(5, *metadata.DescriptionUtf8) : book.BindNull(5);
    static_cast<void>(book.Step());

    if (!metadata.AuthorsUtf8.empty())
    {
        InpxWebReader::Sqlite::CSqliteStatement author(
            connection.GetNativeHandle(),
            "INSERT OR IGNORE INTO authors(normalized_name, display_name) VALUES(?, ?);");
        author.BindText(1, InpxWebReader::Domain::NormalizeText(metadata.AuthorsUtf8.front()));
        author.BindText(2, metadata.AuthorsUtf8.front());
        static_cast<void>(author.Step());
        InpxWebReader::Sqlite::CSqliteStatement authorLink(
            connection.GetNativeHandle(),
            "INSERT INTO book_authors(book_id, author_id, author_order) "
            "SELECT ?, id, 0 FROM authors WHERE normalized_name = ?;");
        authorLink.BindInt64(1, id);
        authorLink.BindText(2, InpxWebReader::Domain::NormalizeText(metadata.AuthorsUtf8.front()));
        static_cast<void>(authorLink.Step());
    }

    InpxWebReader::BookDatabase::CSqliteGenreHelpers::InsertGenres(connection, id, metadata.GenresUtf8);
    InpxWebReader::SearchIndex::CSearchIndexMaintenance::InsertBook(connection, id, metadata);

    InpxWebReader::Sqlite::CSqliteStatement location(
        connection.GetNativeHandle(),
        "INSERT INTO inpx_book_locations(book_id, source_id, segment_id, lib_id, archive_name, entry_name, "
        "availability, file_size_bytes, format) VALUES(?, 1, 1, ?, 'books.zip', ?, ?, 1024, 'fb2');");
    location.BindInt64(1, id);
    location.BindText(2, libId);
    location.BindText(3, libId + ".fb2");
    location.BindText(4, availability);
    static_cast<void>(location.Step());
}

[[nodiscard]] std::filesystem::path CreateCatalog(CTestWorkspace& workspace)
{
    const auto databasePath = workspace.GetPath() / "inpx-web-reader.db";
    InpxWebReader::DatabaseRuntime::CSchemaInitializer::Initialize(databasePath);
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    connection.Execute(
        "INSERT INTO inpx_sources(id, display_name, source_fingerprint, last_seen_snapshot_id) "
        "VALUES(1, 'catalog.inpx', 'sha256:fixture', 'scan-fixture');");
    connection.Execute(
        "INSERT INTO inpx_segments(id, source_id, inp_entry_name, archive_name, inp_fingerprint, "
        "record_count, active_record_count, deleted_record_count, availability) "
        "VALUES(1, 1, 'books.zip.inp', 'books.zip', 'sha256:fixture', 2, 2, 0, 'available');");
    InsertBook(
        connection,
        1,
        {
            .TitleUtf8 = "Пикник на обочине",
            .AuthorsUtf8 = {"Аркадий Стругацкий"},
            .Language = "ru",
            .GenresUtf8 = {"Научная фантастика"},
            .DescriptionUtf8 = "Зона и сталкеры"
        },
        "ru-1",
        "available");
    InsertBook(
        connection,
        2,
        {
            .TitleUtf8 = "Roadside Picnic",
            .AuthorsUtf8 = {"Arkady Strugatsky"},
            .Language = "en",
            .GenresUtf8 = {"Science fiction"}
        },
        "en-1",
        "missing_from_index");
    return databasePath;
}

[[nodiscard]] std::int64_t CountSearchMatches(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::string_view query)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT COUNT(*) FROM search_index WHERE search_index MATCH ?;");
    statement.BindText(1, query);
    return statement.Step() ? statement.GetColumnInt64(0) : 0;
}

} // namespace

TEST_CASE("INPX query repository searches Cyrillic metadata", "[book-database][sqlite][inpx][unicode]")
{
    CTestWorkspace workspace("inpx-query-cyrillic");
    InpxWebReader::BookDatabase::CSqliteBookQueryRepository repository(CreateCatalog(workspace));

    InpxWebReader::Domain::SSearchQuery query;
    query.TextUtf8 = "пикн";
    const auto books = repository.Search(query);

    REQUIRE(books.size() == 1);
    REQUIRE(books.front().Metadata.TitleUtf8 == "Пикник на обочине");
    REQUIRE(books.front().Metadata.AuthorsUtf8 == std::vector<std::string>{"Аркадий Стругацкий"});
    REQUIRE(books.front().IsAvailable);
}

TEST_CASE("INPX query repository filters and reports facets", "[book-database][sqlite][inpx]")
{
    CTestWorkspace workspace("inpx-query-facets");
    InpxWebReader::BookDatabase::CSqliteBookQueryRepository repository(CreateCatalog(workspace));

    InpxWebReader::Domain::SSearchQuery query;
    query.Languages = {"en"};
    REQUIRE(repository.CountSearchResults(query) == 1);
    const auto books = repository.Search(query);
    REQUIRE(books.size() == 1);
    REQUIRE_FALSE(books.front().IsAvailable);

    const auto languages = repository.ListAvailableLanguages({});
    REQUIRE(languages == std::vector<InpxWebReader::Domain::SFacetItem>({
        {"en", 1},
        {"ru", 1}
    }));
    const auto genres = repository.ListAvailableGenres({});
    REQUIRE(genres == std::vector<InpxWebReader::Domain::SFacetItem>({
        {"Science fiction", 1},
        {"Научная фантастика", 1}
    }));
}

TEST_CASE("INPX query repository applies field scopes and tokenizes punctuation", "[book-database][sqlite][inpx]")
{
    CTestWorkspace workspace("inpx-query-fields");
    InpxWebReader::BookDatabase::CSqliteBookQueryRepository repository(CreateCatalog(workspace));

    InpxWebReader::Domain::SSearchQuery titleOnly;
    titleOnly.TextUtf8 = "Strugatsky";
    titleOnly.SearchFields = {
        .Title = true,
        .Authors = false,
        .Description = false
    };
    REQUIRE(repository.Search(titleOnly).empty());

    auto authorsOnly = titleOnly;
    authorsOnly.SearchFields = {
        .Title = false,
        .Authors = true,
        .Description = false
    };
    const auto authorMatches = repository.Search(authorsOnly);
    REQUIRE(authorMatches.size() == 1);
    REQUIRE(authorMatches.front().Metadata.TitleUtf8 == "Roadside Picnic");

    InpxWebReader::Domain::SSearchQuery punctuation;
    punctuation.TextUtf8 = "Roadside, Picnic";
    const auto punctuationMatches = repository.Search(punctuation);
    REQUIRE(punctuationMatches.size() == 1);
    REQUIRE(punctuationMatches.front().Metadata.TitleUtf8 == "Roadside Picnic");

    punctuation.TextUtf8 = "Picnic\xE2\x80\x94Road";
    const auto unicodePunctuationMatches = repository.Search(punctuation);
    REQUIRE(unicodePunctuationMatches.size() == 1);
    REQUIRE(unicodePunctuationMatches.front().Metadata.TitleUtf8 == "Roadside Picnic");

    punctuation.TextUtf8 = "!!! \xE2\x80\x94 ???";
    REQUIRE(repository.Search(punctuation).empty());
    REQUIRE(repository.CountSearchResults(punctuation) == 0);
    REQUIRE(repository.ListAvailableLanguages(punctuation).empty());
    REQUIRE(repository.ListAvailableGenres(punctuation).empty());
}

TEST_CASE("INPX query repository keeps sorting and pagination stable", "[book-database][sqlite][inpx]")
{
    CTestWorkspace workspace("inpx-query-pagination");
    InpxWebReader::BookDatabase::CSqliteBookQueryRepository repository(CreateCatalog(workspace));

    InpxWebReader::Domain::SSearchQuery ascending;
    ascending.SortBy = InpxWebReader::Domain::EBookSort::Title;
    ascending.SortDirection = InpxWebReader::Domain::ESortDirection::Ascending;
    ascending.Limit = 2;
    const auto ascendingBooks = repository.Search(ascending);
    REQUIRE(ascendingBooks.size() == 2);

    auto descending = ascending;
    descending.SortDirection = InpxWebReader::Domain::ESortDirection::Descending;
    const auto descendingBooks = repository.Search(descending);
    REQUIRE(descendingBooks.size() == 2);
    REQUIRE(descendingBooks[0].Id.Value == ascendingBooks[1].Id.Value);
    REQUIRE(descendingBooks[1].Id.Value == ascendingBooks[0].Id.Value);

    auto secondPage = ascending;
    secondPage.Offset = 1;
    secondPage.Limit = 1;
    const auto pagedBooks = repository.Search(secondPage);
    REQUIRE(pagedBooks.size() == 1);
    REQUIRE(pagedBooks.front().Id.Value == ascendingBooks[1].Id.Value);

    auto byAuthor = ascending;
    byAuthor.SortBy = InpxWebReader::Domain::EBookSort::Author;
    const auto authorSortedBooks = repository.Search(byAuthor);
    REQUIRE(authorSortedBooks.size() == 2);
    REQUIRE(authorSortedBooks.front().Metadata.AuthorsUtf8 == std::vector<std::string>{"Arkady Strugatsky"});

    if constexpr (sizeof(std::size_t) >= sizeof(std::uint64_t))
    {
        auto overflowing = ascending;
        overflowing.Offset = static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)()) + 1ULL;
        REQUIRE_THROWS_WITH(repository.Search(overflowing), "Search offset exceeds the SQLite int64 range.");
    }
}

TEST_CASE("INPX query repository materializes cursor order once for every catalog sort", "[book-database][sqlite][inpx][pagination]")
{
    CTestWorkspace workspace("inpx-query-cursor-order");
    const auto databasePath = CreateCatalog(workspace);
    {
        InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
        InsertBook(
            connection,
            3,
            {
                .TitleUtf8 = "Alpha",
                .AuthorsUtf8 = {"Zed Author"},
                .Language = "en",
                .GenresUtf8 = {"Science fiction"}
            },
            "en-3",
            "available");
        InsertBook(
            connection,
            4,
            {
                .TitleUtf8 = "Alpha",
                .Language = "ru",
                .GenresUtf8 = {"Science fiction"}
            },
            "ru-4",
            "available");
        InsertBook(
            connection,
            5,
            {
                .TitleUtf8 = "Beta",
                .AuthorsUtf8 = {"Arkady Strugatsky"},
                .Language = "en",
                .GenresUtf8 = {"Science fiction"}
            },
            "en-5",
            "available");
        connection.Execute("UPDATE inpx_books SET added_at_utc = '2025-01-01T00:00:00Z' WHERE id IN (3, 4);");
        connection.Execute("UPDATE inpx_books SET added_at_utc = '2027-01-01T00:00:00Z' WHERE id = 5;");
    }
    InpxWebReader::BookDatabase::CSqliteBookQueryRepository repository(databasePath);

    for (const auto sort : {
             InpxWebReader::Domain::EBookSort::Title,
             InpxWebReader::Domain::EBookSort::Author,
             InpxWebReader::Domain::EBookSort::DateAdded})
    {
        for (const auto direction : {
                 InpxWebReader::Domain::ESortDirection::Ascending,
                 InpxWebReader::Domain::ESortDirection::Descending})
        {
            InpxWebReader::Domain::SSearchQuery expectedQuery;
            expectedQuery.SortBy = sort;
            expectedQuery.SortDirection = direction;
            expectedQuery.Limit = 100;
            const auto expectedBooks = repository.Search(expectedQuery);

            auto pageQuery = expectedQuery;
            pageQuery.Limit = 1;
            auto page = repository.SearchPage(pageQuery, true, true);
            REQUIRE(page.TotalCount == expectedBooks.size());
            REQUIRE(page.NextCursor.has_value());
            REQUIRE(page.AvailableLanguages.size() == 2);

            std::vector<std::int64_t> actualIds;
            actualIds.push_back(page.Books.front().Id.Value);
            const auto firstCursor = page.NextCursor;
            while (page.NextCursor.has_value())
            {
                pageQuery.Cursor = page.NextCursor;
                pageQuery.Limit = 2;
                page = repository.SearchPage(pageQuery, true, true);
                REQUIRE_FALSE(page.TotalCount.has_value());
                REQUIRE(page.AvailableLanguages.empty());
                REQUIRE(page.AvailableGenres.empty());
                for (const auto& book : page.Books)
                {
                    actualIds.push_back(book.Id.Value);
                }
            }

            std::vector<std::int64_t> expectedIds;
            for (const auto& book : expectedBooks)
            {
                expectedIds.push_back(book.Id.Value);
            }
            REQUIRE(actualIds == expectedIds);

            pageQuery.Cursor = firstCursor;
            pageQuery.Limit = 2;
            const auto replay = repository.SearchPage(pageQuery, false, false);
            REQUIRE(replay.Books.size() == 2);
            REQUIRE(replay.Books[0].Id.Value == expectedIds[1]);
            REQUIRE(replay.Books[1].Id.Value == expectedIds[2]);
        }
    }
}

TEST_CASE("INPX query repository rejects stale mismatched and over-capacity cursors", "[book-database][sqlite][inpx][pagination][limits]")
{
    CTestWorkspace workspace("inpx-query-cursor-errors");
    const auto databasePath = CreateCatalog(workspace);
    InpxWebReader::BookDatabase::CSqliteBookQueryRepository repository(databasePath);

    InpxWebReader::Domain::SSearchQuery query;
    query.Limit = 1;
    const auto firstPage = repository.SearchPage(query, true, true);
    REQUIRE(firstPage.NextCursor.has_value());

    auto mismatched = query;
    mismatched.Cursor = firstPage.NextCursor;
    mismatched.Languages = {"en"};
    try
    {
        static_cast<void>(repository.SearchPage(mismatched, false, false));
        FAIL("Expected a mismatched cursor to be rejected.");
    }
    catch (const InpxWebReader::Domain::CDomainException& error)
    {
        REQUIRE(error.Code() == InpxWebReader::Domain::EDomainErrorCode::CatalogCursorInvalid);
    }

    {
        InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
        connection.Execute("UPDATE inpx_sources SET last_seen_snapshot_id = 'scan-next' WHERE id = 1;");
    }
    auto stale = query;
    stale.Cursor = firstPage.NextCursor;
    try
    {
        static_cast<void>(repository.SearchPage(stale, false, false));
        FAIL("Expected a stale cursor to be rejected.");
    }
    catch (const InpxWebReader::Domain::CDomainException& error)
    {
        REQUIRE(error.Code() == InpxWebReader::Domain::EDomainErrorCode::CatalogSnapshotChanged);
    }

    InpxWebReader::BookDatabase::CSqliteBookQueryRepository constrainedRepository(databasePath, 16);
    try
    {
        static_cast<void>(constrainedRepository.SearchPage(query, true, true));
        FAIL("Expected an over-capacity cursor to be rejected.");
    }
    catch (const InpxWebReader::Domain::CDomainException& error)
    {
        REQUIRE(error.Code()
            == InpxWebReader::Domain::EDomainErrorCode::CatalogCursorCapacityExceeded);
    }
}

TEST_CASE("INPX query repository publishes concurrent cursor reservations independently", "[book-database][sqlite][inpx][pagination][concurrency]")
{
    CTestWorkspace workspace("inpx-query-cursor-concurrency");
    const auto databasePath = CreateCatalog(workspace);
    // Keep the WAL mapping initialized while both readers start. SQLite
    // coordinates cold WAL recovery through file locks that TSan cannot see.
    InpxWebReader::Sqlite::CSqliteConnection warmConnection(databasePath);
    REQUIRE(warmConnection.GetNativeHandle() != nullptr);
    InpxWebReader::BookDatabase::CSqliteBookQueryRepository repository(databasePath);

    InpxWebReader::Domain::SSearchQuery query;
    query.Limit = 1;
    using SSearchPage = InpxWebReader::Domain::IBookQueryRepository::SSearchPage;
    std::array<SSearchPage, 2> firstPages;
    std::array<std::exception_ptr, 2> errors;
    std::atomic<std::size_t> readyWorkers = 0;
    std::atomic_bool startWorkers = false;

    const auto loadFirstPage = [&](const std::size_t index) {
        readyWorkers.fetch_add(1, std::memory_order_release);
        while (!startWorkers.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        try
        {
            firstPages[index] = repository.SearchPage(query, false, false);
        }
        catch (...)
        {
            errors[index] = std::current_exception();
        }
    };

    std::jthread firstWorker(loadFirstPage, 0);
    std::jthread secondWorker(loadFirstPage, 1);
    while (readyWorkers.load(std::memory_order_acquire) != 2)
    {
        std::this_thread::yield();
    }
    startWorkers.store(true, std::memory_order_release);
    firstWorker.join();
    secondWorker.join();

    for (const auto& error : errors)
    {
        if (error)
        {
            std::rethrow_exception(error);
        }
    }
    REQUIRE(firstPages[0].NextCursor.has_value());
    REQUIRE(firstPages[1].NextCursor.has_value());
    REQUIRE(firstPages[0].NextCursor->SessionIdUtf8 != firstPages[1].NextCursor->SessionIdUtf8);

    for (const auto& firstPage : firstPages)
    {
        auto continuationQuery = query;
        continuationQuery.Cursor = firstPage.NextCursor;
        const auto continuation = repository.SearchPage(continuationQuery, false, false);
        REQUIRE(continuation.Books.size() == 1);
        REQUIRE_FALSE(continuation.TotalCount.has_value());
        REQUIRE_FALSE(continuation.NextCursor.has_value());
    }
}

TEST_CASE("INPX query repository rejects obsolete publication without evicting the current generation", "[book-database][sqlite][inpx][pagination][concurrency]")
{
    CTestWorkspace workspace("inpx-query-cursor-generation-race");
    const auto databasePath = CreateCatalog(workspace);
    std::mutex checkpointMutex;
    std::condition_variable checkpointCondition;
    bool obsoleteRequestPaused = false;
    bool releaseObsoleteRequest = false;
    std::atomic<std::size_t> publicationCheckpointCount = 0;

    InpxWebReader::BookDatabase::CSqliteBookQueryRepository repository(
        databasePath,
        64ull * 1024ull * 1024ull,
        {.BeforeSearchSessionPublication = [&]() {
            if (publicationCheckpointCount.fetch_add(1, std::memory_order_relaxed) != 0)
            {
                return;
            }
            std::unique_lock lock(checkpointMutex);
            obsoleteRequestPaused = true;
            checkpointCondition.notify_all();
            checkpointCondition.wait(lock, [&]() { return releaseObsoleteRequest; });
        }});

    InpxWebReader::Domain::SSearchQuery query;
    query.Limit = 1;
    std::exception_ptr obsoleteRequestError;
    std::jthread obsoleteWorker([&]() {
        try
        {
            static_cast<void>(repository.SearchPage(query, false, false));
        }
        catch (...)
        {
            obsoleteRequestError = std::current_exception();
        }
    });

    const auto releaseWorker = [&]() {
        {
            std::scoped_lock lock(checkpointMutex);
            releaseObsoleteRequest = true;
        }
        checkpointCondition.notify_all();
    };

    bool reachedPublicationCheckpoint = false;
    {
        std::unique_lock lock(checkpointMutex);
        reachedPublicationCheckpoint = checkpointCondition.wait_for(
            lock,
            std::chrono::seconds{5},
            [&]() { return obsoleteRequestPaused; });
    }
    if (!reachedPublicationCheckpoint)
    {
        releaseWorker();
        obsoleteWorker.join();
        if (obsoleteRequestError)
        {
            std::rethrow_exception(obsoleteRequestError);
        }
        FAIL("The obsolete request did not reach the publication checkpoint.");
    }

    constexpr std::size_t searchSessionCapacity = 8;
    std::vector<InpxWebReader::Domain::SSearchCursor> currentGenerationCursors;
    currentGenerationCursors.reserve(searchSessionCapacity);
    try
    {
        {
            InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
            connection.Execute(
                "UPDATE inpx_sources SET last_seen_snapshot_id = 'scan-next' WHERE id = 1;");
        }
        for (std::size_t index = 0; index < searchSessionCapacity; ++index)
        {
            const auto page = repository.SearchPage(query, false, false);
            if (!page.NextCursor.has_value())
            {
                throw std::runtime_error("Expected the current generation page to have a cursor.");
            }
            currentGenerationCursors.push_back(*page.NextCursor);
        }
    }
    catch (...)
    {
        releaseWorker();
        obsoleteWorker.join();
        throw;
    }

    releaseWorker();
    obsoleteWorker.join();

    REQUIRE(obsoleteRequestError);
    try
    {
        std::rethrow_exception(obsoleteRequestError);
    }
    catch (const InpxWebReader::Domain::CDomainException& error)
    {
        REQUIRE(error.Code() == InpxWebReader::Domain::EDomainErrorCode::CatalogSnapshotChanged);
    }

    REQUIRE(currentGenerationCursors.size() == searchSessionCapacity);
    for (const auto& cursor : currentGenerationCursors)
    {
        auto continuationQuery = query;
        continuationQuery.Cursor = cursor;
        const auto continuation = repository.SearchPage(continuationQuery, false, false);
        REQUIRE(continuation.Books.size() == 1);
        REQUIRE_FALSE(continuation.NextCursor.has_value());
    }
}

TEST_CASE("INPX query repository returns a complete page from one catalog snapshot", "[book-database][sqlite][inpx]")
{
    CTestWorkspace workspace("inpx-query-page-snapshot");
    const auto databasePath = CreateCatalog(workspace);
    {
        InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
        connection.Execute("UPDATE inpx_sources SET last_seen_snapshot_id = 'scan-17' WHERE id = 1;");
    }
    InpxWebReader::BookDatabase::CSqliteBookQueryRepository repository(databasePath);

    InpxWebReader::Domain::SSearchQuery query;
    query.Languages = {"en"};
    const auto page = repository.SearchPage(query, true, true);

    REQUIRE(page.Books.size() == 1);
    REQUIRE(page.Books.front().Metadata.TitleUtf8 == "Roadside Picnic");
    REQUIRE(page.TotalCount == 1);
    REQUIRE(page.AvailableLanguages == std::vector<InpxWebReader::Domain::SFacetItem>({
        {"en", 1},
        {"ru", 1}
    }));
    REQUIRE(page.AvailableGenres == std::vector<InpxWebReader::Domain::SFacetItem>({{"Science fiction", 1}}));
    REQUIRE(page.SourceFingerprintUtf8 == "sha256:fixture");
    REQUIRE(page.CatalogSnapshotIdUtf8 == "scan-17");
}

TEST_CASE("Catalog statistics reads remain pure under SQLite query-only mode", "[book-database][sqlite][inpx]")
{
    CTestWorkspace workspace("inpx-statistics-query-only");
    const auto databasePath = CreateCatalog(workspace);
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    connection.Execute("PRAGMA query_only = ON;");

    const auto statistics = InpxWebReader::BookDatabase::CCatalogStatisticsMaintenance::Read(
        connection,
        databasePath);
    REQUIRE(statistics.BookCount == 0);

    connection.Execute("PRAGMA query_only = OFF;");
    connection.Execute("DELETE FROM catalog_statistics WHERE singleton = 1;");
    connection.Execute("PRAGMA query_only = ON;");
    REQUIRE_THROWS_WITH(
        InpxWebReader::BookDatabase::CCatalogStatisticsMaintenance::Read(connection, databasePath),
        "Catalog statistics singleton is missing.");
}

TEST_CASE("INPX query repository combines filters and excludes the active facet dimension", "[book-database][sqlite][inpx]")
{
    CTestWorkspace workspace("inpx-query-combined-facets");
    InpxWebReader::BookDatabase::CSqliteBookQueryRepository repository(CreateCatalog(workspace));

    InpxWebReader::Domain::SSearchQuery combined;
    combined.Languages = {"en"};
    combined.GenresUtf8 = {"Science fiction"};
    REQUIRE(repository.CountSearchResults(combined) == 1);
    REQUIRE(repository.Search(combined).front().Metadata.TitleUtf8 == "Roadside Picnic");

    REQUIRE(repository.ListAvailableLanguages(combined)
        == std::vector<InpxWebReader::Domain::SFacetItem>({{"en", 1}}));
    REQUIRE(repository.ListAvailableGenres(combined)
        == std::vector<InpxWebReader::Domain::SFacetItem>({{"Science fiction", 1}}));

    combined.GenresUtf8.clear();
    REQUIRE(repository.ListAvailableLanguages(combined)
        == std::vector<InpxWebReader::Domain::SFacetItem>({
            {"en", 1},
            {"ru", 1}
        }));
    REQUIRE(repository.ListAvailableGenres(combined)
        == std::vector<InpxWebReader::Domain::SFacetItem>({{"Science fiction", 1}}));
}

TEST_CASE("INPX query repository loads details by id", "[book-database][sqlite][inpx]")
{
    CTestWorkspace workspace("inpx-query-details");
    InpxWebReader::BookDatabase::CSqliteBookQueryRepository repository(CreateCatalog(workspace));

    const auto book = repository.GetById({2});
    REQUIRE(book.has_value());
    REQUIRE(book->File.Format == InpxWebReader::Domain::EBookFormat::Fb2);
    REQUIRE(book->File.SizeBytes == 1024);
    REQUIRE_FALSE(repository.GetById({999}).has_value());
}

TEST_CASE("INPX query repository excludes metadata detached from an INPX locator", "[book-database][sqlite][inpx]")
{
    CTestWorkspace workspace("inpx-query-detached-metadata");
    const auto databasePath = CreateCatalog(workspace);
    {
        InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
        const InpxWebReader::Domain::SBookMetadata detachedMetadata{
            .TitleUtf8 = "Detached metadata",
            .AuthorsUtf8 = {"Orphan Author"},
            .Language = "zz"
        };
        InpxWebReader::Sqlite::CSqliteStatement book(
            connection.GetNativeHandle(),
            "INSERT INTO inpx_books(id, title, normalized_title, language, added_at_utc) "
            "VALUES(3, ?, ?, ?, '2026-01-02T03:04:05Z');");
        book.BindText(1, detachedMetadata.TitleUtf8);
        book.BindText(2, InpxWebReader::Domain::NormalizeText(detachedMetadata.TitleUtf8));
        book.BindText(3, detachedMetadata.Language);
        static_cast<void>(book.Step());
        InpxWebReader::SearchIndex::CSearchIndexMaintenance::InsertBook(connection, 3, detachedMetadata);
    }

    InpxWebReader::BookDatabase::CSqliteBookQueryRepository repository(databasePath);
    InpxWebReader::Domain::SSearchQuery allBooks;
    REQUIRE(repository.CountSearchResults(allBooks) == 2);
    REQUIRE(repository.Search(allBooks).size() == 2);
    REQUIRE(repository.ListAvailableLanguages(allBooks)
        == std::vector<InpxWebReader::Domain::SFacetItem>({
            {"en", 1},
            {"ru", 1}
        }));

    InpxWebReader::Domain::SSearchQuery detachedSearch;
    detachedSearch.TextUtf8 = "Detached";
    REQUIRE(repository.CountSearchResults(detachedSearch) == 0);
    REQUIRE(repository.Search(detachedSearch).empty());
    REQUIRE_FALSE(repository.GetById({3}).has_value());
}

TEST_CASE("Contentless FTS delete uses canonical relation order", "[book-database][sqlite][fts]")
{
    CTestWorkspace workspace("inpx-query-fts-canonical-order");
    const auto databasePath = CreateCatalog(workspace);
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    const InpxWebReader::Domain::SBookMetadata insertedMetadata{
        .TitleUtf8 = "Ordering fixture",
        .AuthorsUtf8 = {"Author"},
        .Language = "en",
        .GenresUtf8 = {"Zulu Genre", "alpha genre"}
    };
    InsertBook(connection, 3, insertedMetadata, "ordering", "available");
    REQUIRE(CountSearchMatches(connection, "zulu") == 1);

    auto reconstructedMetadata = insertedMetadata;
    reconstructedMetadata.GenresUtf8 = {"alpha genre", "Zulu Genre"};
    InpxWebReader::SearchIndex::CSearchIndexMaintenance::RemoveBook(
        connection,
        3,
        reconstructedMetadata);
    REQUIRE(CountSearchMatches(connection, "zulu") == 0);

}
