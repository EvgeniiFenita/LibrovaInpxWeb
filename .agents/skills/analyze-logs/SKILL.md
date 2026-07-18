---
name: analyze-logs
description: Analyze InpxWebReader scan and runtime logs for INPX/FB2 parsing, genre normalization, encoding, archive access, scan performance, auth, converter, and Docker failures. Use when logs from a scan or server run are provided.
---

# Analyze InpxWebReader logs

1. Identify product version, scan job id/mode, source paths, and first causal error. Do not expose tokens or credentials.
2. Group repeated diagnostics by code/message, archive, entry, encoding, and stage. Report counts instead of reproducing noisy logs.
3. Separate source-data problems from application defects:
   - malformed/unsupported INPX or FB2;
   - missing ZIP/archive entry or source outage;
   - CP1251/CP866/UTF decoding fallback;
   - genre mapping gaps;
   - SQLite/schema/cache damage;
   - converter failure/timeout/cancellation;
   - auth/config/mount/permission/resource failures.
4. Correlate `[scan-perf]` summaries with scan progress and resource-limit messages before claiming a bottleneck.
5. Recommend the smallest actionable next step: source correction, cache deletion/rescan, config/mount fix, or code/test change.
6. For suspected defects, name the owning module and the exact regression test layer from `AGENTS.md`.

Never suggest database migration. An incompatible cache is deleted and recreated from the INPX source.
