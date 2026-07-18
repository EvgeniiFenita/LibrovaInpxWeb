# InpxWebReader

InpxWebReader is an x86_64 Linux server and browser UI for browsing a read-only INPX e-book catalog. It scans one INPX source, stores searchable metadata in SQLite and generated covers in a filesystem cache, serves a React UI and HTTP API, and streams original FB2 files from the source archives. Optional FB2-to-EPUB download conversion is supported through a Linux `fbc` executable.

The only supported runtime is the Linux service, normally deployed with Docker, with its responsive browser interface.

## Runtime

- Executable: `inpx-web-reader`
- CMake target and C++ namespace: `InpxWebReader`
- Docker image: `inpx-web-reader`
- Environment prefix: `INPX_WEB_READER_`
- Supported platform: x86_64 Linux (`linux/amd64`), normally through Docker

The source directory is mounted read-only. Writable state lives under `/data`: persistent catalog cache is under `/data/cache`, including `Database/inpx-web-reader.db` and `Covers`, while runtime files and logs are under `/data/runtime`.

INPX and archive-root paths always come from the current runtime configuration and are not stored in SQLite. The cache identifies its source by a full-file SHA-256 digest, so an unchanged source and the complete cache directory can be relocated without retaining old host or container paths.

## Quick start

Build the image on Linux:

```sh
docker build \
  -f deploy/inpx-web-reader/Dockerfile \
  -t inpx-web-reader:latest \
  .
```

Copy `deploy/inpx-web-reader/.env.template` to an untracked `.env`, set the source, data, and token-file paths, then run:

```sh
docker compose \
  --env-file deploy/inpx-web-reader/.env \
  -f deploy/inpx-web-reader/docker-compose.yml \
  up -d
```

For a remote Linux build host, copy `.env.example` to the ignored repository-root `.env`, set `INPX_WEB_READER_BUILD_*`, then use:

```sh
python3 scripts/BootstrapRemoteLinux.py
python3 scripts/RunRemoteLinux.py test
```

The remote worker uses all detected CPU threads by default for builds and tests.
Remote synchronization includes only Git-indexed files and refuses untracked, non-ignored paths; add new source files to the index before running the remote lane.

## Verification

Linux workers:

```sh
python3 scripts/RunTests.py
python3 scripts/RunSanitizers.py
python3 scripts/RunSanitizers.py --preset linux-tsan
python3 scripts/RunCoverage.py
python3 scripts/RunFuzzers.py
python3 scripts/RunStaticAnalysis.py
python3 scripts/RunLinuxTests.py
```

Host-side script and web checks:

```sh
python3 scripts/RunScriptTests.py
python3 scripts/RunWebUi.py build
python3 scripts/RunWebUi.py test
python3 scripts/RunWebUi.py test:coverage
python3 scripts/RunWebUi.py test:e2e
```

On Linux, the e2e helper runs the matching official Playwright image against a real local server, so browser dependencies do not depend on the host distribution. From macOS or Windows, run the same real-server lane on the configured Linux worker with `python3 scripts/RunRemoteLinux.py e2e`.

`RunRemoteLinux.py test` is the authoritative full Linux/Docker lane. It builds native code and the runtime image, runs CTest, starts Compose with a generated INPX fixture, verifies scan/catalog/download/restart/auth behavior, checks security and read-only mounts, and enforces runtime resource budgets. Use `--smoke-book-count` for a representative deterministic catalog-size probe.

## Repository map

| Path | Responsibility |
|---|---|
| `apps/InpxWebReader/` | Linux executable entry point |
| `libs/` | Native domain, INPX scan, SQLite, server, storage, parser, and converter libraries |
| `web/inpx-web-reader/` | React/Vite browser UI |
| `deploy/inpx-web-reader/` | Dockerfile, Compose template, and entrypoint |
| `scripts/` | Linux build/test/deploy and remote-host orchestration |
| `tests/Unit/` | Catch2 native tests |
| `tests/Fuzz/` | Clang/libFuzzer parser targets |
| `tests/Scripts/` | Python helper tests |
| `docs/` | Product, architecture, style, web design, and deployment references |

Start with [docs/InpxWebReader-Product.md](docs/InpxWebReader-Product.md), [docs/CodebaseMap.md](docs/CodebaseMap.md), and [docs/ServerWebDeployment.md](docs/ServerWebDeployment.md).
