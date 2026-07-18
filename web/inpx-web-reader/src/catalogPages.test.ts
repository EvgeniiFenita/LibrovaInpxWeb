import { describe, expect, it } from 'vitest';

import type { BookListResponse } from './api/types';
import { CatalogPageStore, findBookInPageChunks } from './catalogPages';
import { mockBook } from './test/mockData';

function makePage(offset: number, snapshotId = 'large-catalog'): BookListResponse {
  return {
    items: [{ ...mockBook, id: offset + 1 }],
    catalogSnapshotId: snapshotId,
    totalCount: offset === 0 ? 16_384 : null,
    offset,
    limit: 1,
    nextCursor: offset + 1 < 16_384 ? `cursor-${offset + 1}` : null,
    facets: offset === 0 ? { languages: [], genres: [] } : null
  };
}

describe('catalog page chunks', () => {
  it('keeps append work linear and large-catalog lookup logarithmic', () => {
    const pageCount = 16_384;
    let appendOperations = 0;
    const store = new CatalogPageStore(() => {
      appendOperations += 1;
    });
    const stablePages = store.pages;

    store.replaceFirstPage(makePage(0));
    for (let pageIndex = 1; pageIndex < pageCount; ++pageIndex) {
      expect(store.appendContinuation(makePage(pageIndex))).toBe(true);
    }

    expect(store.pages).toBe(stablePages);
    expect(store.pages).toHaveLength(pageCount);
    expect(appendOperations).toBe(pageCount);

    let pageProbes = 0;
    const lastBook = findBookInPageChunks(store.pages, pageCount - 1, () => {
      pageProbes += 1;
    });
    expect(lastBook?.id).toBe(pageCount);
    expect(pageProbes).toBeLessThanOrEqual(Math.ceil(Math.log2(pageCount)) + 1);
    expect(findBookInPageChunks(store.pages, -1)).toBeUndefined();
    expect(findBookInPageChunks(store.pages, pageCount)).toBeUndefined();
  });

  it('refuses to mix catalog snapshot generations', () => {
    const store = new CatalogPageStore();
    store.replaceFirstPage(makePage(0, 'snapshot-a'));

    expect(store.appendContinuation(makePage(1, 'snapshot-b'))).toBe(false);
    expect(store.pages).toHaveLength(1);
    expect(store.pages[0].catalogSnapshotId).toBe('snapshot-a');
  });

  it('rejects overlapping and gapped continuation offsets', () => {
    const emptyStore = new CatalogPageStore();
    expect(emptyStore.appendContinuation(makePage(0))).toBe(false);

    const store = new CatalogPageStore();
    store.replaceFirstPage(makePage(0));

    expect(store.appendContinuation(makePage(0))).toBe(false);
    expect(store.appendContinuation(makePage(2))).toBe(false);
    expect(store.pages).toHaveLength(1);
  });
});
