---
name: sqlite
description: Change or review InpxWebReader SQLite schema version 1, initialization, FTS5 search, source locations, scan warnings, statistics, transactions, and native SQL. Use for DatabaseSchema, SchemaInitializer, repository queries, or SQLite correctness/performance work.
---

# SQLite workflow

1. Read `libs/Database/DatabaseSchema.*`, `SchemaInitializer.*`, affected repository code, and schema tests.
2. Treat this as a pre-release disposable cache database. Change the current schema-version-1 shape when needed and update all owning code and tests together. Do not preserve earlier schema-v1 shapes or add migrations; incompatible caches are recreated.
3. Keep only INPX web data: catalog metadata, authors/tags/genres, FTS index, source/location identity, scan warnings, cover statistics, and source statistics.
4. Update DDL, required object/table/index/FTS shape validation, queries, and tests in one change.
5. Use bound parameters and explicit transactions; preserve foreign keys, rollback, and FTS/statistics side effects.
6. Consider indexes and query plans for list/search/facet/rescan paths.
7. Test fresh initialization, damaged/partial rejection, Cyrillic/Unicode search, filters/facets, pagination, and relevant scan reconciliation.
8. Verify on remote Linux with `database-schema|sqlite|inpx` native coverage.
