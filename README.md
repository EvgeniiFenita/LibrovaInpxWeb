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

Copy `deploy/inpx-web-reader/.env.template` to an untracked `.env`, set the source, data, and access-password-file paths, then run:

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

## Recommended Linux host release

The Linux host release flow keeps machine-specific values and the access password on
the development host, while all build and Docker work stays on the x86_64 Linux
worker. Create the ignored local configuration once:

```sh
cp .env.deploy.example .env.deploy
chmod 600 .env.deploy
```

Edit `.env.deploy` and set at least:

```text
INPX_WEB_READER_DEPLOY_HOST_SOURCE_ROOT=/srv/books/inpx
INPX_WEB_READER_DEPLOY_HOST_APP_ROOT=/srv/inpx-web-reader
INPX_WEB_READER_DEPLOY_ACCESS_PASSWORD=abcd-efgh-jkmn-pqrs
```

The custom password must contain 12–256 printable ASCII characters without
spaces. If it is left empty, the bundle generator creates a short high-entropy
password in four readable groups and reuses it while the previous remote bundle
exists. The downloaded value is available locally in
`out/deploy/inpx-web-reader/secrets/inpx-web-reader-auth-token.txt`. Setting it
explicitly in the ignored `.env.deploy` is recommended when the password must
remain stable even if remote build output is cleaned. The target Linux host
source and application roots must be absolute, non-root, and non-overlapping paths.

Build, verify, and download one release bundle:

```sh
python3 scripts/RunRemoteLinux.py release
```

`release` runs script tests and Python static analysis, builds and tests the web
UI, runs the real-server browser workflow, builds the native server, and runs
the Docker/Compose smoke lane. The exact Docker image that passed those checks
is saved into `out/deploy/inpx-web-reader`; it is not rebuilt for packaging.
The image tag contains the product version and Git commit (plus a dirty-tree
hash when applicable), and the bundle contains a manifest and image checksum.

Deployment to the target Linux host remains intentionally manual:

1. Copy the contents of `out/deploy/inpx-web-reader` to the configured host
   application directory. Overwrite release files, but do not mirror with a
   delete option and do not delete the host's `data/` directory.
2. On the target host, run:

   ```sh
   cd /srv/inpx-web-reader
   sh RUN_ON_HOST.sh
   ```

The generated host script verifies the archive checksum, loads the versioned
image, starts Compose, and waits for health. A failed update restores the
previous image when one is available. After a healthy update, it removes
stopped containers from the exact `inpx-web-reader` Compose project and unused
superseded InpxWebReader images; it never performs a global Docker prune. The
credential stored by the current browser can later be replaced or forgotten under Settings
→ Server access; this does not change the password configured on the server.

`RunRemoteLinux.py bundle` remains available as an advanced unverified
packaging command. Use `release` for normal Linux host updates.

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
