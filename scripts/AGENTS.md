# Script rules

- `RunRemoteLinux.py` and `BootstrapRemoteLinux.py` are development-host orchestrators.
- `RunTests.py`, `RunSanitizers.py`, `RunCoverage.py`, `RunFuzzers.py`, `RunStaticAnalysis.py`, `RunLinuxTests.py`, `RunRelease.py`, and `PrepareDeployBundle.py` are Linux workers.
- On macOS or Windows, do not invoke `RunStaticAnalysis.py` locally just to confirm its platform rejection; run it through the Linux worker and report an unavailable remote lane as the blocker.
- Keep all generated files, caches, test data, askpass files, and reports under repository `out/`.
- Validate output/removal paths against their allowed roots.
- Never print or synchronize `.env`, passwords, tokens, or token contents.
- Keep SSH/rsync job locking and reconnect behavior intact.
- Build the remote source snapshot from Git-indexed files only; reject untracked, non-ignored paths and keep the local staging snapshot under `out/`.
- Keep remote orchestration runnable from Windows and macOS development hosts; SSH discovery and askpass helpers must use host-correct executable names and script formats.
- Use only `INPX_WEB_READER_` environment names and Linux CMake presets.
- Add or update `tests/Scripts/Test<Area>.py` for helper behavior changes.
- Keep coverage reports, fuzz corpora/crash artifacts, and sanitizer state under dedicated `out/` children. Fuzz runs must have an explicit seed and bounded runtime so failures are reproducible.
- Run the focused script test first, then the full script suite when shared helpers change, and run `python3 scripts/RunStaticAnalysis.py --skip-clang-tidy` for ruff on Linux.
