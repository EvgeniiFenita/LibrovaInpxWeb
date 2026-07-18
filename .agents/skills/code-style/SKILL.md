---
name: code-style
description: Apply InpxWebReader conventions when writing or reviewing C++20, TypeScript/React, Python, shell, CMake, or configuration code. Use for implementation and style-sensitive refactoring.
---

# Code style workflow

1. Read `docs/CodeStyleGuidelines.md` and nearby code.
2. Reuse existing Unicode, filesystem, SQLite, logging, path-safety, API mapping, and script helpers.
3. Keep native code in namespace `InpxWebReader`; follow existing C/S/E/I type prefixes and ownership conventions.
4. Keep server/domain rules out of React and transport mapping.
5. Keep Python and shell paths/outputs under `out/`; validate destructive paths and protect credentials.
6. Do not add platform branches: production C++, build files, and worker scripts target x86_64 Linux only.
7. Format consistently with the edited file and add comments only for non-obvious invariants.
8. Run the language-appropriate focused tests and analysis from `AGENTS.md`.
