# InpxWebReader — Codebase Map

## Runtime flow

```text
browser React UI
    -> HTTP/JSON and downloads
apps/InpxWebReader + libs/Server
    -> libs/App facade and scan coordinator
    -> libs/Inpx + libs/Parsing + libs/ScanSupport
    -> libs/Database + SQLite/FTS5
    -> read-only INPX/ZIP source + writable cover/runtime cache
```

The process is headless and x86_64 Linux-only. Static web assets are built by Vite and embedded into the `linux/amd64` Docker image beside `inpx-web-reader`.

## Native modules

| Module | Responsibility |
|---|---|
| `libs/Domain` | Book/search types, repository interfaces, format and normalization rules |
| `libs/Foundation` | Unicode/iconv, filesystem, logging, limits, version, SHA-256, ZIP-path and filename helpers |
| `libs/Inpx` | INPX archive/record parsing, per-INP segment payloads/hashes, and source configuration |
| `libs/Parsing` | FB2 metadata/cover parsing and genre normalization |
| `libs/ScanSupport` | Scan concurrency, cover-cache processing, and performance telemetry |
| `libs/Storage` | INPX/runtime layouts, rooted path safety, INPX archive resolution, and cover processing |
| `libs/Database` | Schema v1, SQLite wrappers, FTS/search query repository, statistics and index maintenance |
| `libs/Converter` | Linux `fbc` validation, command building, execution, cancellation, and result handling |
| `libs/App` | Cache bootstrap, catalog/query/download facade, archive access, and scan job service |
| `libs/Server` | Config, auth, executor, HTTP routes, DTO mapping, host and scan coordination |
| `apps/InpxWebReader` | CLI, signal handling, logger startup, host composition, and process lifetime |

All production C++ symbols use namespace `InpxWebReader`. CMake libraries use the same prefix; the executable target is `InpxWebReader` with output name `inpx-web-reader`.

## Web UI

`web/inpx-web-reader` contains React/TypeScript source, API mapping, components, CSS, Vitest tests, Playwright tests, and Vite configuration. `scripts/RunWebUi.py` copies sources and installs dependencies under `out/web/inpx-web-reader`; source directories never receive `node_modules` or build output. Linux browser tests use the official Playwright image whose version is read from `package-lock.json`.

## HTTP surface

- `GET /api/health`, `/api/ready`, `/api/version`, `/api/status`, `/api/stats`, `/api/source`
- `GET /api/books`, `/api/books/{id}`, `/api/books/{id}/download`
- `GET /api/covers/{id}`
- `POST /api/scan/start`, `/api/scan/cancel`
- `GET /api/scan/progress`
- non-API routes serve the built SPA

## Database

The database is `Database/inpx-web-reader.db`, schema version 1. It contains:

- `inpx_books`, authors/tags/genres and their join tables;
- contentless FTS5 `search_index`;
- singleton `inpx_sources` with path-independent full-file SHA-256 source identity and scan metadata;
- `inpx_segments` with per-INP hashes, stable order, portable selected-archive locators, archive stat/manifest guards, counts, availability, and last-seen scan IDs;
- `inpx_deletions` with segment-owned delete markers used during incremental reconciliation;
- `inpx_book_locations` linked to their active INP segment with source archive/entry metadata, base presence, and effective availability;
- `inpx_scan_warnings`;
- `catalog_statistics` and `catalog_stat_cover_files`.

`CSchemaInitializer` accepts an empty database or validates the current schema-version-1 shape. Any other, partial, or earlier pre-release schema-v1 shape is rejected and must be recreated. The current shape may change before the first release, but each change must update DDL, validation, queries, and tests together. Do not add migration history.

Absolute INPX and archive-root paths are supplied by the active server configuration and are not stored in SQLite. Cached cover paths and the exact archive/entry locators selected by a successful scan remain safe paths relative to those roots. An existing cache can therefore move between data roots or deployments without retaining host/container paths, while a later lexically earlier duplicate archive cannot silently retarget a download.

Original and converted downloads read filename metadata and the persisted archive locator guards from one SQLite snapshot, then revalidate the full INPX digest, selected archive stat and manifest, and entry size before and after staging and once more after atomic publication. Manifest validation, entry-size lookup, and extraction use one `O_NOFOLLOW`/`zip_fdopen` archive descriptor, so replacing the archive path cannot retarget an admitted read. A failed final guard restores the previous destination. Each request uses atomically created source-staging and publication workspaces, so concurrent downloads of the same book cannot overwrite one another. Conversion runs outside the serialized catalog backend in its own request workspace, has a bounded runtime, and receives request/shutdown cancellation; Linux termination targets the converter process group with TERM followed by KILL and descendant reaping. Linux converter launches use a small environment allowlist, close inherited file descriptors, and prepare paths, limits, and environment storage before `fork()`. Built-in converter validation probes the identity-checked executable descriptor, bounds captured help output, and reaps the probe process group. Converter output is accepted only from a no-follow regular-file descriptor, copied into an application-owned file, revalidated, and atomically sealed before download publication; symlinks, non-regular outputs, and ambiguous multiple EPUB results fail.

The first catalog-list page is assembled on one SQLite connection inside an explicit read transaction. It materializes the complete filtered and sorted book-id order once, computes total count and facets once, hydrates the first slice, and binds an opaque continuation cursor to the source fingerprint, `catalogSnapshotId`, and effective query. Later cursor pages validate that identity before hydration and read only their in-memory ID slice, so they repeat neither FTS/sort work nor count/facet queries. Cursor sessions use an LRU/TTL cache of at most eight sessions and track their aggregate capacity as a number of book IDs. The configured steady-state memory envelope is converted to that capacity once with a coarse 16-byte allowance per ID; allocator-specific capacities and overheads are not modeled. First-page ID capacity is reserved and published under a generation-aware session lock with committed-identity checks before both operations and one post-publication recheck; a generation switch invalidates old reservations in one linear pass, while same-generation reserve/publish remains logarithmic. Eviction, restart, and a committed catalog change therefore return recoverable typed conflicts instead of mixing generations or allowing an obsolete publisher to evict current cursors. Legacy one-shot `offset` requests remain supported, but `offset` cannot be combined with `cursor`.

`CInpxScanJobService` copies the INPX file into its runtime job workspace, hashes and plans each INP segment, and opens archives only for segments containing active FB2 records. Planning is bounded with coarse limits on cumulative INP input volume and on record, segment, warning, and fallback-filesystem-entry counts; these limits are derived from the configured memory envelope without tracking `sizeof`, container capacity, or allocator overhead at runtime. One INP record line is capped at 1 MiB. Each record accepts at most 256 raw author components and 512 raw genre or keyword components; empty components count toward these defensive limits and are otherwise ignored. An author component accepts at most three raw comma-separated name parts, while integer and file-extension lexical fields are rejected above 32 bytes before parsing or case folding. FB2 scheduling bounds aggregate raw payload bytes in flight with conservative decode headroom; metadata input is capped at 8 MiB, 100,000 XML nodes, and 200,000 attributes to bound DOM amplification. Application-controlled file hashing checks cancellation for every 1 MiB input chunk; decoding, ZIP inflation, metadata analysis, binary lookup, and base64 byte passes checkpoint at each application pass boundary and at least every 64 KiB within a pass, while manifest ordering also checkpoints its comparison work.

An unchanged required archive is skipped by its cached size/mtime guard; a changed guard is compared through an order-independent ZIP manifest of entry name, uncompressed size, and CRC32 before FB2 bytes are read. Application-owned manifest work is bounded by a coarse entry-count allowance and aggregate entry-name volume, hashes the sorted representation incrementally, and checks cancellation during every application-controlled pass. Archive-name resolution checks the direct root candidate without enumerating the tree; only a missing direct candidate builds one cancellation-aware fallback snapshot per scan, capped by visited filesystem entries, in `O(F log F)`, after which lookups are `O(log F)` and preserve lexicographic immediate-child precedence. Changed segment work is sorted by resolved archive path in `O(S log S)` time, so aliases and non-adjacent INP segments share one execution open/manifest pass per archive instead of repeatedly rescanning it. Source-size statistics read distinct persisted `resolved_archive_path` values rather than rediscovering archives by name. The scan keeps the descriptor that matched the planned manifest for the following entry-size checks and payload reads, so path replacement cannot retarget an admitted archive. `libzip` still materializes its central-directory metadata inside its open operation before the application can inspect the entry count, so cancellation and application limits cannot interrupt that library-internal allocation. Archive reads must also reach a validated end of stream so decompression, size, and CRC errors cannot publish a segment. Changed and restored segments are authoritative for their linked rows. Segment order and persisted delete markers allow unchanged active records to become unavailable or be restored when marker segments are added, removed, or reordered. The final source digest is rechecked immediately before one SQLite commit. Covers use validated content-addressed SHA-256 paths and atomic file replacement; each new final path is registered for rollback before rename, while unreferenced cover files are collected only after commit. Cache bootstrap rejects symlinked managed roots/files, and cover writes plus garbage collection revalidate containment and the exact content-addressed layout.

Scan progress reports reused records, segment change counts, skipped/opened archives, and archive payload bytes read. Periodic progress logging captures a compact scalar/string view rather than copying the public snapshot's bounded warning vector. Persisted scan warnings are bounded to 1,000 rows per scan and the latest 10 warning-bearing scan IDs.

## Build and deployment

- Presets: `linux-debug`, `linux-release`, `linux-analysis`, `linux-asan`, `linux-tsan`, `linux-coverage`, `linux-fuzz`.
- Native dependencies: vcpkg manifest in `vcpkg.json`.
- Runtime version owner: `VERSION.txt`; release updates keep `vcpkg.json` and the web package manifest synchronized.
- Docker: `deploy/inpx-web-reader`.
- Remote orchestration: `BootstrapRemoteLinux.py` and `RunRemoteLinux.py`.
- Linux verification/release workers: `RunLinuxTests.py` and `RunRelease.py`.
- NAS/Linux Docker bundle: `PrepareDeployBundle.py`.

## Test navigation

- Native Catch2: `tests/Unit`, one target `InpxWebReaderCoreTests`, with Catch2 tags exported as CTest labels.
- Python helper tests: `tests/Scripts` through `RunScriptTests.py`.
- Web unit/component: `RunWebUi.py test`.
- Browser/real server: `RunWebUi.py test:e2e` on Linux or `RunRemoteLinux.py e2e` from another development host.
- Docker/Compose end to end: `RunRemoteLinux.py test`.
- Verified NAS release bundle: `RunRemoteLinux.py release`; deployment to the NAS remains manual.
- Native coverage: `RunCoverage.py` under `out/reports/coverage/native`; web coverage: `RunWebUi.py test:coverage` under `out/web/inpx-web-reader/coverage/web`.
- Memory/race hardening: `RunSanitizers.py` with `linux-asan` or `linux-tsan`.
- FB2 and INPX/archive fuzzing: `RunFuzzers.py`, with deterministic seed corpora and crash artifacts under `out/fuzz`.

## Frozen decisions

1. x86_64 Linux is the only native/runtime platform; Docker artifacts use `linux/amd64`.
2. The INPX source is read-only; cached metadata and covers are disposable.
3. One process serves API and static UI.
4. One active INPX source; no multi-source workspace.
5. SQLite/FTS5 is the only persistence/search engine.
6. Schema version 1 is a disposable pre-release cache: its current shape may change, and incompatible caches are recreated rather than migrated.
7. Optional conversion uses a configured Linux executable and must preserve cancellation semantics.
8. All transient developer/test state belongs under `out/`.
