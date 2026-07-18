#include "Database/DatabaseSchema.hpp"

#include <string>
#include <vector>

namespace InpxWebReader::DatabaseSchema {
namespace {

constexpr std::string_view GCreateSchemaScript = R"sql(
CREATE TABLE IF NOT EXISTS inpx_books (
    id INTEGER PRIMARY KEY,
    title TEXT NOT NULL,
    normalized_title TEXT NOT NULL,
    language TEXT NOT NULL,
    series TEXT,
    series_index REAL,
    publisher TEXT,
    year INTEGER,
    isbn TEXT,
    description TEXT,
    identifier TEXT,
    cover_path TEXT,
    added_at_utc TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS authors (
    id INTEGER PRIMARY KEY,
    normalized_name TEXT NOT NULL UNIQUE,
    display_name TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS book_authors (
    book_id INTEGER NOT NULL,
    author_id INTEGER NOT NULL,
    author_order INTEGER NOT NULL,
    PRIMARY KEY(book_id, author_id),
    FOREIGN KEY(book_id) REFERENCES inpx_books(id) ON DELETE CASCADE,
    FOREIGN KEY(author_id) REFERENCES authors(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS tags (
    id INTEGER PRIMARY KEY,
    normalized_name TEXT NOT NULL UNIQUE,
    display_name TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS book_tags (
    book_id INTEGER NOT NULL,
    tag_id INTEGER NOT NULL,
    PRIMARY KEY(book_id, tag_id),
    FOREIGN KEY(book_id) REFERENCES inpx_books(id) ON DELETE CASCADE,
    FOREIGN KEY(tag_id) REFERENCES tags(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS genres (
    id INTEGER PRIMARY KEY,
    normalized_name TEXT NOT NULL UNIQUE,
    display_name TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS book_genres (
    book_id INTEGER NOT NULL,
    genre_id INTEGER NOT NULL,
    PRIMARY KEY(book_id, genre_id),
    FOREIGN KEY(book_id) REFERENCES inpx_books(id) ON DELETE CASCADE,
    FOREIGN KEY(genre_id) REFERENCES genres(id) ON DELETE CASCADE
);

CREATE VIRTUAL TABLE IF NOT EXISTS search_index USING fts5(
    title,
    authors,
    tags,
    genres,
    description,
    content=''
);

CREATE TABLE IF NOT EXISTS inpx_sources (
    id INTEGER PRIMARY KEY CHECK(id = 1),
    display_name TEXT NOT NULL,
    source_fingerprint TEXT NOT NULL,
    last_scan_started_at_utc TEXT,
    last_scan_completed_at_utc TEXT,
    last_seen_snapshot_id TEXT
);

CREATE TABLE IF NOT EXISTS inpx_segments (
    id INTEGER PRIMARY KEY,
    source_id INTEGER NOT NULL,
    inp_entry_name TEXT NOT NULL,
    archive_name TEXT NOT NULL,
    inp_fingerprint TEXT NOT NULL,
    segment_order INTEGER NOT NULL DEFAULT 0 CHECK(segment_order >= 0),
    record_count INTEGER NOT NULL CHECK(record_count >= 0),
    active_record_count INTEGER NOT NULL CHECK(active_record_count >= 0),
    deleted_record_count INTEGER NOT NULL CHECK(deleted_record_count >= 0),
    availability TEXT NOT NULL CHECK(availability IN ('available', 'missing_from_index')),
    requires_archive INTEGER NOT NULL DEFAULT 0 CHECK(requires_archive IN (0, 1)),
    resolved_archive_path TEXT,
    archive_file_size_bytes INTEGER NOT NULL DEFAULT 0 CHECK(archive_file_size_bytes >= 0),
    archive_mtime_ticks INTEGER NOT NULL DEFAULT 0,
    archive_manifest_fingerprint TEXT,
    last_seen_scan_id TEXT,
    FOREIGN KEY(source_id) REFERENCES inpx_sources(id) ON DELETE CASCADE,
    UNIQUE(source_id, inp_entry_name),
    CHECK(record_count = active_record_count + deleted_record_count),
    CHECK((requires_archive = 0 AND resolved_archive_path IS NULL AND archive_manifest_fingerprint IS NULL)
       OR (requires_archive = 1 AND resolved_archive_path IS NOT NULL AND archive_manifest_fingerprint IS NOT NULL))
);

CREATE TABLE IF NOT EXISTS inpx_deletions (
    segment_id INTEGER NOT NULL,
    lib_id TEXT NOT NULL,
    PRIMARY KEY(segment_id, lib_id),
    FOREIGN KEY(segment_id) REFERENCES inpx_segments(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS inpx_book_locations (
    book_id INTEGER PRIMARY KEY,
    source_id INTEGER NOT NULL,
    segment_id INTEGER NOT NULL,
    lib_id TEXT NOT NULL,
    archive_name TEXT NOT NULL,
    entry_name TEXT NOT NULL,
    availability TEXT NOT NULL CHECK(availability IN ('available', 'missing_from_index')),
    present_in_segment INTEGER NOT NULL DEFAULT 1 CHECK(present_in_segment IN (0, 1)),
    file_size_bytes INTEGER NOT NULL DEFAULT 0,
    format TEXT NOT NULL DEFAULT 'fb2',
    last_seen_scan_id TEXT,
    FOREIGN KEY(book_id) REFERENCES inpx_books(id) ON DELETE CASCADE,
    FOREIGN KEY(source_id) REFERENCES inpx_sources(id) ON DELETE CASCADE,
    FOREIGN KEY(segment_id) REFERENCES inpx_segments(id) ON DELETE CASCADE,
    UNIQUE(source_id, lib_id)
);

CREATE TABLE IF NOT EXISTS inpx_scan_warnings (
    id INTEGER PRIMARY KEY,
    source_id INTEGER NOT NULL,
    scan_id TEXT NOT NULL,
    warning_code TEXT NOT NULL,
    archive_name TEXT,
    entry_name TEXT,
    message TEXT NOT NULL,
    created_at_utc TEXT NOT NULL,
    FOREIGN KEY(source_id) REFERENCES inpx_sources(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS catalog_statistics (
    singleton INTEGER PRIMARY KEY CHECK(singleton = 1),
    book_count INTEGER NOT NULL DEFAULT 0 CHECK(book_count >= 0),
    unavailable_book_count INTEGER NOT NULL DEFAULT 0 CHECK(unavailable_book_count >= 0),
    cover_cache_size_bytes INTEGER NOT NULL DEFAULT 0 CHECK(cover_cache_size_bytes >= 0),
    inpx_source_size_bytes INTEGER NOT NULL DEFAULT 0 CHECK(inpx_source_size_bytes >= 0),
    updated_at_utc TEXT NOT NULL
);
INSERT INTO catalog_statistics(
    singleton, book_count, unavailable_book_count,
    cover_cache_size_bytes, inpx_source_size_bytes, updated_at_utc)
VALUES(1, 0, 0, 0, 0, '1970-01-01T00:00:00Z')
ON CONFLICT(singleton) DO NOTHING;

CREATE TABLE IF NOT EXISTS catalog_stat_cover_files (
    cover_path TEXT PRIMARY KEY,
    reference_count INTEGER NOT NULL CHECK(reference_count > 0),
    size_bytes INTEGER NOT NULL CHECK(size_bytes >= 0)
);

CREATE INDEX IF NOT EXISTS idx_inpx_books_title_sort ON inpx_books(normalized_title, title, id);
CREATE INDEX IF NOT EXISTS idx_inpx_books_added_title_sort ON inpx_books(added_at_utc, normalized_title, title, id);
CREATE INDEX IF NOT EXISTS idx_inpx_books_language_title_sort ON inpx_books(language, normalized_title, title, id);
CREATE INDEX IF NOT EXISTS idx_inpx_books_cover_path ON inpx_books(cover_path);
CREATE INDEX IF NOT EXISTS idx_book_authors_author_id ON book_authors(author_id);
CREATE INDEX IF NOT EXISTS idx_book_tags_tag_id ON book_tags(tag_id);
CREATE INDEX IF NOT EXISTS idx_book_genres_genre_id ON book_genres(genre_id);
CREATE INDEX IF NOT EXISTS idx_book_genres_genre_book ON book_genres(genre_id, book_id);
CREATE INDEX IF NOT EXISTS idx_inpx_segments_source_id ON inpx_segments(source_id);
CREATE INDEX IF NOT EXISTS idx_inpx_deletions_lib_id ON inpx_deletions(lib_id);
CREATE INDEX IF NOT EXISTS idx_inpx_book_locations_source_id ON inpx_book_locations(source_id);
CREATE INDEX IF NOT EXISTS idx_inpx_book_locations_segment_id ON inpx_book_locations(segment_id);
CREATE INDEX IF NOT EXISTS idx_inpx_book_locations_availability ON inpx_book_locations(availability);
CREATE INDEX IF NOT EXISTS idx_inpx_scan_warnings_source_id ON inpx_scan_warnings(source_id);
CREATE INDEX IF NOT EXISTS idx_inpx_scan_warnings_scan_id ON inpx_scan_warnings(scan_id);
)sql";

const std::vector<std::string_view> GInitializationStatements{
    "PRAGMA foreign_keys = ON;",
    "PRAGMA journal_mode = WAL;",
    GCreateSchemaScript
};

const std::vector<SRequiredSchemaObject> GRequiredObjects{
    {"table", "inpx_books"},
    {"table", "authors"},
    {"table", "book_authors"},
    {"table", "tags"},
    {"table", "book_tags"},
    {"table", "genres"},
    {"table", "book_genres"},
    {"table", "search_index"},
    {"table", "inpx_sources"},
    {"table", "inpx_segments"},
    {"table", "inpx_deletions"},
    {"table", "inpx_book_locations"},
    {"table", "inpx_scan_warnings"},
    {"table", "catalog_statistics"},
    {"table", "catalog_stat_cover_files"}
};

const std::vector<SRequiredTableShape> GRequiredTableShapes{
    {"inpx_books", {
        {"id", "INTEGER", false, 1},
        {"title", "TEXT", true},
        {"normalized_title", "TEXT", true},
        {"language", "TEXT", true},
        {"series", "TEXT"},
        {"series_index", "REAL"},
        {"publisher", "TEXT"},
        {"year", "INTEGER"},
        {"isbn", "TEXT"},
        {"description", "TEXT"},
        {"identifier", "TEXT"},
        {"cover_path", "TEXT"},
        {"added_at_utc", "TEXT", true}
    }, {}, {}},
    {"authors", {
        {"id", "INTEGER", false, 1},
        {"normalized_name", "TEXT", true},
        {"display_name", "TEXT", true}
    }, {}, {"normalized_name text not null unique"}},
    {"book_authors", {
        {"book_id", "INTEGER", true, 1},
        {"author_id", "INTEGER", true, 2},
        {"author_order", "INTEGER", true}
    }, {
        {"book_id", "inpx_books", "id", "NO ACTION", "CASCADE"},
        {"author_id", "authors", "id", "NO ACTION", "CASCADE"}
    }, {}},
    {"tags", {
        {"id", "INTEGER", false, 1},
        {"normalized_name", "TEXT", true},
        {"display_name", "TEXT", true}
    }, {}, {"normalized_name text not null unique"}},
    {"book_tags", {
        {"book_id", "INTEGER", true, 1},
        {"tag_id", "INTEGER", true, 2}
    }, {
        {"book_id", "inpx_books", "id", "NO ACTION", "CASCADE"},
        {"tag_id", "tags", "id", "NO ACTION", "CASCADE"}
    }, {}},
    {"genres", {
        {"id", "INTEGER", false, 1},
        {"normalized_name", "TEXT", true},
        {"display_name", "TEXT", true}
    }, {}, {"normalized_name text not null unique"}},
    {"book_genres", {
        {"book_id", "INTEGER", true, 1},
        {"genre_id", "INTEGER", true, 2}
    }, {
        {"book_id", "inpx_books", "id", "NO ACTION", "CASCADE"},
        {"genre_id", "genres", "id", "NO ACTION", "CASCADE"}
    }, {}},
    {"inpx_sources", {
        {"id", "INTEGER", false, 1},
        {"display_name", "TEXT", true},
        {"source_fingerprint", "TEXT", true},
        {"last_scan_started_at_utc", "TEXT"},
        {"last_scan_completed_at_utc", "TEXT"},
        {"last_seen_snapshot_id", "TEXT"}
    }, {}, {"check(id = 1)"}},
    {"inpx_segments", {
        {"id", "INTEGER", false, 1},
        {"source_id", "INTEGER", true},
        {"inp_entry_name", "TEXT", true},
        {"archive_name", "TEXT", true},
        {"inp_fingerprint", "TEXT", true},
        {"segment_order", "INTEGER", true, 0, "0"},
        {"record_count", "INTEGER", true},
        {"active_record_count", "INTEGER", true},
        {"deleted_record_count", "INTEGER", true},
        {"availability", "TEXT", true},
        {"requires_archive", "INTEGER", true, 0, "0"},
        {"resolved_archive_path", "TEXT"},
        {"archive_file_size_bytes", "INTEGER", true, 0, "0"},
        {"archive_mtime_ticks", "INTEGER", true, 0, "0"},
        {"archive_manifest_fingerprint", "TEXT"},
        {"last_seen_scan_id", "TEXT"}
    }, {
        {"source_id", "inpx_sources", "id", "NO ACTION", "CASCADE"}
    }, {
        "check(record_count >= 0)",
        "check(active_record_count >= 0)",
        "check(deleted_record_count >= 0)",
        "check(segment_order >= 0)",
        "check(requires_archive in (0, 1))",
        "check(archive_file_size_bytes >= 0)",
        "check(availability in ('available', 'missing_from_index'))",
        "check(record_count = active_record_count + deleted_record_count)",
        "check((requires_archive = 0 and resolved_archive_path is null and archive_manifest_fingerprint is null)",
        "or (requires_archive = 1 and resolved_archive_path is not null and archive_manifest_fingerprint is not null))",
        "unique(source_id, inp_entry_name)"
    }},
    {"inpx_deletions", {
        {"segment_id", "INTEGER", true, 1},
        {"lib_id", "TEXT", true, 2}
    }, {
        {"segment_id", "inpx_segments", "id", "NO ACTION", "CASCADE"}
    }, {}},
    {"inpx_book_locations", {
        {"book_id", "INTEGER", false, 1},
        {"source_id", "INTEGER", true},
        {"segment_id", "INTEGER", true},
        {"lib_id", "TEXT", true},
        {"archive_name", "TEXT", true},
        {"entry_name", "TEXT", true},
        {"availability", "TEXT", true},
        {"present_in_segment", "INTEGER", true, 0, "1"},
        {"file_size_bytes", "INTEGER", true, 0, "0"},
        {"format", "TEXT", true, 0, "'fb2'"},
        {"last_seen_scan_id", "TEXT"}
    }, {
        {"book_id", "inpx_books", "id", "NO ACTION", "CASCADE"},
        {"source_id", "inpx_sources", "id", "NO ACTION", "CASCADE"},
        {"segment_id", "inpx_segments", "id", "NO ACTION", "CASCADE"}
    }, {
        "check(availability in ('available', 'missing_from_index'))",
        "check(present_in_segment in (0, 1))",
        "unique(source_id, lib_id)"
    }},
    {"inpx_scan_warnings", {
        {"id", "INTEGER", false, 1},
        {"source_id", "INTEGER", true},
        {"scan_id", "TEXT", true},
        {"warning_code", "TEXT", true},
        {"archive_name", "TEXT"},
        {"entry_name", "TEXT"},
        {"message", "TEXT", true},
        {"created_at_utc", "TEXT", true}
    }, {
        {"source_id", "inpx_sources", "id", "NO ACTION", "CASCADE"}
    }, {}},
    {"catalog_statistics", {
        {"singleton", "INTEGER", false, 1},
        {"book_count", "INTEGER", true, 0, "0"},
        {"unavailable_book_count", "INTEGER", true, 0, "0"},
        {"cover_cache_size_bytes", "INTEGER", true, 0, "0"},
        {"inpx_source_size_bytes", "INTEGER", true, 0, "0"},
        {"updated_at_utc", "TEXT", true}
    }, {}, {
        "check(singleton = 1)",
        "check(book_count >= 0)",
        "check(unavailable_book_count >= 0)",
        "check(cover_cache_size_bytes >= 0)",
        "check(inpx_source_size_bytes >= 0)"
    }},
    {"catalog_stat_cover_files", {
        {"cover_path", "TEXT", false, 1},
        {"reference_count", "INTEGER", true},
        {"size_bytes", "INTEGER", true}
    }, {}, {
        "check(reference_count > 0)",
        "check(size_bytes >= 0)"
    }}
};

const std::vector<SRequiredIndexShape> GRequiredIndexShapes{
    {"idx_inpx_books_title_sort", {"normalized_title", "title", "id"}},
    {"idx_inpx_books_added_title_sort", {"added_at_utc", "normalized_title", "title", "id"}},
    {"idx_inpx_books_language_title_sort", {"language", "normalized_title", "title", "id"}},
    {"idx_inpx_books_cover_path", {"cover_path"}},
    {"idx_book_authors_author_id", {"author_id"}},
    {"idx_book_tags_tag_id", {"tag_id"}},
    {"idx_book_genres_genre_id", {"genre_id"}},
    {"idx_book_genres_genre_book", {"genre_id", "book_id"}},
    {"idx_inpx_segments_source_id", {"source_id"}},
    {"idx_inpx_deletions_lib_id", {"lib_id"}},
    {"idx_inpx_book_locations_source_id", {"source_id"}},
    {"idx_inpx_book_locations_segment_id", {"segment_id"}},
    {"idx_inpx_book_locations_availability", {"availability"}},
    {"idx_inpx_scan_warnings_source_id", {"source_id"}},
    {"idx_inpx_scan_warnings_scan_id", {"scan_id"}}
};

const SFtsTableShape GRequiredSearchIndexShape{
    .Name = "search_index",
    .Columns = {"title", "authors", "tags", "genres", "description"},
    .RequiredSqlTokens = {"using fts5", "content=''"}
};

} // namespace

int CDatabaseSchema::GetCurrentVersion() noexcept
{
    return 1;
}

const std::vector<std::string_view>& CDatabaseSchema::GetInitializationStatements()
{
    return GInitializationStatements;
}

std::string_view CDatabaseSchema::GetCreateSchemaScript() noexcept
{
    return GCreateSchemaScript;
}

const std::vector<SRequiredSchemaObject>& CDatabaseSchema::GetRequiredObjects()
{
    return GRequiredObjects;
}

const std::vector<SRequiredTableShape>& CDatabaseSchema::GetRequiredTableShapes()
{
    return GRequiredTableShapes;
}

const std::vector<SRequiredIndexShape>& CDatabaseSchema::GetRequiredIndexShapes()
{
    return GRequiredIndexShapes;
}

const SFtsTableShape& CDatabaseSchema::GetRequiredSearchIndexShape()
{
    return GRequiredSearchIndexShape;
}

} // namespace InpxWebReader::DatabaseSchema
