# InpxWebReader — Linux/Docker Deployment

## Image and mounts

Build from repository root:

```sh
docker build -f deploy/inpx-web-reader/Dockerfile -t inpx-web-reader:latest .
```

The supported image platform is `linux/amd64`; the native toolchain and optional bundled converter are x86_64. CMake configuration, remote bootstrap/test helpers, Docker build arguments, image inspection, Compose, and generated bundle scripts fail closed on any other architecture instead of relying on emulation.

The runtime image contains `/opt/inpx-web-reader/inpx-web-reader` and `/opt/inpx-web-reader/web`. Its default identity is unprivileged uid/gid `10001`; base Compose can select another explicit non-root uid/gid for NAS source and secret permissions. The service drops capabilities, uses a read-only root filesystem in Compose, and exposes port 8080.

Required mounts:

- read-only INPX source -> `/source:ro`
- writable application data -> `/data`
- bearer token file -> Docker secret

Persistent data contains `/data/cache/Database/inpx-web-reader.db`, `/data/cache/Covers`, `/data/runtime`, and logs. Deleting data forces a clean rescan; no database migration is provided.

INPX and archive-root paths are runtime configuration, not persisted cache data. Moving `/data/cache` therefore does not retain an old `/source` mount path; keep the source configured at its current container location.

## Compose

Copy `deploy/inpx-web-reader/.env.template` to an ignored `.env`. Set:

- `INPX_WEB_READER_SOURCE_PATH`
- `INPX_WEB_READER_DATA_PATH`
- `INPX_WEB_READER_AUTH_TOKEN_FILE`

These three values are host paths. Optional `INPX_WEB_READER_INPX_PATH` and `INPX_WEB_READER_ARCHIVE_ROOT` are absolute paths inside the container under `/source`, for example `/source/catalog.inpx` and `/source/lib.rus.ec`. `INPX_WEB_READER_CONVERTER_PATH` is likewise a container path and is usable only when `fbc` is added to the image or mounted through a Compose override. Then run:

```sh
docker compose \
  --env-file deploy/inpx-web-reader/.env \
  -f deploy/inpx-web-reader/docker-compose.yml \
  up -d
```

The entrypoint auto-detects a single `.inpx` and archive root only when both explicit values are absent, writes a private runtime JSON config, and starts `inpx-web-reader`. A partial explicit source pair is rejected rather than completed by auto-detection.

## Logging

The server writes the same UTC-timestamped records to Docker stderr and to `/data/runtime/Logs/inpx-web-reader.log`. Use `docker compose logs inpx-web-reader` for the container stream; use the file under `/data` when persistent logs must survive a container recreation. The file rotates instead of growing without a bound. Defaults retain the active 20 MiB file plus four rotated files and can be changed with:

- `INPX_WEB_READER_LOG_LEVEL` (`debug`, `info`, `warning`, or `error`; default `info`)
- `INPX_WEB_READER_LOG_MAX_FILE_SIZE_MIB` (default `20`, maximum `1024`)
- `INPX_WEB_READER_LOG_MAX_ROTATED_FILES` (default `4`, maximum `100`)

Parser and scan records intentionally include filesystem paths, archive-entry names, and bounded FB2/XML fragments such as `xml_preview`. Preview collection stops at its byte cap while traversing XML instead of serializing an arbitrarily large subtree first. This context is always present at the level of the underlying event and has no separate opt-in mode because it is needed to diagnose source-specific parser failures. Never publish these logs as public artifacts: they may expose private source layout or book text. Tokens and passwords must never be logged.

`/api/health` is the public process-liveness probe and remains successful while the catalog backend starts. `/api/ready` is the public readiness probe used by the image and Compose healthchecks; it returns `503` until the backend is open. Successful probe requests are debug-only so healthchecks do not fill the production info log. Failed probes and ordinary API/static-asset requests remain visible. The React application currently has no separate browser-to-server logging stream; a successful `path=/` HTTP record proves that the packaged web entry point was served, while browser-console output is not persisted by the deployment.

The resolved `inpxSource.inpxPath` and `inpxSource.archiveRoot` values are authoritative on every start, including when an existing cache database is opened. Both values may be omitted for an existing cache, but a partial explicit source configuration is rejected before automatic detection. Automatic detection publishes a source only when it finds both halves; finding an INPX file without an archive root leaves the source unconfigured so an existing cache can still start browse-only. The cache identifies the source from every byte of the INPX file, so moving or renaming an unchanged file preserves its identity. If the configured bytes differ from the last successful scan, cached browsing remains available, downloads are disabled, and a rescan is required. When an existing cache starts without source configuration or with an unavailable source, browsing and cached covers remain available while downloads and rescans are disabled.

EPUB conversion has a five-minute production deadline and remains distinct from explicit request or server-shutdown cancellation. A timed-out or cancelled converter and its descendants are stopped, and staged files are removed. Conversion must produce exactly one regular EPUB; symlinked, non-regular, or multiple candidate outputs fail. Catalog status and control operations remain available while conversion runs.

Startup scan policy is configured by `startup.autoScan`, `startup.autoScanOnEmptyCache`, and `startup.autoRescanOnSourceChange`. `autoScan` is the master switch. With it enabled, a newly created empty cache is scanned when `autoScanOnEmptyCache` is true; an existing cache whose configured INPX digest changed is rescanned when the opt-in `autoRescanOnSourceChange` is true. Automatic and manual rescans use the same per-INP incremental and atomic reconciliation path.

Incremental rescans reuse unchanged INP segments and skip unchanged archive work when cached guards match. Changed archive metadata triggers manifest verification, so repacked archives and same-size payload changes remain detectable. The common append-only catalog update is therefore proportional to added or changed segments. Implementation-level download guards, converter isolation, scan bounds, archive indexing, and the `libzip` boundary are owned by the [Codebase Map](CodebaseMap.md#database).

## Security

- A non-loopback host requires a non-empty bearer token.
- Keep the token in a mounted file, never in tracked `.env` or command history.
- Treat the server as trusted-LAN software; do not expose it directly to the public Internet.
- Preserve the read-only source mount and Compose hardening.
- Ensure the writable data path is accessible to the configured container identity.

## Remote Linux host

The bootstrap workflow supports a dedicated x86_64 Ubuntu or Debian build machine. Other build-host distributions and architectures are not supported by `BootstrapRemoteLinux.py`.

Repository-root `.env.example` defines only:

```text
INPX_WEB_READER_BUILD_HOST
INPX_WEB_READER_BUILD_USER
INPX_WEB_READER_BUILD_PASSWORD
INPX_WEB_READER_BUILD_WORKDIR
```

Bootstrap once, then verify:

```sh
python3 scripts/BootstrapRemoteLinux.py
python3 scripts/RunRemoteLinux.py test
```

Run the real-server Playwright workflow from a non-Linux development host with:

```sh
python3 scripts/RunRemoteLinux.py e2e
```

The orchestrator creates a temporary source snapshot under local `out/` from Git-indexed files, refuses untracked non-ignored paths, synchronizes that snapshot without ignored credentials or build output, preserves remote dependency caches, and reconnects to an active persistent job instead of racing it. Add or mark new source files in the Git index before verification. The host is a dedicated worker, so build, test, analysis, sanitizer, Docker, and browser lanes use all detected CPU threads by default. Lower parallelism only after an observed resource failure or an explicit request.

The development-side orchestrator is supported on Windows and macOS. It selects the SSH executable beside `rsync` when available and creates a host-native temporary askpass helper under `out/`; all configure, build, test, and Docker work still executes only on the Linux worker.

## Deployment bundle

Build and retrieve a Linux Docker/NAS bundle:

```sh
python3 scripts/RunRemoteLinux.py bundle \
  --nas-source-root /volume/books/inpx \
  --nas-app-root /volume/docker/inpx-web-reader
```

The output defaults to `out/deploy/inpx-web-reader` and contains the image archive, Compose files, token layout, and run/stop scripts. The run script selects a source-readable non-root uid/gid, and base Compose applies that identity in both converter-enabled and converter-free modes before the script verifies source, token, and data permissions. By default the bundle also downloads and wires the optional converter. Pass `--skip-converter-download` for a genuinely converter-free bundle: no converter mount or override is used, conversion stays disabled even if an older override remains on the NAS, and the run script still verifies that the loaded server image is exactly `linux/amd64`.

## Verification contract

`RunRemoteLinux.py test` must pass after changes to native Linux behavior, dependencies, Dockerfile, Compose, entrypoint, mounts, security, image contents, permissions, source scanning, download behavior, logging, or resource limits. It verifies native CTest, exact amd64 image shape, Compose readiness, rejection of missing/incorrect bearer tokens and partial source configuration, a 2,048-book initial import by default, catalog/search/cover/exact-original-download behavior at both the first and last generated segment, packaged web entry-point serving, concurrent polling, token-log hygiene, read-only source integrity, memory budget, and required UTC backend/web-serving records in both Docker output and the persistent runtime log. The smoke then changes an existing book, adds a book, publishes only the affected source archives from a regenerated staging catalog, and requires an incremental rescan to publish a new snapshot with exact parsed/reused/added/updated segment metrics. A container restart must reopen that snapshot with the same stable book id and exact source bytes. `--smoke-book-count` selects a different deterministic initial catalog size; every count, scan-result, statistics, source-summary, and persistence assertion is derived from that value.

This is the authoritative Linux/Docker integration lane, not a replacement for the change-specific script, web, static-analysis, or sanitizer checks in `AGENTS.md`.
