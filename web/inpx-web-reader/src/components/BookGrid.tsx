import { BookOpen, CloudOff } from 'lucide-react';
import type { RefObject } from 'react';
import { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react';

import type { BookListItem } from '../api/types';
import emptyCatalogUrl from '../assets/empty_catalog.png';
import noResultsUrl from '../assets/no_results.png';
import { findBookInPageChunks, type CatalogPageChunk } from '../catalogPages';
import { formatBytes, joinAuthors } from '../format';
import { mobileCatalogMediaQuery, useMediaQuery } from '../hooks/useMediaQuery';
import { CoverImage } from './CoverImage';
import { CoverPlaceholder } from './CoverPlaceholder';

interface VisibleRowView {
  key: number;
  index: number;
  start: number;
}

interface ScrollViewport {
  scrollTop: number;
  height: number;
}

interface VirtualRowWindow {
  first: number;
  last: number;
  capacity: number;
}

interface BookGridProps {
  bookPages: readonly CatalogPageChunk[];
  totalCount: number;
  loading: boolean;
  loadingMore: boolean;
  hasMore: boolean;
  hasActiveQuery: boolean;
  hasError?: boolean;
  token?: string;
  selectedId: number | null;
  onSelectBook: (book: BookListItem) => void;
  onLoadMore: () => void;
}

const cardCellWidth = 184;
const cardGap = 18;
const rowHeight = 398;
const mobileRowHeight = 116;
const loadMoreThresholdPx = 520;
const fallbackVirtualRowCount = 12;
const overscanRowCount = 3;
export const maxVirtualScrollHeightPx = 8_000_000;

export function resolveVirtualRowWindow(
  rowCount: number,
  rowSize: number,
  requestedFirst: number
): VirtualRowWindow {
  if (rowCount <= 0 || rowSize <= 0) {
    return { first: 0, last: 0, capacity: 1 };
  }

  const capacity = Math.max(1, Math.floor(maxVirtualScrollHeightPx / rowSize));
  const maximumFirst = Math.max(0, rowCount - capacity);
  const first = Math.min(maximumFirst, Math.max(0, Math.floor(requestedFirst)));
  return {
    first,
    last: Math.min(rowCount, first + capacity),
    capacity
  };
}

export function resolveVisibleRowRange(
  rowCount: number,
  rowSize: number,
  viewport: ScrollViewport
) {
  if (rowCount <= 0 || rowSize <= 0) {
    return { first: 0, last: 0 };
  }

  if (viewport.height <= 0) {
    return { first: 0, last: Math.min(rowCount, fallbackVirtualRowCount) };
  }

  const scrollTop = Math.max(0, viewport.scrollTop);
  const firstVisible = Math.min(rowCount - 1, Math.floor(scrollTop / rowSize));
  const lastVisible = Math.max(firstVisible + 1, Math.ceil((scrollTop + viewport.height) / rowSize));
  return {
    first: Math.max(0, firstVisible - overscanRowCount),
    last: Math.min(rowCount, lastVisible + overscanRowCount)
  };
}

function useScrollViewport(container: RefObject<HTMLElement>, active: boolean) {
  const [viewport, setViewport] = useState<ScrollViewport>({ scrollTop: 0, height: 0 });

  const syncViewport = useCallback(() => {
    const element = container.current;
    if (!element) {
      return;
    }

    const nextViewport = {
      scrollTop: element.scrollTop,
      height: element.clientHeight
    };
    setViewport((current) => (
      current.scrollTop === nextViewport.scrollTop && current.height === nextViewport.height
        ? current
        : nextViewport
    ));
  }, [container]);

  useLayoutEffect(() => {
    if (!active) {
      return undefined;
    }

    const element = container.current;
    if (!element) {
      return undefined;
    }

    syncViewport();
    const observer = new ResizeObserver(syncViewport);
    observer.observe(element);
    element.addEventListener('scroll', syncViewport, { passive: true });
    return () => {
      observer.disconnect();
      element.removeEventListener('scroll', syncViewport);
    };
  }, [active, container, syncViewport]);

  return { syncViewport, viewport };
}

function useResponsiveColumns(container: RefObject<HTMLElement>, active: boolean) {
  const [columns, setColumns] = useState(1);

  useLayoutEffect(() => {
    if (!active) {
      return undefined;
    }

    const element = container.current;
    if (!element) {
      return undefined;
    }

    const update = () => {
      const bounds = element.getBoundingClientRect();
      const width = Math.max(0, bounds.width - 44);
      setColumns(Math.max(1, Math.floor((width + cardGap) / (cardCellWidth + cardGap))));
    };

    update();
    const animationFrame = window.requestAnimationFrame(update);
    const observer = new ResizeObserver(update);
    observer.observe(element);
    window.addEventListener('resize', update);
    return () => {
      window.cancelAnimationFrame(animationFrame);
      observer.disconnect();
      window.removeEventListener('resize', update);
    };
  }, [active, container]);

  return columns;
}

function Cover({ book, token }: { book: BookListItem; token?: string }) {
  const authors = joinAuthors(book.authors);
  return (
    <CoverImage
      coverUrl={book.coverUrl}
      token={token}
      fallback={<CoverPlaceholder title={book.title} authors={authors} />}
    />
  );
}

function BookCard({
  book,
  selected,
  token,
  onSelect
}: {
  book: BookListItem;
  selected: boolean;
  token?: string;
  onSelect: (book: BookListItem) => void;
}) {
  const showFormatBadge = book.format !== 'fb2';

  return (
    <article className={selected ? 'book-card selected' : 'book-card'}>
      <button type="button" className="book-card-main" onClick={() => onSelect(book)}>
        <div className="cover-frame">
          <Cover book={book} token={token} />
          {showFormatBadge && <span className="cover-format">{book.format.toUpperCase()}</span>}
          {!book.isAvailable && (
            <span className="cover-badge">
              <CloudOff aria-hidden="true" size={13} />
              Unavailable
            </span>
          )}
        </div>
        <div className="book-card-text">
          <h3>{book.title}</h3>
          <p>{joinAuthors(book.authors)}</p>
          <div className="book-card-meta" aria-label="Book metadata">
            {book.year && <span>{book.year}</span>}
            {book.language && <span>{book.language}</span>}
            <span>{formatBytes(book.sizeBytes)}</span>
          </div>
        </div>
      </button>
    </article>
  );
}

function bookMetaLine(book: BookListItem) {
  return [
    book.format.toUpperCase(),
    book.year ? book.year.toString() : null,
    book.language || null,
    formatBytes(book.sizeBytes)
  ].filter(Boolean).join(' / ');
}

function BookListRow({
  book,
  selected,
  onSelect
}: {
  book: BookListItem;
  selected: boolean;
  onSelect: (book: BookListItem) => void;
}) {
  return (
    <article className={selected ? 'mobile-book-row selected' : 'mobile-book-row'}>
      <button type="button" className="mobile-book-main" onClick={() => onSelect(book)}>
        <div className="mobile-book-spine" aria-hidden="true">
          <BookOpen size={18} />
        </div>
        <div className="mobile-book-copy">
          <h3>{book.title}</h3>
          <p>{joinAuthors(book.authors)}</p>
          <span>{bookMetaLine(book)}</span>
        </div>
        {!book.isAvailable && (
          <span className="mobile-unavailable-badge">
            <CloudOff aria-hidden="true" size={13} />
            Unavailable
          </span>
        )}
      </button>
    </article>
  );
}

export function BookGrid({
  bookPages,
  totalCount,
  loading,
  loadingMore,
  hasMore,
  hasActiveQuery,
  hasError = false,
  token,
  selectedId,
  onSelectBook,
  onLoadMore
}: BookGridProps) {
  const parentRef = useRef<HTMLDivElement>(null);
  const loadMoreArmedRef = useRef(true);
  const rebasedScrollTopRef = useRef<number | null>(null);
  const previousBookPagesRef = useRef(bookPages);
  const previousFirstPageRef = useRef(bookPages[0]);
  const previousLoadedBookCountRef = useRef(0);
  const previousLayoutRef = useRef({ columns: 1, isMobileCatalog: false });
  const [windowFirstRow, setWindowFirstRow] = useState(0);
  const isMobileCatalog = useMediaQuery(mobileCatalogMediaQuery);
  const columns = useResponsiveColumns(parentRef, totalCount > 0 && !isMobileCatalog);
  const lastPage = bookPages[bookPages.length - 1];
  const loadedBookCount = lastPage ? lastPage.offset + lastPage.items.length : 0;
  const rowCount = Math.ceil(loadedBookCount / (isMobileCatalog ? 1 : columns));
  const booksForRow = (rowIndex: number) => {
    const firstBookIndex = rowIndex * (isMobileCatalog ? 1 : columns);
    const result: BookListItem[] = [];
    for (let column = 0; column < (isMobileCatalog ? 1 : columns); ++column) {
      const book = findBookInPageChunks(bookPages, firstBookIndex + column);
      if (book) {
        result.push(book);
      }
    }
    return result;
  };

  const activeRowHeight = isMobileCatalog ? mobileRowHeight : rowHeight;
  const { syncViewport, viewport } = useScrollViewport(parentRef, rowCount > 0);
  const rowWindow = resolveVirtualRowWindow(rowCount, activeRowHeight, windowFirstRow);
  const windowCapacity = rowWindow.capacity;
  const windowFirst = rowWindow.first;
  const windowLast = rowWindow.last;

  const resetVirtualWindow = useCallback(() => {
    loadMoreArmedRef.current = true;
    rebasedScrollTopRef.current = null;
    setWindowFirstRow(0);
    const element = parentRef.current;
    if (element) {
      element.scrollTop = 0;
      syncViewport();
    }
  }, [syncViewport]);

  useLayoutEffect(() => {
    const firstPage = bookPages[0];
    const layoutChanged = previousLayoutRef.current.columns !== columns
      || previousLayoutRef.current.isMobileCatalog !== isMobileCatalog;
    const catalogChanged = previousBookPagesRef.current !== bookPages
      || previousFirstPageRef.current !== firstPage
      || loadedBookCount < previousLoadedBookCountRef.current;

    if (layoutChanged || catalogChanged) {
      resetVirtualWindow();
    }

    previousBookPagesRef.current = bookPages;
    previousFirstPageRef.current = firstPage;
    previousLoadedBookCountRef.current = loadedBookCount;
    previousLayoutRef.current = { columns, isMobileCatalog };
  }, [bookPages, columns, isMobileCatalog, loadedBookCount, resetVirtualWindow]);

  const rebaseVirtualWindow = useCallback((firstRow: number, scrollTop: number) => {
    const element = parentRef.current;
    if (!element || element.clientHeight <= 0 || element.scrollHeight <= 0) {
      return;
    }

    loadMoreArmedRef.current = false;
    rebasedScrollTopRef.current = scrollTop;
    element.scrollTop = scrollTop;
    setWindowFirstRow(firstRow);
    syncViewport();
  }, [syncViewport]);

  const requestMoreIfNeeded = useCallback(() => {
    const element = parentRef.current;
    if (!element) {
      return;
    }

    if (element.clientHeight <= 0 || element.scrollHeight <= 0) {
      return;
    }

    const rebasedScrollTop = rebasedScrollTopRef.current;
    if (!loadMoreArmedRef.current && rebasedScrollTop !== null) {
      if (Math.abs(element.scrollTop - rebasedScrollTop) <= 1) {
        return;
      }
      loadMoreArmedRef.current = true;
      rebasedScrollTopRef.current = null;
    }

    if (element.scrollTop <= loadMoreThresholdPx && windowFirst > 0) {
      const shift = Math.min(
        windowFirst,
        Math.max(1, Math.floor(windowCapacity / 2))
      );
      rebaseVirtualWindow(
        windowFirst - shift,
        element.scrollTop + shift * activeRowHeight
      );
      return;
    }

    const remaining = element.scrollHeight - element.scrollTop - element.clientHeight;
    if (remaining > loadMoreThresholdPx) {
      return;
    }

    if (windowLast < rowCount) {
      const shift = Math.min(
        rowCount - windowLast,
        Math.max(1, Math.floor(windowCapacity / 2))
      );
      rebaseVirtualWindow(
        windowFirst + shift,
        Math.max(0, element.scrollTop - shift * activeRowHeight)
      );
      return;
    }

    if (hasMore && !loading && !loadingMore) {
      onLoadMore();
    }
  }, [
    activeRowHeight,
    hasMore,
    loading,
    loadingMore,
    onLoadMore,
    rebaseVirtualWindow,
    rowCount,
    windowCapacity,
    windowFirst,
    windowLast
  ]);

  useEffect(() => {
    requestMoreIfNeeded();
  }, [
    columns,
    loadedBookCount,
    requestMoreIfNeeded,
    viewport.height,
    viewport.scrollTop
  ]);

  const windowRowCount = rowWindow.last - rowWindow.first;
  const visibleRange = resolveVisibleRowRange(windowRowCount, activeRowHeight, viewport);
  const visibleRows: VisibleRowView[] = Array.from(
    { length: visibleRange.last - visibleRange.first },
    (_, offset) => {
      const localIndex = visibleRange.first + offset;
      const index = rowWindow.first + localIndex;
      return { key: index, index, start: localIndex * activeRowHeight };
    }
  );
  const atLoadedEnd = rowWindow.last === rowCount;
  const showLoadMoreFooter = atLoadedEnd && (hasMore || loadingMore);
  const footerHeight = showLoadMoreFooter ? 56 : 0;
  const totalHeight = windowRowCount * activeRowHeight + footerHeight;
  const visibleCount = Math.min(loadedBookCount, totalCount);

  if (!loading && hasError && totalCount === 0 && loadedBookCount === 0) {
    return null;
  }

  if (!loading && totalCount === 0) {
    const title = hasActiveQuery ? 'Nothing found' : 'No books in catalog';
    const body = hasActiveQuery
      ? 'Try a different search or clear the active filters.'
      : 'The cached catalog does not contain any books yet.';
    const image = hasActiveQuery ? noResultsUrl : emptyCatalogUrl;

    return (
      <section className="empty-state" aria-label="Empty catalog">
        <img src={image} alt="" className={hasActiveQuery ? 'empty-illustration no-results' : 'empty-illustration'} />
        <h2>{title}</h2>
        <p>{body}</p>
      </section>
    );
  }

  return (
    <section className="grid-panel" aria-label="Book catalog">
      <div className="grid-summary">
        <span>{visibleCount.toLocaleString()} of {totalCount.toLocaleString()} books</span>
        {loading && <span className="loading-dot">Updating</span>}
        {loadingMore && <span className="loading-dot">Loading more</span>}
      </div>
      <div ref={parentRef} className="book-grid-scroll">
        <div
          data-window-first-row={rowWindow.first}
          style={{ height: `${totalHeight}px`, position: 'relative' }}
        >
          {visibleRows.map((virtualRow) => (
            <div
              key={virtualRow.key}
              className={isMobileCatalog ? 'virtual-row mobile-virtual-row' : 'virtual-row'}
              style={{
                transform: `translateY(${virtualRow.start}px)`,
                gridTemplateColumns: isMobileCatalog ? '1fr' : `repeat(${columns}, minmax(${cardCellWidth}px, 1fr))`
              }}
            >
              {booksForRow(virtualRow.index).map((book) => (
                isMobileCatalog
                  ? (
                      <BookListRow
                        key={book.id}
                        book={book}
                        selected={selectedId === book.id}
                        onSelect={onSelectBook}
                      />
                    )
                  : (
                      <BookCard
                        key={book.id}
                        book={book}
                        selected={selectedId === book.id}
                        token={token}
                        onSelect={onSelectBook}
                      />
                    )
              ))}
            </div>
          ))}
          {showLoadMoreFooter && (
            <div
              className="grid-load-more"
              style={{ transform: `translateY(${windowRowCount * activeRowHeight}px)` }}
            >
              {loadingMore ? 'Loading more books...' : 'Scroll for more books'}
            </div>
          )}
        </div>
      </div>
    </section>
  );
}
