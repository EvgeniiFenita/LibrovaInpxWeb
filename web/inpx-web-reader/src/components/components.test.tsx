import { focusManager, QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { act, fireEvent, render, renderHook, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { StrictMode } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { App, buildDownloadToastText, triggerBrowserDownload } from '../App';
import { ApiError, apiClient, defaultBookQuery } from '../api/client';
import type { BookListResponse } from '../api/types';
import { useCursorBookPages } from '../hooks/useCursorBookPages';
import { activeScan, mockBook, mockBookList, mockDetails, mockStats, mockStatus, mockSource } from '../test/mockData';
import { BookDetailsDrawer } from './BookDetailsDrawer';
import {
  BookGrid,
  maxVirtualScrollHeightPx,
  resolveVirtualRowWindow,
  resolveVisibleRowRange
} from './BookGrid';
import { CoverPlaceholder } from './CoverPlaceholder';
import { SearchToolbar } from './SearchToolbar';
import { SettingsDialog } from './SettingsDialog';
import { StartupScanDialog } from './StartupScanDialog';
import { ToastRegion, toastAutoDismissMs } from './ToastRegion';

function stubMobileCatalogMedia(matches: boolean) {
  vi.stubGlobal('matchMedia', (query: string) => ({
    matches,
    media: query,
    onchange: null,
    addEventListener: vi.fn(),
    removeEventListener: vi.fn(),
    addListener: vi.fn(),
    removeListener: vi.fn(),
    dispatchEvent: vi.fn()
  }));
}

function scrollCatalogToEnd() {
  const scrollContainer = document.querySelector<HTMLElement>('.book-grid-scroll');
  expect(scrollContainer).not.toBeNull();
  Object.defineProperties(scrollContainer!, {
    clientHeight: { configurable: true, value: 800 },
    scrollHeight: { configurable: true, value: 800 }
  });
  scrollContainer!.scrollTop = 0;
  fireEvent.scroll(scrollContainer!);
}

describe('web UI components', () => {
  afterEach(() => {
    vi.restoreAllMocks();
    vi.useRealTimers();
    focusManager.setFocused(undefined);
    window.localStorage.clear();
    window.history.replaceState(null, '', '/');
  });

  it('keeps toolbar facets visible while a filtered catalog refresh is pending', async () => {
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    const pendingFilteredBooks = new Promise<typeof mockBookList>(() => undefined);
    const initialBookList = {
      ...mockBookList,
      facets: {
        languages: [{ value: 'en', count: 7 }, { value: 'ru', count: 5 }],
        genres: [{ value: 'Science Fiction', count: 7 }, { value: 'History', count: 3 }]
      }
    };

    vi.spyOn(apiClient, 'getStatus').mockResolvedValue({
      ...mockStatus,
      scan: { active: false, status: 'completed' }
    });
    vi.spyOn(apiClient, 'getSource').mockResolvedValue(mockSource);
    vi.spyOn(apiClient, 'getScanProgress').mockResolvedValue({ active: false, status: 'completed' });
    vi.spyOn(apiClient, 'getStats').mockResolvedValue(mockStatus.runtime.storage.cacheDatabasePresent ? {
      bookCount: 12,
      unavailableBookCount: 0,
      inpxSourceSizeBytes: 0,
      coverCacheSizeBytes: 0,
      databaseSizeBytes: 4096,
      totalCatalogSizeBytes: 4096
    } : mockStats);
    vi.spyOn(apiClient, 'getCover').mockResolvedValue(new Blob(['cover'], { type: 'image/png' }));
    vi.spyOn(apiClient, 'getBooks').mockImplementation((query) => (
      query.language ? pendingFilteredBooks : Promise.resolve(initialBookList)
    ));

    render(
      <QueryClientProvider client={queryClient}>
        <App />
      </QueryClientProvider>
    );

    await screen.findByRole('heading', { name: 'InpxWebReader' });
    fireEvent.click(screen.getByText('No filters active'));

    expect(await screen.findByRole('button', { name: 'en (7)' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'ru (5)' })).toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: 'en (7)' }));

    await waitFor(() => expect(apiClient.getBooks).toHaveBeenCalledTimes(2));
    expect(screen.getByRole('button', { name: 'ru (5)' })).toBeInTheDocument();

    queryClient.clear();
  });

  it('abandons in-flight pagination when the source generation changes', async () => {
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    let generation = 'fixture';
    let oldPageSignal: AbortSignal | undefined;
    let resolveOldPage: ((page: BookListResponse) => void) | undefined;
    vi.spyOn(apiClient, 'getStatus').mockResolvedValue({
      ...mockStatus,
      scan: { active: false, status: 'idle' }
    });
    vi.spyOn(apiClient, 'getSource').mockResolvedValue(mockSource);
    vi.spyOn(apiClient, 'getStats').mockResolvedValue(mockStats);
    vi.spyOn(apiClient, 'getCover').mockResolvedValue(new Blob(['cover'], { type: 'image/png' }));
    const getBooks = vi.spyOn(apiClient, 'getBooks').mockImplementation((_bookQuery, _token, signal, cursor) => {
      if (generation === 'fixture' && cursor) {
        oldPageSignal = signal;
        return new Promise<BookListResponse>((resolve) => {
          resolveOldPage = resolve;
        });
      }

      if (generation === 'fixture') {
        return Promise.resolve({
          ...mockBookList,
          items: [{ ...mockBook, title: 'Old generation book' }],
          totalCount: 2,
          limit: 1,
          nextCursor: 'old-generation-cursor'
        });
      }

      return Promise.resolve({
        ...mockBookList,
        items: [{ ...mockBook, id: 70, title: 'New generation book' }],
        totalCount: 1
      });
    });

    render(
      <QueryClientProvider client={queryClient}>
        <App />
      </QueryClientProvider>
    );

    expect(await screen.findByText('Old generation book')).toBeInTheDocument();
    scrollCatalogToEnd();
    await waitFor(() => expect(getBooks).toHaveBeenCalledWith(
      expect.objectContaining({ offset: 0 }),
      '',
      expect.any(AbortSignal),
      'old-generation-cursor'
    ));

    generation = 'next';
    act(() => {
      queryClient.setQueryData(['source', ''], {
        source: {
          ...mockSource.source!,
          lastSeenSnapshotId: 'next-generation'
        }
      });
    });

    expect(await screen.findByText('New generation book')).toBeInTheDocument();
    expect(screen.queryByText('Old generation book')).not.toBeInTheDocument();
    expect(oldPageSignal?.aborted).toBe(true);

    await act(async () => {
      resolveOldPage?.({
        ...mockBookList,
        items: [{ ...mockBook, id: 71, title: 'Late old continuation' }],
        offset: 1,
        totalCount: null,
        limit: 1,
        nextCursor: null,
        facets: null
      });
      await Promise.resolve();
    });
    expect(screen.queryByText('Late old continuation')).not.toBeInTheDocument();

    queryClient.clear();
  });

  it('resets infinite pagination instead of merging server snapshot generations', async () => {
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    let firstPageRequests = 0;
    vi.spyOn(apiClient, 'getStatus').mockResolvedValue({
      ...mockStatus,
      scan: { active: false, status: 'idle' }
    });
    vi.spyOn(apiClient, 'getSource').mockResolvedValue(mockSource);
    vi.spyOn(apiClient, 'getStats').mockResolvedValue(mockStats);
    vi.spyOn(apiClient, 'getCover').mockResolvedValue(new Blob(['cover'], { type: 'image/png' }));
    const getBooks = vi.spyOn(apiClient, 'getBooks').mockImplementation(async (_bookQuery, _token, _signal, cursor) => {
      if (cursor) {
        return {
          ...mockBookList,
          catalogSnapshotId: 'snapshot-b',
          items: [{ ...mockBook, id: 8, title: 'Mismatched second page' }],
          offset: 1,
          totalCount: null,
          limit: 1,
          nextCursor: null,
          facets: null
        };
      }

      firstPageRequests += 1;
      if (firstPageRequests === 1) {
        return {
          ...mockBookList,
          catalogSnapshotId: 'snapshot-a',
          items: [{ ...mockBook, title: 'Old first page' }],
          totalCount: 2,
          limit: 1,
          nextCursor: 'snapshot-a-cursor'
        };
      }

      return {
        ...mockBookList,
        catalogSnapshotId: 'snapshot-b',
        items: [{ ...mockBook, id: 70, title: 'Coherent refreshed page' }],
        totalCount: 1
      };
    });

    render(
      <QueryClientProvider client={queryClient}>
        <App />
      </QueryClientProvider>
    );

    expect(await screen.findByText('Old first page')).toBeInTheDocument();
    scrollCatalogToEnd();
    await waitFor(() => expect(getBooks).toHaveBeenCalledWith(
      expect.objectContaining({ offset: 0 }),
      '',
      expect.any(AbortSignal),
      'snapshot-a-cursor'
    ));
    expect(await screen.findByText('Coherent refreshed page')).toBeInTheDocument();
    expect(screen.queryByText('Old first page')).not.toBeInTheDocument();
    expect(screen.queryByText('Mismatched second page')).not.toBeInTheDocument();
    expect(firstPageRequests).toBe(2);

    queryClient.clear();
  });

  it('restarts catalog pagination after the server expires a cursor session', async () => {
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    let firstPageRequests = 0;
    vi.spyOn(apiClient, 'getStatus').mockResolvedValue({
      ...mockStatus,
      scan: { active: false, status: 'idle' }
    });
    vi.spyOn(apiClient, 'getSource').mockResolvedValue(mockSource);
    vi.spyOn(apiClient, 'getStats').mockResolvedValue(mockStats);
    vi.spyOn(apiClient, 'getCover').mockResolvedValue(new Blob(['cover'], { type: 'image/png' }));
    const getBooks = vi.spyOn(apiClient, 'getBooks').mockImplementation(async (
      _bookQuery,
      _token,
      _signal,
      cursor
    ) => {
      if (cursor) {
        throw new ApiError(409, 'catalog_cursor_expired', 'Cursor expired.');
      }

      firstPageRequests += 1;
      if (firstPageRequests === 1) {
        return {
          ...mockBookList,
          items: [{ ...mockBook, title: 'Expired first page' }],
          totalCount: 2,
          limit: 1,
          nextCursor: 'expiring-cursor'
        };
      }
      return {
        ...mockBookList,
        items: [{ ...mockBook, id: 70, title: 'Restarted first page' }],
        totalCount: 1,
        nextCursor: null
      };
    });

    render(
      <QueryClientProvider client={queryClient}>
        <App />
      </QueryClientProvider>
    );

    expect(await screen.findByText('Expired first page')).toBeInTheDocument();
    scrollCatalogToEnd();
    expect(await screen.findByText('Restarted first page')).toBeInTheDocument();
    expect(screen.queryByText('Expired first page')).not.toBeInTheDocument();
    expect(firstPageRequests).toBe(2);
    expect(getBooks).toHaveBeenCalledWith(
      expect.objectContaining({ offset: 0 }),
      '',
      expect.any(AbortSignal),
      'expiring-cursor'
    );

    queryClient.clear();
  });

  it('keeps loaded cursor pages when the window regains focus', async () => {
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    vi.spyOn(apiClient, 'getStatus').mockResolvedValue({
      ...mockStatus,
      scan: { active: false, status: 'idle' }
    });
    vi.spyOn(apiClient, 'getSource').mockResolvedValue(mockSource);
    vi.spyOn(apiClient, 'getStats').mockResolvedValue(mockStats);
    vi.spyOn(apiClient, 'getCover').mockResolvedValue(new Blob(['cover'], { type: 'image/png' }));
    const getBooks = vi.spyOn(apiClient, 'getBooks').mockImplementation(async (
      _bookQuery,
      _token,
      _signal,
      cursor
    ) => cursor
      ? {
          ...mockBookList,
          items: [{ ...mockBook, id: 8, title: 'Focused second page' }],
          offset: 1,
          totalCount: null,
          limit: 1,
          nextCursor: null,
          facets: null
        }
      : {
          ...mockBookList,
          items: [{ ...mockBook, title: 'Focused first page' }],
          totalCount: 2,
          limit: 1,
          nextCursor: 'focus-cursor'
        });

    render(
      <QueryClientProvider client={queryClient}>
        <App />
      </QueryClientProvider>
    );

    expect(await screen.findByText('Focused first page')).toBeInTheDocument();
    scrollCatalogToEnd();
    expect(await screen.findByText('Focused second page')).toBeInTheDocument();
    expect(getBooks).toHaveBeenCalledTimes(2);

    await act(async () => {
      focusManager.setFocused(false);
      focusManager.setFocused(true);
      await new Promise((resolve) => window.setTimeout(resolve, 0));
    });

    expect(getBooks).toHaveBeenCalledTimes(2);
    expect(screen.getByText('Focused first page')).toBeInTheDocument();
    expect(screen.getByText('Focused second page')).toBeInTheDocument();

    queryClient.clear();
  });

  it('coalesces concurrent continuation requests and stops after the final cursor', async () => {
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    let resolveContinuation: ((page: BookListResponse) => void) | undefined;
    const getBooks = vi.spyOn(apiClient, 'getBooks').mockImplementation((
      _bookQuery,
      _token,
      _signal,
      cursor
    ) => cursor
      ? new Promise<BookListResponse>((resolve) => {
          resolveContinuation = resolve;
        })
      : Promise.resolve({
          ...mockBookList,
          items: [{ ...mockBook, title: 'Concurrent first page' }],
          totalCount: 2,
          limit: 1,
          nextCursor: 'concurrent-cursor'
        }));
    const query = { ...defaultBookQuery, limit: 1 };
    const { result } = renderHook(() => useCursorBookPages({
      catalogContextKey: 'concurrent-catalog',
      query,
      token: '',
      enabled: true
    }), {
      wrapper: ({ children }) => (
        <QueryClientProvider client={queryClient}>{children}</QueryClientProvider>
      )
    });

    await waitFor(() => expect(result.current.hasNextPage).toBe(true));

    let firstLoad!: Promise<void>;
    await act(async () => {
      firstLoad = result.current.fetchNextPage();
      await result.current.fetchNextPage();
    });
    expect(getBooks).toHaveBeenCalledTimes(2);

    const completeContinuation = resolveContinuation;
    if (!completeContinuation) {
      throw new Error('Continuation request was not started.');
    }
    await act(async () => {
      completeContinuation({
        ...mockBookList,
        items: [{ ...mockBook, id: 8, title: 'Concurrent second page' }],
        offset: 1,
        totalCount: null,
        limit: 1,
        nextCursor: null,
        facets: null
      });
      await firstLoad;
    });

    expect(result.current.pages).toHaveLength(2);
    expect(result.current.pages[1].items[0].title).toBe('Concurrent second page');
    expect(result.current.hasNextPage).toBe(false);

    await act(async () => result.current.fetchNextPage());
    expect(getBooks).toHaveBeenCalledTimes(2);

    queryClient.clear();
  });

  it('publishes the first page without an empty successful render', async () => {
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    let firstPageResolved = false;
    let resolveFirstPage: ((page: BookListResponse) => void) | undefined;
    const getBooks = vi.spyOn(apiClient, 'getBooks').mockImplementation(() => (
      new Promise<BookListResponse>((resolve) => {
        resolveFirstPage = resolve;
      })
    ));
    const observedStates: Array<{
      firstPageResolved: boolean;
      isFetchingFirstPage: boolean;
      pageCount: number;
      hasError: boolean;
    }> = [];
    const query = { ...defaultBookQuery, limit: 1 };
    const { result, unmount } = renderHook(() => {
      const current = useCursorBookPages({
        catalogContextKey: 'first-page-render-catalog',
        query,
        token: '',
        enabled: true
      });
      observedStates.push({
        firstPageResolved,
        isFetchingFirstPage: current.isFetchingFirstPage,
        pageCount: current.pages.length,
        hasError: Boolean(current.error)
      });
      return current;
    }, {
      wrapper: ({ children }) => (
        <QueryClientProvider client={queryClient}>{children}</QueryClientProvider>
      )
    });

    await waitFor(() => expect(getBooks).toHaveBeenCalledTimes(1));
    const completeFirstPage = resolveFirstPage;
    if (!completeFirstPage) {
      throw new Error('First-page request was not started.');
    }
    await act(async () => {
      firstPageResolved = true;
      completeFirstPage({
        ...mockBookList,
        items: [{ ...mockBook, title: 'Synchronously published first page' }],
        totalCount: 1,
        limit: 1,
        nextCursor: null
      });
      await Promise.resolve();
    });

    await waitFor(() => expect(result.current.pages).toHaveLength(1));
    expect(observedStates).not.toContainEqual({
      firstPageResolved: true,
      isFetchingFirstPage: false,
      pageCount: 0,
      hasError: false
    });

    unmount();
    queryClient.clear();
  });

  it('preserves a cached first page and cursor through StrictMode effect replay', async () => {
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    const query = { ...defaultBookQuery, limit: 1 };
    const firstPage = {
      ...mockBookList,
      items: [{ ...mockBook, title: 'StrictMode first page' }],
      totalCount: 2,
      limit: 1,
      nextCursor: 'strict-mode-cursor'
    };
    queryClient.setQueryData(['books', 'strict-mode-catalog', query, ''], firstPage);
    const getBooks = vi.spyOn(apiClient, 'getBooks').mockResolvedValue({
      ...mockBookList,
      items: [{ ...mockBook, id: 8, title: 'StrictMode second page' }],
      offset: 1,
      totalCount: null,
      limit: 1,
      nextCursor: null,
      facets: null
    });

    const { result, unmount } = renderHook(() => useCursorBookPages({
      catalogContextKey: 'strict-mode-catalog',
      query,
      token: '',
      enabled: true
    }), {
      wrapper: ({ children }) => (
        <StrictMode>
          <QueryClientProvider client={queryClient}>{children}</QueryClientProvider>
        </StrictMode>
      )
    });

    await waitFor(() => expect(result.current.pages).toHaveLength(1));
    expect(result.current.pages[0].items[0].title).toBe('StrictMode first page');
    expect(result.current.hasNextPage).toBe(true);

    await act(async () => result.current.fetchNextPage());

    expect(result.current.pages).toHaveLength(2);
    expect(result.current.pages[1].items[0].title).toBe('StrictMode second page');
    expect(getBooks).toHaveBeenCalledWith(
      expect.objectContaining({ offset: 0 }),
      '',
      expect.any(AbortSignal),
      'strict-mode-cursor'
    );

    unmount();
    queryClient.clear();
  });

  it('retries a transient continuation error without discarding loaded pages', async () => {
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    let continuationAttempts = 0;
    const getBooks = vi.spyOn(apiClient, 'getBooks').mockImplementation(async (
      _bookQuery,
      _token,
      _signal,
      cursor
    ) => {
      if (!cursor) {
        return {
          ...mockBookList,
          items: [{ ...mockBook, title: 'Retry first page' }],
          totalCount: 2,
          limit: 1,
          nextCursor: 'retry-cursor'
        };
      }

      continuationAttempts += 1;
      if (continuationAttempts === 1) {
        throw new ApiError(503, 'backend_unavailable', 'Continuation failed.');
      }
      return {
        ...mockBookList,
        items: [{ ...mockBook, id: 8, title: 'Retried second page' }],
        offset: 1,
        totalCount: null,
        limit: 1,
        nextCursor: null,
        facets: null
      };
    });
    const query = { ...defaultBookQuery, limit: 1 };
    const { result } = renderHook(() => useCursorBookPages({
      catalogContextKey: 'retry-catalog',
      query,
      token: '',
      enabled: true
    }), {
      wrapper: ({ children }) => (
        <QueryClientProvider client={queryClient}>{children}</QueryClientProvider>
      )
    });

    await waitFor(() => expect(result.current.hasNextPage).toBe(true));
    await act(async () => result.current.fetchNextPage());

    expect(result.current.error).toMatchObject({ message: 'Continuation failed.' });
    expect(result.current.hasNextPage).toBe(false);
    expect(result.current.pages).toHaveLength(1);
    expect(result.current.pages[0].items[0].title).toBe('Retry first page');

    await act(async () => result.current.retry());

    expect(result.current.error).toBeNull();
    expect(result.current.pages).toHaveLength(2);
    expect(result.current.pages[1].items[0].title).toBe('Retried second page');
    expect(getBooks).toHaveBeenCalledTimes(3);

    queryClient.clear();
  });

  it('opens the cached catalog without starting a scan when startup progress is idle', async () => {
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    vi.spyOn(apiClient, 'getStatus').mockResolvedValue({
      ...mockStatus,
      scan: { active: false, status: 'idle' }
    });
    vi.spyOn(apiClient, 'getSource').mockResolvedValue(mockSource);
    const getScanProgress = vi.spyOn(apiClient, 'getScanProgress').mockImplementation(() => new Promise(() => undefined));
    vi.spyOn(apiClient, 'getStats').mockResolvedValue(mockStats);
    vi.spyOn(apiClient, 'getCover').mockResolvedValue(new Blob(['cover'], { type: 'image/png' }));
    vi.spyOn(apiClient, 'getBooks').mockResolvedValue(mockBookList);
    const startScan = vi.spyOn(apiClient, 'startScan').mockResolvedValue({
      jobId: 9,
      scan: activeScan
    });

    render(
      <QueryClientProvider client={queryClient}>
        <App />
      </QueryClientProvider>
    );

    await screen.findByRole('heading', { name: 'InpxWebReader' });

    expect(startScan).not.toHaveBeenCalled();
    expect(getScanProgress).not.toHaveBeenCalled();
    expect(screen.queryByRole('main', { name: 'Server startup scan' })).not.toBeInTheDocument();

    queryClient.clear();
  });

  it('forwards startup scan cancellation through the API client', async () => {
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    const cancellingScan = { ...activeScan, status: 'cancelling' as const };
    let cancellationAccepted = false;
    vi.spyOn(apiClient, 'getStatus').mockImplementation(() => Promise.resolve({
      ...mockStatus,
      scan: cancellationAccepted ? cancellingScan : activeScan
    }));
    vi.spyOn(apiClient, 'getSource').mockResolvedValue(mockSource);
    vi.spyOn(apiClient, 'getScanProgress').mockImplementation(() => Promise.resolve(
      cancellationAccepted ? cancellingScan : activeScan
    ));
    vi.spyOn(apiClient, 'getStats').mockResolvedValue(mockStats);
    vi.spyOn(apiClient, 'getCover').mockResolvedValue(new Blob(['cover'], { type: 'image/png' }));
    vi.spyOn(apiClient, 'getBooks').mockResolvedValue(mockBookList);
    const cancelScan = vi.spyOn(apiClient, 'cancelScan').mockImplementation(async () => {
      cancellationAccepted = true;
      return {
        accepted: true,
        scan: cancellingScan
      };
    });

    render(
      <QueryClientProvider client={queryClient}>
        <App />
      </QueryClientProvider>
    );

    fireEvent.click(await screen.findByRole('button', { name: 'Cancel scan' }));
    await waitFor(() => expect(cancelScan).toHaveBeenCalledTimes(1));
    expect(await screen.findByRole('button', { name: 'Cancelling…' })).toBeDisabled();

    queryClient.clear();
  });

  it('keeps scan cancellation errors inside the active startup dialog', async () => {
    const user = userEvent.setup();
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    vi.spyOn(apiClient, 'getStatus').mockResolvedValue({ ...mockStatus, scan: activeScan });
    vi.spyOn(apiClient, 'getSource').mockResolvedValue(mockSource);
    vi.spyOn(apiClient, 'getScanProgress').mockResolvedValue(activeScan);
    vi.spyOn(apiClient, 'cancelScan').mockRejectedValue(new Error('Cancellation service is unavailable.'));

    render(
      <QueryClientProvider client={queryClient}>
        <App />
      </QueryClientProvider>
    );

    await user.click(await screen.findByRole('button', { name: 'Cancel scan' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('Cancellation service is unavailable.');
    expect(screen.getByRole('button', { name: 'Cancel scan' })).toBeEnabled();
    expect(screen.getByLabelText('Notifications')).toHaveTextContent('Cancellation service is unavailable.');

    queryClient.clear();
  });

  it('holds an external scan terminal outcome and refreshes catalog details before continuing', async () => {
    const user = userEvent.setup();
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    let postScan = false;
    vi.spyOn(apiClient, 'getStatus').mockResolvedValue({
      ...mockStatus,
      scan: { active: false, status: 'idle' }
    });
    vi.spyOn(apiClient, 'getSource').mockResolvedValue(mockSource);
    vi.spyOn(apiClient, 'getStats').mockResolvedValue(mockStats);
    const getBooks = vi.spyOn(apiClient, 'getBooks').mockImplementation(async () => ({
      ...mockBookList,
      items: [{
        ...mockBook,
        title: postScan ? 'Refreshed Test Book' : 'Test Book',
        actions: {
          ...mockBook.actions,
          canDownloadOriginal: !postScan
        }
      }]
    }));
    const getDetails = vi.spyOn(apiClient, 'getBookDetails').mockImplementation(async () => ({
      book: {
        ...mockDetails,
        title: postScan ? 'Refreshed Test Book' : 'Test Book',
        actions: {
          ...mockDetails.actions,
          canDownloadOriginal: !postScan
        }
      }
    }));
    vi.spyOn(apiClient, 'getCover').mockResolvedValue(new Blob(['cover'], { type: 'image/png' }));

    render(
      <QueryClientProvider client={queryClient}>
        <App />
      </QueryClientProvider>
    );

    await user.click(await screen.findByRole('button', { name: /Test Book/i }));
    expect(await screen.findByRole('button', { name: 'Original' })).toBeEnabled();
    expect(getDetails).toHaveBeenCalledTimes(1);

    act(() => {
      queryClient.setQueryData(['scan', ''], {
        ...activeScan,
        jobId: 11
      });
    });
    await waitFor(() => expect(queryClient.getQueryData(['scan', ''])).toMatchObject({ active: true }));

    postScan = true;
    act(() => {
      queryClient.setQueryData(['scan', ''], {
        ...activeScan,
        active: false,
        jobId: 11,
        status: 'completed',
        percent: 100,
        result: {
          totalRecords: 1,
          scannedRecords: 1,
          parsedFb2Records: 0,
          addedRecords: 0,
          updatedRecords: 1,
          markedUnavailableRecords: 0,
          unavailableRecords: 0,
          skippedRecords: 0,
          reusedRecords: 1,
          segmentsTotal: 1,
          segmentsUnchanged: 1,
          segmentsAdded: 0,
          segmentsChanged: 0,
          segmentsRemoved: 0,
          archivesSkipped: 1,
          archivesOpened: 0,
          archiveBytesRead: 0,
          warningCount: 0
        }
      });
    });

    expect(await screen.findByRole('heading', { name: 'Scan completed' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Continue' })).toBeEnabled();
    await waitFor(() => expect(getDetails).toHaveBeenCalledTimes(2));

    await user.click(screen.getByRole('button', { name: 'Continue' }));
    expect(await screen.findByRole('dialog', { name: 'Book details' })).toHaveTextContent('Refreshed Test Book');
    expect(screen.getByRole('button', { name: 'Original' })).toBeDisabled();
    await waitFor(() => expect(getBooks.mock.calls.length).toBeGreaterThanOrEqual(2));

    queryClient.clear();
  });

  it('keeps polling stale active progress after status observes the same scan completing', async () => {
    vi.useFakeTimers();
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    const runningScan = { ...activeScan, jobId: 11 };
    const completedScan = {
      ...runningScan,
      active: false,
      status: 'completed' as const,
      percent: 100,
      result: {
        totalRecords: 1,
        scannedRecords: 1,
        parsedFb2Records: 0,
        addedRecords: 1,
        updatedRecords: 0,
        markedUnavailableRecords: 0,
        unavailableRecords: 0,
        skippedRecords: 0,
        reusedRecords: 0,
        segmentsTotal: 1,
        segmentsUnchanged: 0,
        segmentsAdded: 1,
        segmentsChanged: 0,
        segmentsRemoved: 0,
        archivesSkipped: 0,
        archivesOpened: 1,
        archiveBytesRead: 2048,
        warningCount: 0
      }
    };

    vi.spyOn(apiClient, 'getStatus').mockResolvedValue({ ...mockStatus, scan: runningScan });
    vi.spyOn(apiClient, 'getSource').mockResolvedValue(mockSource);
    const getScanProgress = vi.spyOn(apiClient, 'getScanProgress')
      .mockResolvedValueOnce(runningScan)
      .mockResolvedValue(completedScan);
    vi.spyOn(apiClient, 'getStats').mockResolvedValue(mockStats);
    vi.spyOn(apiClient, 'getCover').mockResolvedValue(new Blob(['cover'], { type: 'image/png' }));
    vi.spyOn(apiClient, 'getBooks').mockResolvedValue(mockBookList);

    try {
      render(
        <QueryClientProvider client={queryClient}>
          <App />
        </QueryClientProvider>
      );

      await vi.waitFor(() => expect(getScanProgress).toHaveBeenCalledTimes(1));
      expect(screen.getByRole('main', { name: 'Server startup scan' })).toBeInTheDocument();

      act(() => {
        queryClient.setQueryData(['status', ''], { ...mockStatus, scan: completedScan });
      });
      expect(queryClient.getQueryData(['scan', ''])).toMatchObject({ active: true, jobId: 11 });

      await act(async () => {
        await vi.advanceTimersByTimeAsync(1200);
      });

      await vi.waitFor(() => expect(getScanProgress).toHaveBeenCalledTimes(2));
      expect(screen.getByRole('heading', { name: 'Scan completed' })).toBeInTheDocument();
    } finally {
      queryClient.clear();
      vi.useRealTimers();
    }
  });

  it('keeps scan start errors visible in both the startup dialog and global notifications', async () => {
    const user = userEvent.setup();
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    vi.spyOn(apiClient, 'getStatus').mockResolvedValue({
      ...mockStatus,
      scan: { active: false, status: 'idle' }
    });
    vi.spyOn(apiClient, 'getSource').mockResolvedValue(mockSource);
    vi.spyOn(apiClient, 'getStats').mockResolvedValue(mockStats);
    vi.spyOn(apiClient, 'getBooks').mockResolvedValue(mockBookList);
    vi.spyOn(apiClient, 'getCover').mockResolvedValue(new Blob(['cover'], { type: 'image/png' }));
    vi.spyOn(apiClient, 'startScan').mockRejectedValue(new Error('Scan service is unavailable.'));

    render(
      <QueryClientProvider client={queryClient}>
        <App />
      </QueryClientProvider>
    );

    await screen.findByRole('heading', { name: 'InpxWebReader' });
    await user.click(screen.getByRole('button', { name: 'Settings' }));
    await user.click(screen.getByRole('button', { name: 'Rescan' }));

    expect(await screen.findByRole('heading', { name: 'Scan could not start' })).toBeInTheDocument();
    expect(screen.getByRole('alert')).toHaveTextContent('Scan service is unavailable.');
    expect(screen.getByLabelText('Notifications')).toHaveTextContent('Scan service is unavailable.');
    expect(screen.getByRole('button', { name: 'Continue' })).toBeEnabled();

    queryClient.clear();
  });

  it('polls status quickly while the server is still opening for an initial scan', async () => {
    vi.useFakeTimers();
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    const getStatus = vi.spyOn(apiClient, 'getStatus')
      .mockResolvedValueOnce({
        ...mockStatus,
        status: 'closed',
        scan: { active: false, status: 'idle' }
      })
      .mockResolvedValue({
        ...mockStatus,
        scan: activeScan
      });
    vi.spyOn(apiClient, 'getSource').mockResolvedValue(mockSource);
    const getScanProgress = vi.spyOn(apiClient, 'getScanProgress').mockResolvedValue(activeScan);
    vi.spyOn(apiClient, 'getStats').mockResolvedValue(mockStats);
    vi.spyOn(apiClient, 'getCover').mockResolvedValue(new Blob(['cover'], { type: 'image/png' }));
    vi.spyOn(apiClient, 'getBooks').mockResolvedValue(mockBookList);

    try {
      render(
        <QueryClientProvider client={queryClient}>
          <App />
        </QueryClientProvider>
      );

      await vi.waitFor(() => expect(getStatus).toHaveBeenCalledTimes(1));
      expect(screen.queryByRole('main', { name: 'Server startup scan' })).not.toBeInTheDocument();

      await act(async () => {
        await vi.advanceTimersByTimeAsync(1200);
      });
      await vi.waitFor(() => expect(getStatus).toHaveBeenCalledTimes(2));
      expect(screen.getByRole('main', { name: 'Server startup scan' })).toBeInTheDocument();
      expect(getScanProgress).toHaveBeenCalled();
    } finally {
      queryClient.clear();
      vi.useRealTimers();
    }
  });

  it('stops scan polling after auth failure until the token changes', async () => {
    window.localStorage.clear();
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    const unauthorized = new ApiError(401, 'unauthorized', 'Token required.');
    const getStatus = vi.spyOn(apiClient, 'getStatus').mockImplementation((requestToken) => (
      requestToken === 'new-token'
        ? Promise.resolve({ ...mockStatus, scan: { active: false, status: 'idle' } })
        : Promise.reject(unauthorized)
    ));
    vi.spyOn(apiClient, 'getSource').mockResolvedValue(mockSource);
    const getScanProgress = vi.spyOn(apiClient, 'getScanProgress').mockImplementation((requestToken) => (
      requestToken === 'new-token'
        ? Promise.resolve({ active: false, status: 'idle' })
        : Promise.reject(unauthorized)
    ));
    vi.spyOn(apiClient, 'getStats').mockResolvedValue(mockStats);
    vi.spyOn(apiClient, 'getBooks').mockResolvedValue(mockBookList);
    vi.spyOn(apiClient, 'getCover').mockResolvedValue(new Blob(['cover'], { type: 'image/png' }));

    render(
      <QueryClientProvider client={queryClient}>
        <App />
      </QueryClientProvider>
    );

    await screen.findByRole('heading', { name: 'Server access' });
    const scanCallsAfterGate = getScanProgress.mock.calls.length;

    vi.useFakeTimers();
    try {
      await act(async () => {
        await vi.advanceTimersByTimeAsync(1200);
      });
      expect(getScanProgress).toHaveBeenCalledTimes(scanCallsAfterGate);
    } finally {
      vi.useRealTimers();
    }

    const user = userEvent.setup();
    const tokenInput = screen.getByLabelText('Server access token');
    await user.type(tokenInput, 'new-token');

    expect(tokenInput).toHaveValue('new-token');
    expect(screen.getByRole('dialog', { name: 'Server access' })).toBeInTheDocument();
    expect(getStatus).toHaveBeenCalledTimes(1);

    await user.click(screen.getByRole('button', { name: 'Unlock catalog' }));

    await waitFor(() => expect(getStatus).toHaveBeenCalledWith('new-token'));
    await screen.findByRole('heading', { name: 'InpxWebReader' });
    expect(getScanProgress).toHaveBeenCalledTimes(scanCallsAfterGate);

    queryClient.clear();
  });

  it('shows catalog load errors without rendering the empty-catalog state', async () => {
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    vi.spyOn(apiClient, 'getStatus').mockResolvedValue({
      ...mockStatus,
      scan: { active: false, status: 'idle' }
    });
    vi.spyOn(apiClient, 'getSource').mockResolvedValue(mockSource);
    vi.spyOn(apiClient, 'getScanProgress').mockResolvedValue({ active: false, status: 'idle' });
    vi.spyOn(apiClient, 'getStats').mockResolvedValue(mockStats);
    vi.spyOn(apiClient, 'getCover').mockResolvedValue(new Blob(['cover'], { type: 'image/png' }));
    vi.spyOn(apiClient, 'getBooks')
      .mockRejectedValueOnce(new ApiError(503, 'backend_unavailable', 'Catalog failed.'))
      .mockResolvedValue(mockBookList);

    render(
      <QueryClientProvider client={queryClient}>
        <App />
      </QueryClientProvider>
    );

    expect(await screen.findByRole('alert')).toHaveTextContent('Catalog failed.');
    expect(screen.queryByRole('heading', { name: 'No books in catalog' })).not.toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: 'Retry catalog' }));
    expect(await screen.findByText('Test Book')).toBeInTheDocument();
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();

    queryClient.clear();
  });

  it('shows an unconfigured source truthfully and preserves a filtered zero count', async () => {
    window.history.replaceState(null, '', '/?text=missing');
    const user = userEvent.setup();
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    vi.spyOn(apiClient, 'getStatus').mockResolvedValue({
      ...mockStatus,
      scan: { active: false, status: 'idle' },
      capabilities: {
        canRescanInpxSource: false,
        canDownloadOriginal: false,
        canDownloadAsEpub: false
      },
      inpxSource: undefined
    });
    vi.spyOn(apiClient, 'getSource').mockResolvedValue({ source: null });
    vi.spyOn(apiClient, 'getStats').mockResolvedValue({
      ...mockStats,
      bookCount: 17
    });
    vi.spyOn(apiClient, 'getBooks').mockResolvedValue({
      ...mockBookList,
      items: [],
      totalCount: 0,
      facets: { languages: [], genres: [] }
    });

    render(
      <QueryClientProvider client={queryClient}>
        <App />
      </QueryClientProvider>
    );

    await screen.findByRole('heading', { name: 'InpxWebReader' });
    expect(screen.getByText('Source not configured')).toBeInTheDocument();
    expect(screen.getByText('Downloads unavailable')).toBeInTheDocument();
    expect(screen.queryByText('Source available')).not.toBeInTheDocument();
    expect(screen.getByLabelText('Book count')).toHaveTextContent('0books');
    await screen.findByRole('heading', { name: 'Nothing found' });
    expect(screen.getByText('Try a different search or clear the active filters.')).toBeInTheDocument();

    await user.click(screen.getByRole('button', { name: 'Settings' }));
    const settings = screen.getByRole('dialog', { name: 'Catalog information' });
    expect(settings).toHaveTextContent('Not configured');
    expect(screen.getByRole('button', { name: 'Rescan' })).toBeDisabled();

    queryClient.clear();
  });

  it('keeps the details dialog open with retry when the details endpoint fails', async () => {
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    vi.spyOn(apiClient, 'getStatus').mockResolvedValue({
      ...mockStatus,
      scan: { active: false, status: 'idle' }
    });
    vi.spyOn(apiClient, 'getSource').mockResolvedValue(mockSource);
    vi.spyOn(apiClient, 'getScanProgress').mockResolvedValue({ active: false, status: 'idle' });
    vi.spyOn(apiClient, 'getStats').mockResolvedValue(mockStats);
    vi.spyOn(apiClient, 'getCover').mockResolvedValue(new Blob(['cover'], { type: 'image/png' }));
    vi.spyOn(apiClient, 'getBooks').mockResolvedValue(mockBookList);
    const getDetails = vi.spyOn(apiClient, 'getBookDetails').mockRejectedValue(
      new ApiError(500, 'details_failed', 'Details failed.')
    );

    render(
      <QueryClientProvider client={queryClient}>
        <App />
      </QueryClientProvider>
    );

    await screen.findByRole('heading', { name: 'InpxWebReader' });
    fireEvent.click(await screen.findByRole('button', { name: /Test Book/i }));

    expect(await screen.findByRole('heading', { name: 'Could not load details' })).toBeInTheDocument();
    expect(screen.getByRole('alert')).toHaveTextContent('Details failed.');

    fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
    await waitFor(() => expect(getDetails).toHaveBeenCalledTimes(2));

    queryClient.clear();
  });

  it('activates the details modal lifecycle and restores focus to the selected card', async () => {
    const user = userEvent.setup();
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    vi.spyOn(apiClient, 'getStatus').mockResolvedValue({
      ...mockStatus,
      scan: { active: false, status: 'idle' }
    });
    vi.spyOn(apiClient, 'getSource').mockResolvedValue(mockSource);
    vi.spyOn(apiClient, 'getStats').mockResolvedValue(mockStats);
    vi.spyOn(apiClient, 'getBooks').mockResolvedValue(mockBookList);
    vi.spyOn(apiClient, 'getBookDetails').mockResolvedValue({ book: mockDetails });
    vi.spyOn(apiClient, 'getCover').mockResolvedValue(new Blob(['cover'], { type: 'image/png' }));

    render(
      <QueryClientProvider client={queryClient}>
        <App />
      </QueryClientProvider>
    );

    const bookButton = await screen.findByRole('button', { name: /Test Book/i });
    await user.click(bookButton);

    const closeButton = await screen.findByRole('button', { name: 'Close details' });
    await waitFor(() => expect(closeButton).toHaveFocus());
    expect(document.querySelector('.top-bar')).toHaveAttribute('aria-hidden', 'true');

    await user.tab({ shift: true });
    expect(screen.getByRole('button', { name: 'Original' })).toHaveFocus();
    await user.tab();
    expect(closeButton).toHaveFocus();

    await user.keyboard('{Escape}');
    await waitFor(() => expect(screen.queryByRole('dialog', { name: 'Book details' })).not.toBeInTheDocument());
    expect(bookButton).toHaveFocus();
    expect(document.querySelector('.top-bar')).not.toHaveAttribute('aria-hidden');

    queryClient.clear();
  });

  it('keeps open details stable when a pending debounced search commits', async () => {
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    vi.spyOn(apiClient, 'getStatus').mockResolvedValue({
      ...mockStatus,
      scan: { active: false, status: 'idle' }
    });
    vi.spyOn(apiClient, 'getSource').mockResolvedValue(mockSource);
    vi.spyOn(apiClient, 'getStats').mockResolvedValue(mockStats);
    const getBooks = vi.spyOn(apiClient, 'getBooks').mockResolvedValue(mockBookList);
    vi.spyOn(apiClient, 'getBookDetails').mockResolvedValue({ book: mockDetails });
    vi.spyOn(apiClient, 'getCover').mockResolvedValue(new Blob(['cover'], { type: 'image/png' }));

    render(
      <QueryClientProvider client={queryClient}>
        <App />
      </QueryClientProvider>
    );

    const bookButton = await screen.findByRole('button', { name: /Test Book/i });
    fireEvent.change(screen.getByLabelText('Search catalog'), {
      target: { value: 'Test' }
    });
    fireEvent.click(bookButton);
    expect(await screen.findByRole('dialog', { name: 'Book details' })).toBeInTheDocument();

    await waitFor(() => {
      expect(getBooks.mock.calls.some(([request]) => request.text === 'Test')).toBe(true);
    });
    expect(screen.getByRole('dialog', { name: 'Book details' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Original' })).toBeEnabled();

    queryClient.clear();
  });

  it('refreshes open details when status polling reports the source unavailable', async () => {
    const user = userEvent.setup();
    const queryClient = new QueryClient({
      defaultOptions: {
        queries: { retry: false },
        mutations: { retry: false }
      }
    });
    let sourceUnavailable = false;
    vi.spyOn(apiClient, 'getStatus').mockResolvedValue({
      ...mockStatus,
      scan: { active: false, status: 'idle' }
    });
    vi.spyOn(apiClient, 'getSource').mockResolvedValue(mockSource);
    vi.spyOn(apiClient, 'getStats').mockResolvedValue(mockStats);
    vi.spyOn(apiClient, 'getBooks').mockImplementation(async () => ({
      ...mockBookList,
      items: [{
        ...mockBook,
        actions: {
          ...mockBook.actions,
          canDownloadOriginal: !sourceUnavailable
        }
      }]
    }));
    const getDetails = vi.spyOn(apiClient, 'getBookDetails').mockImplementation(async () => ({
      book: {
        ...mockDetails,
        actions: {
          ...mockDetails.actions,
          canDownloadOriginal: !sourceUnavailable
        }
      }
    }));
    vi.spyOn(apiClient, 'getCover').mockResolvedValue(new Blob(['cover'], { type: 'image/png' }));

    render(
      <QueryClientProvider client={queryClient}>
        <App />
      </QueryClientProvider>
    );

    await user.click(await screen.findByRole('button', { name: /Test Book/i }));
    expect(await screen.findByRole('button', { name: 'Original' })).toBeEnabled();

    sourceUnavailable = true;
    act(() => {
      queryClient.setQueryData(['status', ''], {
        ...mockStatus,
        scan: { active: false, status: 'idle' },
        capabilities: {
          ...mockStatus.capabilities,
          canDownloadOriginal: false
        },
        inpxSource: {
          ...mockStatus.inpxSource!,
          available: false,
          sourceWarning: 'Source is unavailable.'
        }
      });
    });

    expect(await screen.findByText('Source unavailable')).toBeInTheDocument();
    await waitFor(() => expect(getDetails).toHaveBeenCalledTimes(2));
    expect(screen.getByRole('button', { name: 'Original' })).toBeDisabled();

    queryClient.clear();
  });

  it('submits toolbar search without losing selected filters', async () => {
    const onQueryChange = vi.fn();
    render(
      <SearchToolbar
        query={{ ...defaultBookQuery, language: 'en' }}
        languageFacets={[{ value: 'en', count: 7 }]}
        genreFacets={[]}
        busy={false}
        onQueryChange={onQueryChange}
      />
    );

    fireEvent.change(screen.getByLabelText('Search catalog'), {
      target: { value: 'history' }
    });

    await waitFor(() => {
      expect(onQueryChange).toHaveBeenCalledWith(expect.objectContaining({
        text: 'history',
        language: 'en',
        offset: 0
      }));
    });
  });

  it('keeps at least one search field selected', () => {
    const onQueryChange = vi.fn();
    render(
      <SearchToolbar
        query={{ ...defaultBookQuery, fields: ['title'] }}
        languageFacets={[]}
        genreFacets={[]}
        busy={false}
        onQueryChange={onQueryChange}
      />
    );

    fireEvent.click(screen.getAllByText('Title')[0]);
    expect(onQueryChange).not.toHaveBeenCalled();
  });

  it('opens mobile catalog tools in a bottom sheet instead of desktop dropdowns', () => {
    stubMobileCatalogMedia(true);
    render(
      <SearchToolbar
        query={defaultBookQuery}
        languageFacets={[{ value: 'en', count: 7 }]}
        genreFacets={[{ value: 'Science Fiction', count: 5 }]}
        busy={false}
        onQueryChange={vi.fn()}
      />
    );

    expect(screen.queryByText('No filters active')).not.toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: 'Filters' }));

    expect(screen.getByRole('dialog', { name: 'Catalog tools' })).toBeInTheDocument();
    expect(screen.getByRole('heading', { name: 'Languages' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'en (7)' })).toBeInTheDocument();
  });

  it('focuses modal controls and keeps focus stable when callbacks change', async () => {
    const onClose = vi.fn();
    const nextOnClose = vi.fn();
    const { rerender } = render(
      <>
        <button type="button">Before dialog</button>
        <SettingsDialog
          status={mockStatus}
          source={mockSource.source}
          statistics={mockStats}
          rescanBusy={false}
          onClose={onClose}
          onRescan={vi.fn()}
        />
      </>
    );

    const closeButton = screen.getByRole('button', { name: 'Close settings' });
    await waitFor(() => expect(closeButton).toHaveFocus());

    const rescanButton = screen.getByRole('button', { name: 'Rescan' });
    rescanButton.focus();

    rerender(
      <>
        <button type="button">Before dialog</button>
        <SettingsDialog
          status={mockStatus}
          source={mockSource.source}
          statistics={mockStats}
          rescanBusy={false}
          onClose={nextOnClose}
          onRescan={vi.fn()}
        />
      </>
    );

    expect(screen.getByRole('button', { name: 'Rescan' })).toHaveFocus();

    fireEvent.keyDown(document, { key: 'Escape' });
    expect(onClose).not.toHaveBeenCalled();
    expect(nextOnClose).toHaveBeenCalledTimes(1);
  });

  it('uses the first readable title character for cover placeholders', () => {
    render(<CoverPlaceholder title="(«Скажу, что жил я на одной из улиц...»)" authors="" />);

    expect(screen.getByText('С')).toBeInTheDocument();
  });

  it('auto-dismisses toast notifications', () => {
    vi.useFakeTimers();
    const onDismiss = vi.fn();

    render(
      <ToastRegion
        toasts={[{ id: 7, tone: 'success', text: 'Download started.' }]}
        onDismiss={onDismiss}
      />
    );

    vi.advanceTimersByTime(toastAutoDismissMs - 1);
    expect(onDismiss).not.toHaveBeenCalled();

    vi.advanceTimersByTime(1);
    expect(onDismiss).toHaveBeenCalledWith(7);
  });

  it('does not extend existing toast timers when new notifications arrive', () => {
    vi.useFakeTimers();
    const onDismiss = vi.fn();
    const { rerender } = render(
      <ToastRegion
        toasts={[{ id: 7, tone: 'success', text: 'Download started.' }]}
        onDismiss={onDismiss}
      />
    );

    vi.advanceTimersByTime(3000);
    rerender(
      <ToastRegion
        toasts={[
          { id: 7, tone: 'success', text: 'Download started.' },
          { id: 8, tone: 'error', text: 'Download failed.' }
        ]}
        onDismiss={onDismiss}
      />
    );

    vi.advanceTimersByTime(1499);
    expect(onDismiss).not.toHaveBeenCalled();

    vi.advanceTimersByTime(1);
    expect(onDismiss).toHaveBeenCalledTimes(1);
    expect(onDismiss).toHaveBeenLastCalledWith(7);

    vi.advanceTimersByTime(2999);
    expect(onDismiss).toHaveBeenCalledTimes(1);

    vi.advanceTimersByTime(1);
    expect(onDismiss).toHaveBeenCalledTimes(2);
    expect(onDismiss).toHaveBeenLastCalledWith(8);
  });

  it('adds file and iPhone Files guidance to download toasts', () => {
    expect(buildDownloadToastText(
      { fileName: 'Smoke Book.fb2' },
      {
        userAgent: 'Mozilla/5.0 (X11; Linux x86_64)',
        platform: 'Linux x86_64',
        maxTouchPoints: 0
      }
    )).toBe('Download requested: Smoke Book.fb2.');

    expect(buildDownloadToastText(
      { fileName: 'Smoke Book.fb2' },
      {
        userAgent: 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1',
        platform: 'iPhone',
        maxTouchPoints: 5
      }
    )).toBe('Download requested: Smoke Book.fb2. On iPhone or iPad, check Safari Downloads or Files > Browse > Downloads.');
  });

  it('keeps the generated download URL alive after clicking the browser download link', () => {
    vi.useFakeTimers();
    const createObjectUrl = vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:inpx-web-reader-download-test');
    const revokeObjectUrl = vi.spyOn(URL, 'revokeObjectURL').mockImplementation(() => undefined);
    const click = vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(() => undefined);

    triggerBrowserDownload(
      { blob: new Blob(['book'], { type: 'application/octet-stream' }), fileName: 'Smoke Book.fb2' },
      1000
    );

    expect(createObjectUrl).toHaveBeenCalledTimes(1);
    expect(click).toHaveBeenCalledTimes(1);
    expect(revokeObjectUrl).not.toHaveBeenCalled();
    expect(document.querySelector('a[href="blob:inpx-web-reader-download-test"]')).not.toBeInTheDocument();

    vi.advanceTimersByTime(999);
    expect(revokeObjectUrl).not.toHaveBeenCalled();

    vi.advanceTimersByTime(1);
    expect(revokeObjectUrl).toHaveBeenCalledWith('blob:inpx-web-reader-download-test');
  });

  it('shows no-results guidance without a rescan action', () => {
    const { rerender } = render(
      <BookGrid
        bookPages={[]}
        totalCount={0}
        loading={false}
        loadingMore={false}
        hasMore={false}
        hasActiveQuery={true}
        selectedId={null}
        onSelectBook={vi.fn()}
        onLoadMore={vi.fn()}
      />
    );

    expect(screen.getByRole('heading', { name: 'Nothing found' })).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Rescan' })).not.toBeInTheDocument();

    rerender(
      <BookGrid
        bookPages={[]}
        totalCount={0}
        loading={false}
        loadingMore={false}
        hasMore={false}
        hasActiveQuery={false}
        selectedId={null}
        onSelectBook={vi.fn()}
        onLoadMore={vi.fn()}
      />
    );

    expect(screen.getByRole('heading', { name: 'No books in catalog' })).toBeInTheDocument();
    expect(screen.getByText('The cached catalog does not contain any books yet.')).toBeInTheDocument();
    expect(screen.queryByText(/rescan/i)).not.toBeInTheDocument();
  });

  it('gates detail actions from DTO flags', () => {
    render(
      <BookDetailsDrawer
        book={mockDetails}
        loading={false}
        token=""
        onClose={vi.fn()}
        onDownloadOriginal={vi.fn()}
        onDownloadEpub={vi.fn()}
      />
    );

    const dialog = screen.getByRole('dialog', { name: 'Book details' });
    expect(dialog).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Original' })).toBeEnabled();
    expect(screen.getByRole('button', { name: 'EPUB' })).toBeDisabled();
    expect(screen.getByRole('button', { name: 'EPUB' })).toHaveAttribute('title', 'EPUB download is unavailable.');
  });

  it('invokes the enabled EPUB download contract', () => {
    const onDownloadEpub = vi.fn();
    const epubDetails = {
      ...mockDetails,
      epubDownloadUrl: '/api/books/7/download?format=epub',
      actions: {
        ...mockDetails.actions,
        canDownloadAsEpub: true
      }
    };
    render(
      <BookDetailsDrawer
        book={epubDetails}
        loading={false}
        token=""
        onClose={vi.fn()}
        onDownloadOriginal={vi.fn()}
        onDownloadEpub={onDownloadEpub}
      />
    );

    const epubButton = screen.getByRole('button', { name: 'EPUB' });
    expect(epubButton).toBeEnabled();
    fireEvent.click(epubButton);
    expect(onDownloadEpub).toHaveBeenCalledWith(epubDetails);
  });

  it('renders startup scan progress and gates continue until terminal state', () => {
    const onContinue = vi.fn();
    const onCancel = vi.fn();
    const { rerender } = render(
      <StartupScanDialog
        scan={activeScan}
        starting={false}
        onContinue={onContinue}
        onCancel={onCancel}
      />
    );

    expect(screen.getByText('42% complete')).toBeInTheDocument();
    expect(screen.getByRole('status', { name: 'Scan progress status' })).toHaveTextContent('42% complete');
    expect(screen.getByRole('progressbar', { name: 'Scan completion' })).toHaveAttribute('aria-valuenow', '42');
    expect(screen.getByRole('progressbar', { name: 'Scan completion' })).toHaveAttribute(
      'aria-valuetext',
      'Scanning archive. 42% complete.'
    );
    expect(screen.getByRole('button', { name: 'Continue' })).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: 'Cancel scan' }));
    expect(onCancel).toHaveBeenCalledTimes(1);

    rerender(
      <StartupScanDialog
        scan={{ ...activeScan, active: false, status: 'completed' }}
        starting={false}
        onContinue={onContinue}
        onCancel={onCancel}
      />
    );

    expect(screen.getByRole('status', { name: 'Scan progress status' })).toHaveTextContent('Scan completed');
    expect(screen.getByRole('progressbar', { name: 'Scan completion' })).toHaveAttribute('aria-valuenow', '100');
    fireEvent.click(screen.getByRole('button', { name: 'Continue' }));
    expect(onContinue).toHaveBeenCalledTimes(1);
  });

  it('opens a rendered book card from the virtual grid', () => {
    const onSelect = vi.fn();
    render(
      <BookGrid
        bookPages={[{ offset: 0, items: [mockBook] }]}
        totalCount={1}
        loading={false}
        loadingMore={false}
        hasMore={false}
        hasActiveQuery={false}
        selectedId={null}
        onSelectBook={onSelect}
        onLoadMore={vi.fn()}
      />
    );

    fireEvent.click(screen.getByRole('button', { name: /Test Book/i }));
    expect(onSelect).toHaveBeenCalledWith(mockBook);
  });

  it('resolves visible rows directly from immutable cursor page chunks', () => {
    render(
      <BookGrid
        bookPages={[
          { offset: 0, items: [{ ...mockBook, id: 70, title: 'First cursor chunk' }] },
          { offset: 1, items: [{ ...mockBook, id: 71, title: 'Middle cursor chunk' }] },
          { offset: 2, items: [{ ...mockBook, id: 72, title: 'Last cursor chunk' }] }
        ]}
        totalCount={3}
        loading={false}
        loadingMore={false}
        hasMore={false}
        hasActiveQuery={false}
        selectedId={null}
        onSelectBook={vi.fn()}
        onLoadMore={vi.fn()}
      />
    );

    expect(screen.getByText('First cursor chunk')).toBeInTheDocument();
    expect(screen.getByText('Middle cursor chunk')).toBeInTheDocument();
    expect(screen.getByText('Last cursor chunk')).toBeInTheDocument();
  });

  it('keeps visible-row work bounded for a large loaded catalog', () => {
    const range = resolveVisibleRowRange(1_000_000, 100, {
      scrollTop: 50_000_000,
      height: 800
    });

    expect(range.first).toBe(499_997);
    expect(range.last - range.first).toBe(14);
  });

  it('keeps the physical scroll window within browser height limits', () => {
    const firstWindow = resolveVirtualRowWindow(1_000_000, 398, 0);
    const lastWindow = resolveVirtualRowWindow(1_000_000, 398, 1_000_000);

    expect(firstWindow.first).toBe(0);
    expect(firstWindow.capacity * 398).toBeLessThanOrEqual(maxVirtualScrollHeightPx);
    expect(firstWindow.last).toBe(firstWindow.capacity);
    expect(lastWindow.last).toBe(1_000_000);
    expect(lastWindow.last - lastWindow.first).toBe(firstWindow.capacity);
  });

  it('rebases a large catalog before requesting another cursor page', async () => {
    const onLoadMore = vi.fn();
    const { container } = render(
      <BookGrid
        bookPages={[
          { offset: 0, items: [{ ...mockBook, id: 70, title: 'First sparse book' }] },
          { offset: 999_999, items: [{ ...mockBook, id: 71, title: 'Last sparse book' }] }
        ]}
        totalCount={1_000_100}
        loading={false}
        loadingMore={false}
        hasMore={true}
        hasActiveQuery={false}
        selectedId={null}
        onSelectBook={vi.fn()}
        onLoadMore={onLoadMore}
      />
    );

    const scrollContainer = container.querySelector<HTMLElement>('.book-grid-scroll');
    const scrollWindow = scrollContainer?.firstElementChild as HTMLElement | null;
    expect(scrollContainer).not.toBeNull();
    expect(scrollWindow).not.toBeNull();
    expect(Number.parseInt(scrollWindow?.style.height ?? '', 10))
      .toBeLessThanOrEqual(maxVirtualScrollHeightPx);

    Object.defineProperties(scrollContainer!, {
      clientHeight: { configurable: true, value: 800 },
      scrollHeight: { configurable: true, value: maxVirtualScrollHeightPx }
    });
    scrollContainer!.scrollTop = maxVirtualScrollHeightPx - 800;
    fireEvent.scroll(scrollContainer!);

    await waitFor(() => expect(Number(scrollWindow?.dataset.windowFirstRow)).toBeGreaterThan(0));
    expect(onLoadMore).not.toHaveBeenCalled();
    expect(scrollContainer!.scrollTop).toBeLessThan(maxVirtualScrollHeightPx - 800);

    await act(async () => Promise.resolve());
    scrollContainer!.scrollTop = 0;
    expect(scrollContainer!.scrollTop).toBe(0);
    fireEvent.scroll(scrollContainer!);

    await waitFor(() => expect(Number(scrollWindow?.dataset.windowFirstRow)).toBe(0));
    expect(scrollContainer!.scrollTop).toBeGreaterThan(0);
    expect(onLoadMore).not.toHaveBeenCalled();
  });

  it('renders a dense mobile catalog list when the phone layout is active', () => {
    stubMobileCatalogMedia(true);
    const onSelect = vi.fn();
    const { container } = render(
      <BookGrid
        bookPages={[{
          offset: 0,
          items: [mockBook, { ...mockBook, id: 8, title: 'Second Test Book' }]
        }]}
        totalCount={2}
        loading={false}
        loadingMore={false}
        hasMore={false}
        hasActiveQuery={false}
        selectedId={null}
        onSelectBook={onSelect}
        onLoadMore={vi.fn()}
      />
    );

    expect(container.querySelectorAll('.mobile-book-row')).toHaveLength(2);
    expect(container.querySelector('.book-card')).not.toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: /Second Test Book/i }));
    expect(onSelect).toHaveBeenCalledWith(expect.objectContaining({ id: 8 }));
  });

  it('loads protected covers with the bearer token', async () => {
    const fetchMock = vi.fn().mockResolvedValue(
      new Response(new Blob(['cover'], { type: 'image/png' }), { status: 200 })
    );
    vi.stubGlobal('fetch', fetchMock);

    render(
      <BookGrid
        bookPages={[{ offset: 0, items: [mockBook] }]}
        totalCount={1}
        loading={false}
        loadingMore={false}
        hasMore={false}
        hasActiveQuery={false}
        token="secret"
        selectedId={null}
        onSelectBook={vi.fn()}
        onLoadMore={vi.fn()}
      />
    );

    await waitFor(() => expect(fetchMock).toHaveBeenCalledWith('/api/covers/7', expect.any(Object)));
    const coverCall = fetchMock.mock.calls.find(([url]) => url === '/api/covers/7');
    expect(coverCall).toBeDefined();
    const headers = coverCall?.[1].headers as Headers;
    expect(headers.get('Authorization')).toBe('Bearer secret');
  });

  it('requests more books when additional results are available', async () => {
    const onLoadMore = vi.fn();
    render(
      <BookGrid
        bookPages={[{ offset: 0, items: [mockBook] }]}
        totalCount={120}
        loading={false}
        loadingMore={false}
        hasMore={true}
        hasActiveQuery={false}
        selectedId={null}
        onSelectBook={vi.fn()}
        onLoadMore={onLoadMore}
      />
    );

    scrollCatalogToEnd();
    await waitFor(() => expect(onLoadMore).toHaveBeenCalledTimes(1));
  });

  it('shows settings information and starts rescan from the dialog', () => {
    const onRescan = vi.fn();
    render(
      <SettingsDialog
        status={mockStatus}
        source={mockSource.source}
        statistics={mockStatus.runtime.storage.cacheDatabasePresent ? {
          bookCount: 12,
          unavailableBookCount: 1,
          inpxSourceSizeBytes: 0,
          coverCacheSizeBytes: 0,
          databaseSizeBytes: 4096,
          totalCatalogSizeBytes: 4096
        } : undefined}
        rescanBusy={false}
        onClose={vi.fn()}
        onRescan={onRescan}
      />
    );

    expect(screen.getByRole('heading', { name: 'Catalog information' })).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Rescan' }));
    expect(onRescan).toHaveBeenCalledTimes(1);
  });

  it('distinguishes unavailable and changed source states in settings', () => {
    const { rerender } = render(
      <SettingsDialog
        status={mockStatus}
        source={{ ...mockSource.source!, available: false, requiresRescan: false }}
        statistics={mockStats}
        rescanBusy={false}
        onClose={vi.fn()}
        onRescan={vi.fn()}
      />
    );

    expect(screen.getByText('Status').closest('.info-row')).toHaveTextContent('StatusUnavailable');

    rerender(
      <SettingsDialog
        status={mockStatus}
        source={{ ...mockSource.source!, available: false, requiresRescan: true }}
        statistics={mockStats}
        rescanBusy={false}
        onClose={vi.fn()}
        onRescan={vi.fn()}
      />
    );

    expect(screen.getByText('Status').closest('.info-row')).toHaveTextContent('StatusChanged — rescan required');
  });

  it('disables settings rescan from the server capability', () => {
    const onRescan = vi.fn();
    render(
      <SettingsDialog
        status={{
          ...mockStatus,
          capabilities: {
            ...mockStatus.capabilities,
            canRescanInpxSource: false
          }
        }}
        source={mockSource.source}
        statistics={mockStats}
        rescanBusy={false}
        onClose={vi.fn()}
        onRescan={onRescan}
      />
    );

    const rescanButton = screen.getByRole('button', { name: 'Rescan' });
    expect(rescanButton).toBeDisabled();
    expect(rescanButton).toHaveAttribute(
      'title',
      'INPX source is not configured or unavailable.'
    );
    fireEvent.click(rescanButton);
    expect(onRescan).not.toHaveBeenCalled();
  });
});
