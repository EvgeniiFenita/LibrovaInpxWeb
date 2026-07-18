---
name: scan-pipeline
description: Change the INPX scan pipeline in InpxWebReader, including INPX records, FB2 parsing, archive access, cover caching, rescan reconciliation, cancellation, warnings, concurrency, and performance telemetry. Use for scan behavior changes.
---

# INPX scan workflow

1. Trace the full path: `libs/Inpx` -> `libs/App/InpxScanJobService` -> `libs/Parsing`/`libs/ScanSupport` -> `libs/Database` -> server progress DTOs.
2. Preserve read-only source behavior and deterministic cache reconciliation.
3. Define initial-scan, rescan, missing-source, malformed-entry, partial-failure, and cancellation semantics before editing.
4. Preserve per-INP hashes, segment order, delete-marker ownership, base record presence, and effective availability so skipped segments reconcile identically to parsed segments.
5. Keep source archive/entry identity in `inpx_book_locations`; do not require an archive for a segment without active FB2 records; keep warnings bounded and actionable.
6. Preserve Unicode, Cyrillic, CP1251/CP866, ZIP path safety, atomic cover publication, and cover-cache cleanup.
7. Bound workers/in-flight payloads and keep database writes serialized through the established writer path.
8. Add focused native tests for initial/rescan, add/change/remove/restore/reorder, delete markers, parser/database/rollback behavior, and server/web e2e coverage when public progress or results change.
9. Run the remote Linux/Docker lane for source mounts, concurrency, resource, or deployment-visible changes.
