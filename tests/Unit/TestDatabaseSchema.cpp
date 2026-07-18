#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>

#include "Database/DatabaseSchema.hpp"
#include "Database/SchemaInitializer.hpp"
#include "Database/SqliteConnection.hpp"
#include "Database/SqliteStatement.hpp"
#include "TestWorkspace.hpp"

namespace {

[[nodiscard]] bool HasObject(const std::string_view name)
{
    const auto& objects = InpxWebReader::DatabaseSchema::CDatabaseSchema::GetRequiredObjects();
    return std::ranges::any_of(objects, [name](const auto& object) { return object.Name == name; });
}

[[nodiscard]] const InpxWebReader::DatabaseSchema::SRequiredTableShape* FindTable(const std::string_view name)
{
    const auto& tables = InpxWebReader::DatabaseSchema::CDatabaseSchema::GetRequiredTableShapes();
    const auto iterator = std::ranges::find_if(tables, [name](const auto& table) { return table.Name == name; });
    return iterator == tables.end() ? nullptr : &*iterator;
}

[[nodiscard]] bool HasColumn(
    const InpxWebReader::DatabaseSchema::SRequiredTableShape& table,
    const std::string_view name)
{
    return std::ranges::any_of(table.Columns, [name](const auto& column) { return column.Name == name; });
}

} // namespace

TEST_CASE("INPX web database schema starts a new version line", "[database-schema][inpx]")
{
    REQUIRE(InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion() == 1);
    REQUIRE(HasObject("inpx_books"));
    REQUIRE(HasObject("inpx_sources"));
    REQUIRE(HasObject("inpx_segments"));
    REQUIRE(HasObject("inpx_deletions"));
    REQUIRE(HasObject("inpx_book_locations"));
    REQUIRE(HasObject("inpx_scan_warnings"));
    REQUIRE(HasObject("search_index"));
    REQUIRE(HasObject("catalog_statistics"));
    REQUIRE(HasObject("catalog_stat_cover_files"));
}

TEST_CASE("INPX location owns source file metadata", "[database-schema][inpx]")
{
    const auto* source = FindTable("inpx_sources");
    REQUIRE(source != nullptr);
    REQUIRE(HasColumn(*source, "source_fingerprint"));
    REQUIRE_FALSE(HasColumn(*source, "inpx_path"));
    REQUIRE_FALSE(HasColumn(*source, "archive_root"));

    const auto* locations = FindTable("inpx_book_locations");
    REQUIRE(locations != nullptr);
    REQUIRE(HasColumn(*locations, "archive_name"));
    REQUIRE(HasColumn(*locations, "entry_name"));
    REQUIRE(HasColumn(*locations, "availability"));
    REQUIRE(HasColumn(*locations, "file_size_bytes"));
    REQUIRE(HasColumn(*locations, "format"));
    REQUIRE(HasColumn(*locations, "segment_id"));
    REQUIRE(HasColumn(*locations, "present_in_segment"));

    const auto* segments = FindTable("inpx_segments");
    REQUIRE(segments != nullptr);
    REQUIRE(HasColumn(*segments, "inp_entry_name"));
    REQUIRE(HasColumn(*segments, "inp_fingerprint"));
    REQUIRE(HasColumn(*segments, "segment_order"));
    REQUIRE(HasColumn(*segments, "requires_archive"));
    REQUIRE(HasColumn(*segments, "resolved_archive_path"));
    REQUIRE(HasColumn(*segments, "archive_manifest_fingerprint"));
}

TEST_CASE("INPX schema enforces singleton counts source identity and cascades", "[database-schema][sqlite][inpx]")
{
    CTestWorkspace workspace("inpx-schema-constraints");
    const auto databasePath = workspace.GetPath() / "constraints.db";
    InpxWebReader::DatabaseRuntime::CSchemaInitializer::Initialize(databasePath);
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);

    REQUIRE_THROWS(connection.Execute(
        "INSERT INTO inpx_sources(id, display_name, source_fingerprint) "
        "VALUES(2, 'two.inpx', 'sha256:two');"));
    REQUIRE_THROWS(connection.Execute(
        "UPDATE catalog_statistics SET book_count = -1 WHERE singleton = 1;"));

    connection.Execute(
        "INSERT INTO inpx_sources(id, display_name, source_fingerprint) "
        "VALUES(1, 'one.inpx', 'sha256:one');");
    REQUIRE_THROWS(connection.Execute(
        "INSERT INTO inpx_segments(id, source_id, inp_entry_name, archive_name, inp_fingerprint, "
        "record_count, active_record_count, deleted_record_count, availability) "
        "VALUES(2, 1, 'invalid-count.zip.inp', 'invalid-count.zip', 'sha256:segment', 3, 1, 1, 'available');"));
    REQUIRE_THROWS(connection.Execute(
        "INSERT INTO inpx_segments(id, source_id, inp_entry_name, archive_name, inp_fingerprint, "
        "record_count, active_record_count, deleted_record_count, availability) "
        "VALUES(3, 1, 'invalid-state.zip.inp', 'invalid-state.zip', 'sha256:segment', 1, 1, 0, 'stale');"));
    REQUIRE_THROWS(connection.Execute(
        "INSERT INTO inpx_segments(id, source_id, inp_entry_name, archive_name, inp_fingerprint, "
        "record_count, active_record_count, deleted_record_count, availability, requires_archive) "
        "VALUES(4, 1, 'missing-guard.zip.inp', 'missing-guard.zip', 'sha256:segment', 1, 1, 0, 'available', 1);"));
    connection.Execute(
        "INSERT INTO inpx_segments(id, source_id, inp_entry_name, archive_name, inp_fingerprint, "
        "record_count, active_record_count, deleted_record_count, availability) "
        "VALUES(1, 1, 'one.zip.inp', 'one.zip', 'sha256:segment', 2, 2, 0, 'available');");
    connection.Execute(
        "INSERT INTO inpx_books(id, title, normalized_title, language, added_at_utc) "
        "VALUES(1, 'One', 'one', 'en', '2026-01-01T00:00:00Z'), "
        "(2, 'Two', 'two', 'en', '2026-01-01T00:00:00Z');");
    connection.Execute(
        "INSERT INTO inpx_book_locations(book_id, source_id, segment_id, lib_id, archive_name, entry_name, availability) "
        "VALUES(1, 1, 1, 'duplicate', 'one.zip', 'one.fb2', 'available');");
    connection.Execute(
        "INSERT INTO inpx_deletions(segment_id, lib_id) VALUES(1, 'deleted');");
    REQUIRE_THROWS(connection.Execute(
        "INSERT INTO inpx_book_locations(book_id, source_id, segment_id, lib_id, archive_name, entry_name, availability) "
        "VALUES(2, 1, 1, 'invalid-state', 'two.zip', 'two.fb2', 'stale');"));
    REQUIRE_THROWS(connection.Execute(
        "INSERT INTO inpx_book_locations(book_id, source_id, segment_id, lib_id, archive_name, entry_name, "
        "availability, present_in_segment) "
        "VALUES(2, 1, 1, 'invalid-presence', 'two.zip', 'two.fb2', 'available', 2);"));
    REQUIRE_THROWS(connection.Execute(
        "INSERT INTO inpx_book_locations(book_id, source_id, segment_id, lib_id, archive_name, entry_name, availability) "
        "VALUES(2, 1, 1, 'duplicate', 'two.zip', 'two.fb2', 'available');"));

    connection.Execute(
        "INSERT INTO inpx_scan_warnings(source_id, scan_id, warning_code, message, created_at_utc) "
        "VALUES(1, 'scan', 'fixture', 'fixture warning', '2026-01-01T00:00:00Z');");
    connection.Execute("DELETE FROM inpx_sources WHERE id = 1;");

    InpxWebReader::Sqlite::CSqliteStatement warningCount(
        connection.GetNativeHandle(),
        "SELECT COUNT(*) FROM inpx_scan_warnings;");
    REQUIRE(warningCount.Step());
    REQUIRE(warningCount.GetColumnInt(0) == 0);

    InpxWebReader::Sqlite::CSqliteStatement deletionCount(
        connection.GetNativeHandle(),
        "SELECT COUNT(*) FROM inpx_deletions;");
    REQUIRE(deletionCount.Step());
    REQUIRE(deletionCount.GetColumnInt(0) == 0);
}
