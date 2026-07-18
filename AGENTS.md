# InpxWebReader — Agent Instructions

## Product boundary

InpxWebReader is one x86_64 Linux C++20 server plus a browser UI. It supports one read-only INPX catalog, a SQLite/FTS metadata cache, generated covers, original FB2 downloads, and optional Linux FB2-to-EPUB conversion.

Keep the product limited to the Linux server, browser UI, disposable INPX cache, downloads, and optional conversion described here. Do not add additional runtime surfaces, mutable book storage, personal-library state, cloud services, or multiple active sources.

Technical names are fixed:

- product: `InpxWebReader`
- CMake target and C++ namespace: `InpxWebReader`
- executable/image: `inpx-web-reader`
- environment prefix: `INPX_WEB_READER_`

## Read order

Read only the relevant sections, in this order:

1. `docs/InpxWebReader-Product.md`
2. `docs/CodebaseMap.md`
3. `docs/CodeStyleGuidelines.md` for code changes
4. `docs/WebUiDesignSystem.md` for browser UI changes
5. `docs/ServerWebDeployment.md` for Docker or remote Linux changes

Use repository skills when their descriptions match the task.
The named reference documents own their topics; `README.md` and skills are routing summaries. If implementation, tests, and the owning document disagree, resolve or report the conflict instead of silently choosing a different source.

## Architecture constraints

- x86_64 Linux is the only runtime and native build platform; Docker artifacts use `linux/amd64`.
- CMake and vcpkg are the canonical native toolchain.
- Keep native libraries under `libs/<Slice>/` with local CMake targets.
- Keep domain and application rules out of React components and HTTP mapping.
- Treat the INPX source as read-only.
- Keep all writable cache/runtime state under the configured data roots.
- Preserve Unicode, Cyrillic, CP1251/CP866, filesystem, ZIP-entry, and SQLite correctness.
- Avoid quadratic or worse algorithms on catalog-, book-, archive-, scan-, and search-sized inputs. Prefer `O(n)` or `O(n log n)` work; `O(n^2)` is allowed only for a collection with a documented hard small bound, and scale-sensitive changes must be checked with representative input volume.
- Conversion cancellation is distinct from converter failure.
- The product is pre-release: change the current schema version 1 freely when the implementation requires it, updating DDL, validation, queries, and tests together. Do not preserve compatibility with earlier schema-v1 shapes and do not add migrations; incompatible caches are deleted and recreated.
- Keep generated files, caches, test workspaces, logs, and reports under repository `out/`.

## Verification

Select the smallest complete set, preserving build before test:

| Change | Required checks |
|---|---|
| Native C++ | Build `InpxWebReaderCoreTests`, then matching Catch2/CTest labels on Linux |
| Schema or SQL | Native `database-schema|sqlite|inpx` coverage |
| Server/API | Native `server|http` coverage; add web e2e for contract changes |
| Web TypeScript/React/CSS | `RunWebUi.py build` and `RunWebUi.py test`; add `test:e2e` for routing/API/server workflows |
| Python scripts | Matching `RunScriptTests.py --pattern ...` tests and `RunStaticAnalysis.py --skip-clang-tidy` for ruff on Linux |
| Docker/deployment | `RunRemoteLinux.py test` |
| Memory/ownership boundaries | Focused `RunSanitizers.py` lane on Linux |
| Shared-state/concurrency changes | Focused `RunSanitizers.py --preset linux-tsan` lane on Linux |
| Parser/archive input handling | Deterministic fixtures plus `RunFuzzers.py` on Linux |
| Coverage-sensitive work | `RunCoverage.py` and/or `RunWebUi.py test:coverage`; inspect the report, not only the percentage |
| Scale/resource behavior | `RunRemoteLinux.py test --smoke-book-count <representative-count>` |
| Documentation only | Check referenced paths and commands; no unrelated code suite |

`scripts/RunStaticAnalysis.py` is Linux-only. On macOS or Windows, do not invoke it locally just to observe its platform rejection; run the required static-analysis lane on the Linux worker (or report that exact remote blocker) instead. Local `ruff` may be used as an additional fast check, but it does not replace the required Linux script.

Any new or changed external contract (CLI option, HTTP route/query/body/status, browser workflow, Docker/environment option, or persisted schema rule) must add or update a test at the real contract boundary. A production contract change without its corresponding boundary test is incomplete and must not be merged.

The authoritative Linux/Docker integration checkpoint is:

```sh
python3 scripts/RunRemoteLinux.py test
```

This command does not replace the change-specific checks above. CI runs the baseline native, script, web/e2e, and Docker lanes; coverage, sanitizers, fuzzing, and representative scale checks remain conditional or scheduled lanes.

The remote Linux host is a dedicated build/test worker. Use all detected CPU threads by default; do not apply a conservative parallelism cap. Reduce build, test, analyzer, sanitizer, Docker, or browser worker counts only after an observed resource failure or when the user explicitly requests it.

Do not silently replace an unavailable required remote/Docker lane with unrelated local checks. Report the exact blocker.

## Repository discipline

- Inspect actual Git state before editing and preserve unrelated user changes.
- Reuse existing helpers before adding new abstractions.
- Use `rg` for search and `apply_patch` for ordinary edits.
- Never write credentials to tracked files or logs.
- Remote synchronization transfers only Git-indexed files and rejects untracked, non-ignored paths; add new source files to the index before using the remote lane.
- Keep tests isolated under `out/tests` or `out/test-workspaces`.
- Create commits only after reviewing the staged diff and verification evidence.
- Keep `README.md`, reference docs, skills, scripts, and code names synchronized.
