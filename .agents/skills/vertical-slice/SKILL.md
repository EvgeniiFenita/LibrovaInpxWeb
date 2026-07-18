---
name: vertical-slice
description: Implement an end-to-end InpxWebReader feature across native domain/application/database/server contracts and the React UI. Use for new read queries, scan actions, download capabilities, or browser workflows spanning multiple layers.
---

# Vertical slice workflow

1. Define user-visible behavior, failure/cancellation semantics, and acceptance checks within the INPX-only product boundary.
2. Extend the smallest native domain/application contract first.
3. Add schema/query changes only when required, following `$sqlite`.
4. Expose the behavior through server DTO/HTTP mapping with auth, limits, and structured errors.
5. Map typed API data in `web/inpx-web-reader/src/api` and compose UI state without duplicating business rules.
6. Add unit tests at each changed contract and one real server/browser or Docker scenario when cross-layer drift is possible.
7. Update relevant product/codebase/deployment docs and skills.
8. Verify focused layers, then the remote Linux lane when the slice affects runtime or deployment behavior.
