# InpxWebReader — Web UI Design System

## Scope

This document applies to the responsive React UI in `web/inpx-web-reader`.

## Principles

- Optimize for browsing a large personal catalog on wide, tablet-sized, and mobile browser viewports.
- Keep search, filters, scan state, availability, and download actions explicit.
- Preserve the existing warm editorial visual language and use `InpxWebReader` exactly in visible product copy.
- Prefer accessible semantic controls, keyboard operation, visible focus, and readable status/error text.
- Avoid encoding domain rules in components; obtain capabilities and state from typed API contracts.

## Tokens and components

Use the CSS custom properties already defined in `src/styles.css` for color, spacing, radius, typography, borders, and state surfaces. Reuse existing catalog cards, cover placeholders, drawers/dialogs, search toolbar, scan dialog, toast region, and download actions before adding new patterns.

## Responsive behavior

- Wide layouts may use persistent catalog controls and side/drawer surfaces.
- Narrow layouts use bottom sheets or modal surfaces with focus containment and restoration.
- Modal content remains vertically scrollable in short landscape viewports and under browser text zoom; opening and closing restores focus without leaving hidden interactive content mounted.
- Avoid fixed geometry that clips translated metadata, Cyrillic titles, long authors, or browser text scaling.
- Preserve touch target size and scroll ownership; nested scroll regions require a clear reason.

## States

Every async surface must represent loading, empty, error, unavailable-source, scan-progress, cancellation, and success where applicable. Disable actions according to server capabilities, not duplicated client-side business rules.

The startup scan dialog exposes cancellation while a scan is active and keeps the cancelling state disabled until the server reaches a terminal state. Its semantic live progress summary distinguishes parsed records from records reused through unchanged INP segments, and the final terminal outcome remains visible until the user dismisses it.

Catalog pages belong to the server-provided `catalogSnapshotId`. Infinite scrolling follows the server's opaque `nextCursor`; total count and facets come from the first page and remain visible while continuation pages omit that repeated summary work. Loaded pages stay as immutable chunks in an append-only store: adding a page does not copy or regroup earlier pages, visible books use logarithmic page-offset lookup, and the fixed-height catalog computes its visible row range directly from the scroll viewport. Its physical scroll surface is capped at 8,000,000 CSS pixels and rebased in bounded row windows, so catalogs beyond browser element-height limits remain reachable without rendering or copying the full result set. A transient first-page or continuation failure exposes an explicit retry; continuation retry preserves earlier immutable pages and never loops automatically. Automatic focus refresh does not collapse a loaded cursor session; a source/capability generation change, explicit catalog invalidation, or recoverable expired/stale cursor resets the query to a new first page before appending rows, so generations are never mixed. Authentication settings keep an access-password draft separate from the submitted bearer credential so closing a dialog cannot silently change active credentials. The settings dialog may reveal, replace, or forget the password stored in the current browser; it never changes the server-side secret.

## Verification

For web changes run:

```sh
python3 scripts/RunWebUi.py build
python3 scripts/RunWebUi.py test
python3 scripts/RunWebUi.py test:coverage
```

Add `python3 scripts/RunWebUi.py test:e2e` for routing, modal/focus behavior, responsive interaction, API mapping, downloads, auth, static assets, or real server workflows. Docker-served UI changes also require the remote Linux lane.
