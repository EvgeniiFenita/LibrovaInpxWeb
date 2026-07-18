---
name: testing
description: Add, update, select, and run InpxWebReader tests across Catch2/CTest, Python unittest, Vitest, Playwright, and remote Linux/Docker smoke. Use for regression tests, test placement, verification planning, or test infrastructure changes.
---

# Testing workflow

1. Identify the changed production contract and choose the closest layer.
2. Add Catch2 tests in `tests/Unit` for native rules, SQLite, parsers, scan, server, converter, filesystem, and Unicode.
3. Add Python unittest coverage in `tests/Scripts/Test<Area>.py` for helpers and orchestration.
4. Use Vitest for API/client/component contracts and Playwright for routed browser/real-server behavior.
5. Run `RunWebUi.py test:e2e` on Linux; it selects the matching official Playwright image from `package-lock.json` and exercises a real local server.
6. Use `RunRemoteLinux.py test` for the x86_64 Linux image, Compose, mounts, permissions, auth, scanning, downloads, restart, concurrency, and resources.
7. Treat the remote Linux host as a dedicated worker and use all detected CPU threads. Lower parallelism only after an observed resource failure or an explicit request.
8. Keep all generated state under `out/tests` or `out/test-workspaces`; never touch user/system temp or real application data.
9. Prefer behavior assertions through production entry points over source-text matching or call-count-only fakes.
10. Build required targets before CTest and use the narrowest reliable label/regex; broaden only when the contract requires it.
11. For CLI behavior, execute the built binary and assert exit code, stdout, and stderr. For HTTP/UI behavior, use the real server/browser boundary when the route or workflow contract changes.
12. Use exact result, ordering, facet-count, persistence, and cleanup assertions. Cover invalid, empty, minimum, exact-limit, over-limit, overflow, missing-resource, cancellation, and permission cases that are relevant to the contract.
13. Every new or changed CLI, HTTP, browser, Docker/environment, or schema contract requires a matching boundary-level regression test in the same change.
14. Use `RunCoverage.py` and `RunWebUi.py test:coverage` to find weak modules, then inspect missing behavior rather than treating a percentage as proof.
15. Run `RunSanitizers.py --preset linux-tsan` for shared-state changes and `RunFuzzers.py` for parser/archive input changes. Keep seeds, corpora, and crash artifacts reproducible under `out/`.
16. Record the commands actually run and report an unavailable required Linux/Docker lane as a blocker; do not substitute unrelated local checks.
