# InpxWebReader — Code Style

## C++

- Use C++20 and namespace `InpxWebReader`.
- Prefer RAII, value types, `std::unique_ptr`, `std::optional`, `std::span`, `std::string_view`, and explicit ownership.
- Use `PascalCase` for types/methods, `camelCase` for local variables/parameters, and `m_` for members.
- Prefix classes with `C`, structs with `S`, enums with `E`, and interfaces with `I` where consistent with existing code.
- Keep headers and implementations together in their owning `libs/<Slice>` directory.
- Route UTF-8/path conversion through `Foundation/UnicodeConversion`; never assume source data is ASCII.
- Bind SQL values through statements; do not concatenate user input into SQL.
- Log actionable boundary outcomes without tokens, passwords, or full sensitive configuration. Filesystem paths, archive-entry names, and bounded FB2/XML diagnostic previews are intentional parser-research context and remain available at their normal info/warning/error levels; do not move them behind a separate diagnostic mode. Treat the resulting log files as sensitive support data and bound storage with rotation.
- Write comments only for non-obvious invariants, ownership, protocol, or recovery behavior.

x86_64 Linux is the only native target. Keep platform-specific implementation and build branches out of production code.

## TypeScript and React

- Use strict TypeScript and functional React components.
- Keep API DTO mapping in `src/api`; keep server/domain rules out of components.
- Treat query state as explicit typed data and preserve accessible names/focus behavior.
- Reuse components/hooks and CSS tokens before adding one-off variants.
- Keep technical storage keys under the `inpx-web-reader.` prefix.
- Use `InpxWebReader` exactly in user-visible product copy.

## Python

- Use `snake_case`, type annotations, `pathlib`, explicit subprocess argument lists, and `sys.executable` for child Python.
- Keep generated state under `out/`; validate any removal/output path against its allowed root.
- Never put credentials in command lines, tracked files, or diagnostic output.
- Linux worker scripts must fail clearly on other hosts. Remote orchestration stays platform-neutral and delegates product work to Linux.

## Shell, CMake, and configuration

- Shell scripts use POSIX `sh` unless a Linux-only Bash feature is required and declared.
- Quote paths and fail closed with `set -eu`.
- Use technical names exactly: `InpxWebReader`, `inpx-web-reader`, `INPX_WEB_READER_`.
- Keep CMake output under `out/` and use only the Linux presets.
- Keep Docker runtime read-only except declared `/data` and temporary mounts.

## Tests

- Test behavior and public contracts, not source-text snippets when a runtime/parser assertion is possible.
- Keep workspaces under `out/tests` or `out/test-workspaces`.
- Add regression coverage for Unicode/legacy encodings, ZIP paths, SQLite effects, cancellation, source read-only behavior, auth, and API mapping.
- Preserve `build -> test` ordering.
