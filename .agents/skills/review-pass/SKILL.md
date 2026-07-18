---
name: review-pass
description: Review and harden InpxWebReader changes, commits, or branches. Use for a final project-specific review of native, web, schema, scripts, Docker, tests, documentation, and naming consistency.
---

# Review pass

1. Establish review scope and ignore unrelated pre-existing changes.
2. Check product boundary: INPX web only, x86_64 Linux only, read-only source, one active source, schema version 1, and no unrelated library-management features.
3. Review correctness by boundary: Unicode/paths, SQLite transactions/schema, scan cancellation/reconciliation, archive access, converter process lifetime, HTTP auth/limits, React DTO mapping, Docker permissions/mounts.
4. Search for stale technical names, environment prefixes, target paths, removed feature vocabulary, and non-Linux branches.
5. Confirm regression tests exercise production behavior and all workspaces stay under `out/`.
6. Map changed files to the verification matrix in `AGENTS.md`; preserve build before test.
7. Report only actionable findings with file/line evidence and risk. Fix only when the user requested implementation.
