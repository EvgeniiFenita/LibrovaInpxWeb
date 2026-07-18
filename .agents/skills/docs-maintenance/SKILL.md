---
name: docs-maintenance
description: Maintain InpxWebReader README, agent rules, product, architecture, style, web design, deployment docs, and repository skills. Use for documentation updates, drift audits, deduplication, renamed paths/commands, and implemented-state verification.
---

# Documentation maintenance

1. Verify claims against current code, CMake, scripts, Docker, schema, API routes, and tests.
2. Keep ownership layered:
   - `README.md`: public overview and entry commands;
   - `AGENTS.md`: global workflow and constraints;
   - `docs/InpxWebReader-Product.md`: scope and invariants;
   - `docs/CodebaseMap.md`: architecture and navigation;
   - `docs/CodeStyleGuidelines.md`: language conventions;
   - `docs/WebUiDesignSystem.md`: browser UI;
   - `docs/ServerWebDeployment.md`: Linux/Docker operations;
   - skills: concise task procedures.
3. Remove duplicated or stale text instead of preserving historical alternatives.
4. Use only current names, paths, environment variables, schema version, and Linux commands.
5. Delete documentation for removed features and historical implementation plans.
6. Check all referenced files and run `--help` for changed CLI examples when practical.
