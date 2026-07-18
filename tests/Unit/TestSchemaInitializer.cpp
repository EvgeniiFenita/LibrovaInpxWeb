#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <stdexcept>

#include "Database/SchemaInitializer.hpp"
#include "Database/SqliteConnection.hpp"
#include "Database/SqliteStatement.hpp"
#include "TestWorkspace.hpp"

namespace {

[[nodiscard]] bool HasTable(const std::filesystem::path& databasePath, const std::string& name)
{
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = ?;");
    statement.BindText(1, name);
    return statement.Step() && statement.GetColumnInt(0) == 1;
}

[[nodiscard]] std::int64_t CountUserSchemaObjects(const std::filesystem::path& databasePath)
{
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT COUNT(*) FROM sqlite_master WHERE name NOT GLOB 'sqlite_*';");
    return statement.Step() ? statement.GetColumnInt64(0) : 0;
}

void SeedCatalog(const InpxWebReader::Sqlite::CSqliteConnection& connection)
{
    connection.Execute(
        "INSERT INTO inpx_sources(id, display_name, source_fingerprint) "
        "VALUES(1, 'catalog.inpx', 'sha256:fixture');");
}

void InitializeFreshDatabase(const std::filesystem::path& databasePath)
{
    InpxWebReader::DatabaseRuntime::CSchemaInitializer::Initialize(databasePath);
    REQUIRE(InpxWebReader::DatabaseRuntime::CSchemaInitializer::ReadUserVersion(databasePath) == 1);
}

void RewriteSchemaSql(
    const std::filesystem::path& databasePath,
    const std::string_view objectName,
    const std::string_view oldSql,
    const std::string_view newSql,
    const std::string_view objectToDelete = {})
{
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    connection.Execute("PRAGMA writable_schema = ON;");
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "UPDATE sqlite_master SET sql = replace(sql, ?, ?) WHERE name = ?;");
    statement.BindText(1, oldSql);
    statement.BindText(2, newSql);
    statement.BindText(3, objectName);
    static_cast<void>(statement.Step());
    if (!objectToDelete.empty())
    {
        InpxWebReader::Sqlite::CSqliteStatement deleteStatement(
            connection.GetNativeHandle(),
            "DELETE FROM sqlite_master WHERE name = ?;");
        deleteStatement.BindText(1, objectToDelete);
        static_cast<void>(deleteStatement.Step());
    }
    connection.Execute("PRAGMA writable_schema = OFF;");
}

} // namespace

TEST_CASE("Schema initializer creates a fresh INPX cache database", "[database-schema][sqlite][inpx]")
{
    CTestWorkspace workspace("inpx-schema-fresh");
    const auto databasePath = workspace.GetPath() / "inpx-web-reader.db";

    InpxWebReader::DatabaseRuntime::CSchemaInitializer::Initialize(databasePath);

    REQUIRE(InpxWebReader::DatabaseRuntime::CSchemaInitializer::ReadUserVersion(databasePath) == 1);
    REQUIRE(HasTable(databasePath, "inpx_books"));
    REQUIRE(HasTable(databasePath, "inpx_sources"));
    REQUIRE(HasTable(databasePath, "inpx_segments"));
    REQUIRE(HasTable(databasePath, "inpx_deletions"));
}

TEST_CASE("Schema initializer rejects a partial database", "[database-schema][sqlite][inpx]")
{
    CTestWorkspace workspace("inpx-schema-partial");
    const auto databasePath = workspace.GetPath() / "partial.db";
    {
        InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
        connection.Execute("CREATE TABLE unexpected(id INTEGER PRIMARY KEY);");
    }

    REQUIRE_THROWS_WITH(
        InpxWebReader::DatabaseRuntime::CSchemaInitializer::Initialize(databasePath),
        Catch::Matchers::ContainsSubstring("partially initialized"));
}

TEST_CASE("Schema initializer reopens a complete version 1 database", "[database-schema][sqlite][inpx]")
{
    CTestWorkspace workspace("inpx-schema-reopen");
    const auto databasePath = workspace.GetPath() / "complete.db";
    InitializeFreshDatabase(databasePath);

    REQUIRE_NOTHROW(InpxWebReader::DatabaseRuntime::CSchemaInitializer::Initialize(databasePath));
}

TEST_CASE("Catalog initialization publishes schema and required singletons atomically", "[database-schema][sqlite][inpx]")
{
    CTestWorkspace workspace("inpx-catalog-schema-atomic");
    const auto databasePath = workspace.GetPath() / "catalog.db";

    REQUIRE_THROWS_WITH(
        InpxWebReader::DatabaseRuntime::CSchemaInitializer::InitializeCatalog(
            databasePath,
            [](const InpxWebReader::Sqlite::CSqliteConnection&) {
                throw std::runtime_error("injected catalog seed failure");
            }),
        Catch::Matchers::ContainsSubstring("injected catalog seed failure"));
    REQUIRE(InpxWebReader::DatabaseRuntime::CSchemaInitializer::ReadUserVersion(databasePath) == 0);
    REQUIRE(CountUserSchemaObjects(databasePath) == 0);

    REQUIRE_NOTHROW(InpxWebReader::DatabaseRuntime::CSchemaInitializer::InitializeCatalog(
        databasePath,
        SeedCatalog));
    REQUIRE(InpxWebReader::DatabaseRuntime::CSchemaInitializer::ReadUserVersion(databasePath) == 1);

    bool reopenSeedCalled = false;
    REQUIRE_NOTHROW(InpxWebReader::DatabaseRuntime::CSchemaInitializer::InitializeCatalog(
        databasePath,
        [&](const InpxWebReader::Sqlite::CSqliteConnection&) {
            reopenSeedCalled = true;
        }));
    REQUIRE_FALSE(reopenSeedCalled);
}

TEST_CASE("Catalog initialization rejects missing runtime singletons", "[database-schema][sqlite][inpx]")
{
    CTestWorkspace workspace("inpx-catalog-schema-singletons");

    SECTION("source singleton")
    {
        const auto databasePath = workspace.GetPath() / "missing-source.db";
        InitializeFreshDatabase(databasePath);
        REQUIRE_THROWS_WITH(
            InpxWebReader::DatabaseRuntime::CSchemaInitializer::InitializeCatalog(databasePath, SeedCatalog),
            Catch::Matchers::ContainsSubstring("INPX source singleton is missing or invalid"));
    }

    SECTION("statistics singleton")
    {
        const auto databasePath = workspace.GetPath() / "missing-statistics.db";
        InpxWebReader::DatabaseRuntime::CSchemaInitializer::InitializeCatalog(databasePath, SeedCatalog);
        {
            InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
            connection.Execute("DELETE FROM catalog_statistics WHERE singleton = 1;");
        }
        REQUIRE_THROWS_WITH(
            InpxWebReader::DatabaseRuntime::CSchemaInitializer::InitializeCatalog(databasePath, SeedCatalog),
            Catch::Matchers::ContainsSubstring("catalog statistics singleton is missing or invalid"));
    }
}

TEST_CASE("Schema initializer rejects catalog rows detached from the INPX source", "[database-schema][sqlite][inpx]")
{
    CTestWorkspace workspace("inpx-schema-relations");

    SECTION("book without INPX locator")
    {
        const auto databasePath = workspace.GetPath() / "missing-locator.db";
        InitializeFreshDatabase(databasePath);
        {
            InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
            connection.Execute(
                "INSERT INTO inpx_books(id, title, normalized_title, language, added_at_utc) "
                "VALUES(1, 'Detached', 'detached', 'en', '2026-01-02T03:04:05Z');");
        }

        REQUIRE_THROWS_WITH(
            InpxWebReader::DatabaseRuntime::CSchemaInitializer::Initialize(databasePath),
            Catch::Matchers::ContainsSubstring("every INPX book must have an INPX locator"));
    }

    SECTION("persisted foreign key violation")
    {
        const auto databasePath = workspace.GetPath() / "foreign-key-row.db";
        InitializeFreshDatabase(databasePath);
        {
            InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
            connection.Execute("PRAGMA foreign_keys = OFF;");
            connection.Execute(
                "INSERT INTO inpx_scan_warnings(source_id, scan_id, warning_code, message, created_at_utc) "
                "VALUES(99, 'scan', 'fixture', 'Detached warning', '2026-01-02T03:04:05Z');");
        }

        REQUIRE_THROWS_WITH(
            InpxWebReader::DatabaseRuntime::CSchemaInitializer::Initialize(databasePath),
            Catch::Matchers::ContainsSubstring("persisted rows violate a foreign key"));
    }
}

TEST_CASE("Schema initializer rejects an unsupported schema version", "[database-schema][sqlite][inpx]")
{
    CTestWorkspace workspace("inpx-schema-version");
    const auto databasePath = workspace.GetPath() / "unsupported.db";
    InitializeFreshDatabase(databasePath);
    {
        InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
        connection.Execute("PRAGMA user_version = 2;");
    }

    REQUIRE_THROWS_WITH(
        InpxWebReader::DatabaseRuntime::CSchemaInitializer::Initialize(databasePath),
        Catch::Matchers::ContainsSubstring("Unsupported database schema version 2"));
}

TEST_CASE("Schema initializer rejects damaged table and index shapes", "[database-schema][sqlite][inpx]")
{
    CTestWorkspace workspace("inpx-schema-shapes");

    SECTION("extra table column")
    {
        const auto databasePath = workspace.GetPath() / "extra-column.db";
        InitializeFreshDatabase(databasePath);
        {
            InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
            connection.Execute("ALTER TABLE inpx_books ADD COLUMN unexpected TEXT;");
        }

        REQUIRE_THROWS_WITH(
            InpxWebReader::DatabaseRuntime::CSchemaInitializer::Initialize(databasePath),
            Catch::Matchers::ContainsSubstring("unexpected extra column"));
    }

    SECTION("missing required index")
    {
        const auto databasePath = workspace.GetPath() / "missing-index.db";
        InitializeFreshDatabase(databasePath);
        {
            InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
            connection.Execute("DROP INDEX idx_inpx_books_title_sort;");
        }

        REQUIRE_THROWS_WITH(
            InpxWebReader::DatabaseRuntime::CSchemaInitializer::Initialize(databasePath),
            Catch::Matchers::ContainsSubstring("missing required index 'idx_inpx_books_title_sort'"));
    }
}

TEST_CASE("Schema initializer rejects damaged foreign keys and constraints", "[database-schema][sqlite][inpx]")
{
    CTestWorkspace workspace("inpx-schema-contracts");

    SECTION("foreign key action")
    {
        const auto databasePath = workspace.GetPath() / "foreign-key.db";
        InitializeFreshDatabase(databasePath);
        RewriteSchemaSql(
            databasePath,
            "inpx_scan_warnings",
            "ON DELETE CASCADE",
            "ON DELETE RESTRICT");

        REQUIRE_THROWS_WITH(
            InpxWebReader::DatabaseRuntime::CSchemaInitializer::Initialize(databasePath),
            Catch::Matchers::ContainsSubstring("ON DELETE CASCADE"));
    }

    SECTION("singleton check")
    {
        const auto databasePath = workspace.GetPath() / "singleton-check.db";
        InitializeFreshDatabase(databasePath);
        RewriteSchemaSql(databasePath, "inpx_sources", " CHECK(id = 1)", "");

        REQUIRE_THROWS_WITH(
            InpxWebReader::DatabaseRuntime::CSchemaInitializer::Initialize(databasePath),
            Catch::Matchers::ContainsSubstring("missing required SQL token 'check(id = 1)'"));
    }

    SECTION("unique source identity")
    {
        const auto databasePath = workspace.GetPath() / "source-identity.db";
        InitializeFreshDatabase(databasePath);
        RewriteSchemaSql(
            databasePath,
            "inpx_book_locations",
            "UNIQUE(source_id, lib_id)",
            "CHECK(source_id > 0)",
            "sqlite_autoindex_inpx_book_locations_1");

        REQUIRE_THROWS_WITH(
            InpxWebReader::DatabaseRuntime::CSchemaInitializer::Initialize(databasePath),
            Catch::Matchers::ContainsSubstring("missing required SQL token 'unique(source_id, lib_id)'"));
    }
}

TEST_CASE("Schema initializer rejects non-canonical current-version objects", "[database-schema][sqlite][inpx]")
{
    CTestWorkspace workspace("inpx-schema-exact-shape");

    SECTION("unexpected object")
    {
        const auto databasePath = workspace.GetPath() / "unexpected-object.db";
        InitializeFreshDatabase(databasePath);
        {
            InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
            connection.Execute("CREATE TABLE extension_state(id INTEGER PRIMARY KEY);");
        }
        REQUIRE_THROWS_WITH(
            InpxWebReader::DatabaseRuntime::CSchemaInitializer::Initialize(databasePath),
            Catch::Matchers::ContainsSubstring("unexpected table 'extension_state'"));
    }

    SECTION("different FTS tokenizer")
    {
        const auto databasePath = workspace.GetPath() / "fts-options.db";
        InitializeFreshDatabase(databasePath);
        RewriteSchemaSql(
            databasePath,
            "search_index",
            "content=''",
            "content='', tokenize='ascii'");
        REQUIRE_THROWS_WITH(
            InpxWebReader::DatabaseRuntime::CSchemaInitializer::Initialize(databasePath),
            Catch::Matchers::ContainsSubstring("search_index' does not match the exact current definition"));
    }

    SECTION("unique replacement index")
    {
        const auto databasePath = workspace.GetPath() / "unique-index.db";
        InitializeFreshDatabase(databasePath);
        {
            InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
            connection.Execute("DROP INDEX idx_inpx_books_cover_path;");
            connection.Execute(
                "CREATE UNIQUE INDEX idx_inpx_books_cover_path ON inpx_books(cover_path);");
        }
        REQUIRE_THROWS_WITH(
            InpxWebReader::DatabaseRuntime::CSchemaInitializer::Initialize(databasePath),
            Catch::Matchers::ContainsSubstring("idx_inpx_books_cover_path' does not match the exact current definition"));
    }

    SECTION("constraint token in a comment")
    {
        const auto databasePath = workspace.GetPath() / "comment-spoof.db";
        InitializeFreshDatabase(databasePath);
        RewriteSchemaSql(
            databasePath,
            "inpx_sources",
            " CHECK(id = 1)",
            " /* CHECK(id = 1) */");
        REQUIRE_THROWS_WITH(
            InpxWebReader::DatabaseRuntime::CSchemaInitializer::Initialize(databasePath),
            Catch::Matchers::ContainsSubstring("inpx_sources' does not match the exact current definition"));
    }
}
