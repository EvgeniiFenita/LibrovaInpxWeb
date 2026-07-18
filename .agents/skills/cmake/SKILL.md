---
name: cmake
description: Modify InpxWebReader CMake targets, Linux presets, vcpkg integration, test registration, or web-asset wiring. Use for CMakeLists.txt, CMakePresets.json, target names, dependency wiring, and build graph changes.
---

# CMake workflow

1. Inspect the owning `CMakeLists.txt`, root target graph, and relevant preset before editing.
2. Preserve x86_64 Linux-only constraints and repository-root `out/` build/install directories.
3. Use the fixed names:
   - project/namespace/executable target: `InpxWebReader`;
   - libraries/tests/web targets: `InpxWebReader*`;
   - output binary: `inpx-web-reader`;
   - build option prefix: `INPX_WEB_READER_`.
4. Keep one static library per native slice and declare public/private include/link ownership explicitly.
5. Update source lists, target dependencies, Docker build targets, scripts, and tests together when a target changes.
6. Configure fresh on remote Linux, build before CTest, and select the narrowest reliable labels. Use `RunRemoteLinux.py test` for repository-wide or Docker-facing graph changes.
7. Preserve `RunRemoteLinux.py` support on both Windows and macOS development hosts; keep host-specific SSH/askpass handling in the remote transport while all build execution remains on Linux.

Keep the graph x86_64 Linux-only and single-config; do not add alternate runtime targets or optional product-mode switches.
