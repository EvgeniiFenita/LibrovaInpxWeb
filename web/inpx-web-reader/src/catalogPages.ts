import type { BookListItem, BookListResponse } from './api/types';

export interface CatalogPageChunk {
  offset: number;
  items: readonly BookListItem[];
}

type PageAppendObserver = (pageCount: number) => void;
type PageProbeObserver = (pageIndex: number) => void;

export class CatalogPageStore {
  private readonly pageChunks: BookListResponse[] = [];

  constructor(private readonly onPageAppended?: PageAppendObserver) {}

  get pages(): readonly BookListResponse[] {
    return this.pageChunks;
  }

  replaceFirstPage(page?: BookListResponse) {
    this.pageChunks.length = 0;
    if (page) {
      this.appendUnchecked(page);
    }
  }

  appendContinuation(page: BookListResponse) {
    const firstPage = this.pageChunks[0];
    const previousPage = this.pageChunks[this.pageChunks.length - 1];
    const expectedOffset = previousPage
      ? previousPage.offset + previousPage.items.length
      : 0;
    if (!firstPage
      || page.catalogSnapshotId !== firstPage.catalogSnapshotId
      || page.offset !== expectedOffset) {
      return false;
    }

    this.appendUnchecked(page);
    return true;
  }

  private appendUnchecked(page: BookListResponse) {
    this.pageChunks.push(page);
    this.onPageAppended?.(this.pageChunks.length);
  }
}

export function findBookInPageChunks(
  pageChunks: readonly CatalogPageChunk[],
  index: number,
  onPageProbe?: PageProbeObserver
) {
  let first = 0;
  let last = pageChunks.length - 1;
  while (first <= last) {
    const middle = first + Math.floor((last - first) / 2);
    onPageProbe?.(middle);
    const page = pageChunks[middle];
    if (index < page.offset) {
      last = middle - 1;
      continue;
    }

    const relativeIndex = index - page.offset;
    if (relativeIndex >= page.items.length) {
      first = middle + 1;
      continue;
    }
    return page.items[relativeIndex];
  }
  return undefined;
}
