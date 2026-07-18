# InpxWebReader — Product

## Purpose

InpxWebReader makes a large read-only INPX e-book catalog searchable and downloadable through a trusted home-LAN browser UI. The service is local-first, single-user, and self-hosted.

## Supported workflow

1. Mount one INPX source directory read-only.
2. Select or auto-detect one `.inpx` catalog and its ZIP archive root.
3. Scan metadata and FB2 payloads into a local SQLite/FTS cache.
4. Browse, search, filter, inspect details and covers in the web UI.
5. Download the original FB2 entry or optionally convert it to EPUB with Linux `fbc`.
6. Rescan the source and retain availability/warning/statistics information.

## Product invariants

- One user and one active INPX source.
- x86_64 Linux server (`linux/amd64`) and browser UI only.
- The INPX source is never modified.
- INPX source paths are deployment configuration and are never persisted in the portable cache database.
- Source identity is the SHA-256 digest of every byte in the INPX file; paths, filenames, timestamps, and archive-root locations do not contribute to it.
- Rescan is incremental by INP segment: unchanged segment hashes reuse cached rows, while added, changed, removed, restored, and reordered segments are reconciled independently.
- Delete markers remain associated with their INP segment. A marker in a later segment suppresses an earlier active record; removing or reordering that marker restores availability without reparsing the unchanged active segment.
- A rescan publishes metadata, FTS, availability, statistics, warnings, segment state, and source identity atomically. Failure, cancellation, or a source change before commit preserves the previous catalog snapshot.
- An INPX snapshot without `.inp` segments, missing required archive entries, corrupt FB2 payloads, duplicate active `LibId` values, and declared-size mismatches reject the scan instead of publishing a partial catalog. Segments containing no active FB2 records do not require an archive.
- A download is bound to the exact portable archive locator and source guards published by its scan. Source, archive, or entry mutation before publication fails the request without returning mixed-snapshot bytes.
- Runtime remains useful without Internet access after dependencies and image are prepared.
- Unicode and Cyrillic data are preserved across INPX, FB2, ZIP, SQLite, JSON, HTTP, logs, and filenames.
- LAN binding requires an access password carried as a bearer credential; loopback development may run without one.
- The cache is disposable and reconstructible from the source.

## In scope

- INPX parsing, source discovery, initial scan, rescan, cancellation, warnings, and progress.
- FB2 metadata, cover extraction, legacy text encodings, and genre normalization.
- SQLite schema version 1 with FTS5 search, facets, source locations, scan warnings, and statistics.
- Original download and optional FB2-to-EPUB conversion.
- Responsive React UI served by the native server.
- Docker/Compose deployment and remote Linux build/test/bundle tooling.

## Out of scope

- Additional runtime surfaces, non-Linux deployments, and non-x86_64 server architectures.
- Writable copies of source books, arbitrary folder import, EPUB catalog ingestion, and trash workflows.
- Collections, favorites, ratings, reviews, reading progress, and synchronization.
- Multiple active INPX sources, accounts, cloud services, public Internet hosting, and DRM.
- Compatibility with any database that does not match the current schema-version-1 shape.

## Version and compatibility

The application line starts at `0.1.0` and uses schema version 1. Before the first release, the current schema-v1 shape may change without backward compatibility. Delete an incompatible cache and rescan the INPX source; migration chains are not part of the product.

## Success criteria

- A clean Docker deployment discovers or accepts an INPX source and completes its initial scan.
- Search and facets return correct Cyrillic and Unicode results.
- Original downloads reproduce source bytes without modifying the source.
- Optional EPUB conversion reports success, failure, timeout, and cancellation distinctly.
- Restart reopens the current cache and rescan reconciles added, changed, removed, and restored INP segments, including entries omitted from a changed segment.
- A relocated cache uses the currently configured INPX and archive-root paths; without source configuration it remains browsable while scan and download actions are disabled.
- Remote Linux verification covers the image, Compose runtime, security posture, read-only source, concurrency, and resource budgets.
