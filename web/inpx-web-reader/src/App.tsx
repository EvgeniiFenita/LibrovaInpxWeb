import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { Activity, AlertTriangle, HardDrive, KeyRound, Settings } from 'lucide-react';
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import brandBadgeUrl from './assets/brand_badge.png';
import { ApiError, apiClient, defaultBookQuery } from './api/client';
import type { BookDetails, BookListItem, BookQuery, DownloadResult, ScanProgress } from './api/types';
import {
  buildCatalogContextKey,
  resolveEffectiveSourceStatus,
  resolveSourceState,
  selectLatestScan,
  type SourceState
} from './catalogState';
import { BookDetailsDrawer } from './components/BookDetailsDrawer';
import { BookGrid } from './components/BookGrid';
import { SearchToolbar } from './components/SearchToolbar';
import { SettingsDialog } from './components/SettingsDialog';
import { StartupScanDialog } from './components/StartupScanDialog';
import { ToastMessage, ToastRegion } from './components/ToastRegion';
import { formatBytes } from './format';
import { useCursorBookPages } from './hooks/useCursorBookPages';
import { useModalA11y } from './hooks/useModalA11y';
import { usePersistentState } from './hooks/usePersistentState';

const tokenStorageKey = 'inpx-web-reader.access-token';
const downloadObjectUrlRevokeDelayMs = 60_000;
const sourceHeaderLabels: Record<SourceState, string> = {
  loading: 'Checking source',
  unconfigured: 'Source not configured',
  unavailable: 'Source unavailable',
  changed: 'Source changed',
  ready: 'Source available'
};

function parseSearchFields(value: string | null): BookQuery['fields'] {
  if (!value) {
    return defaultBookQuery.fields;
  }

  const allowed = new Set<BookQuery['fields'][number]>(['title', 'authors', 'description']);
  const fields = value
    .split(',')
    .map((item) => item.trim())
    .filter((item): item is BookQuery['fields'][number] => allowed.has(item as BookQuery['fields'][number]));
  return fields.length > 0 ? fields : defaultBookQuery.fields;
}

function parseSort(value: string | null): BookQuery['sort'] {
  switch (value) {
    case 'title':
    case 'authors':
    case 'added':
      return value;
    default:
      return defaultBookQuery.sort;
  }
}

function parseDirection(value: string | null): BookQuery['direction'] {
  return value === 'desc' ? 'desc' : defaultBookQuery.direction;
}

function readInitialQuery(): BookQuery {
  const params = new URLSearchParams(window.location.search);
  return {
    ...defaultBookQuery,
    text: params.get('text') ?? defaultBookQuery.text,
    fields: parseSearchFields(params.get('fields')),
    language: params.get('language') ?? defaultBookQuery.language,
    genre: params.get('genre') ?? defaultBookQuery.genre,
    sort: parseSort(params.get('sort')),
    direction: parseDirection(params.get('direction'))
  };
}

function writeQueryToUrl(query: BookQuery) {
  const params = new URLSearchParams();
  const defaultFields = defaultBookQuery.fields.join(',');
  for (const [key, value] of Object.entries({
    text: query.text,
    fields: query.fields.join(',') === defaultFields ? '' : query.fields.join(','),
    language: query.language,
    genre: query.genre,
    sort: query.sort === defaultBookQuery.sort ? '' : query.sort,
    direction: query.direction === defaultBookQuery.direction ? '' : query.direction
  })) {
    if (value) {
      params.set(key, value);
    }
  }

  const suffix = params.toString();
  window.history.replaceState(null, '', suffix ? `?${suffix}` : window.location.pathname);
}

export function triggerBrowserDownload(
  result: DownloadResult,
  revokeDelayMs = downloadObjectUrlRevokeDelayMs
) {
  const url = URL.createObjectURL(result.blob);
  const anchor = document.createElement('a');
  anchor.href = url;
  anchor.download = result.fileName;
  anchor.rel = 'noopener';

  try {
    document.body.append(anchor);
    anchor.click();
  } finally {
    anchor.remove();
    window.setTimeout(() => URL.revokeObjectURL(url), revokeDelayMs);
  }
}

interface BrowserPlatform {
  userAgent: string;
  platform?: string;
  maxTouchPoints?: number;
}

function readBrowserPlatform(): BrowserPlatform {
  return {
    userAgent: window.navigator.userAgent,
    platform: window.navigator.platform,
    maxTouchPoints: window.navigator.maxTouchPoints
  };
}

function isAppleMobileBrowser(platform: BrowserPlatform) {
  return /\b(iPhone|iPad|iPod)\b/i.test(platform.userAgent)
    || (platform.platform === 'MacIntel' && (platform.maxTouchPoints ?? 0) > 1);
}

export function buildDownloadToastText(
  result: Pick<DownloadResult, 'fileName'>,
  platform: BrowserPlatform = readBrowserPlatform()
) {
  const fileName = result.fileName.trim() || 'book';
  const text = `Download requested: ${fileName}.`;
  return isAppleMobileBrowser(platform)
    ? `${text} On iPhone or iPad, check Safari Downloads or Files > Browse > Downloads.`
    : text;
}

function errorText(error: unknown) {
  if (error instanceof ApiError) {
    return error.message;
  }
  if (error instanceof Error) {
    return error.message;
  }
  return 'Unexpected error.';
}

function hasActiveCatalogQuery(query: BookQuery) {
  return Boolean(
    query.text.trim()
      || query.language
      || query.genre
      || query.fields.join(',') !== defaultBookQuery.fields.join(',')
  );
}

function AuthGate({
  token,
  onTokenSubmit
}: {
  token: string;
  onTokenSubmit: (value: string) => void;
}) {
  const [draftToken, setDraftToken] = useState(token);
  const modalRef = useModalA11y<HTMLElement>();
  return (
    <main className="startup-stage" aria-label="Server access" data-modal-root>
      <section
        ref={modalRef}
        className="auth-dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="auth-title"
        tabIndex={-1}
      >
        <img src={brandBadgeUrl} alt="" className="auth-logo" />
        <div>
          <span className="eyebrow">InpxWebReader Server</span>
          <h1 id="auth-title">Server access</h1>
          <p>Enter the access password configured for this server.</p>
        </div>
        <form
          className="auth-form"
          onSubmit={(event) => {
            event.preventDefault();
            onTokenSubmit(draftToken.trim());
          }}
        >
          <label className="auth-token-field">
            <KeyRound aria-hidden="true" size={17} />
            <input
              type="password"
              value={draftToken}
              onChange={(event) => setDraftToken(event.target.value)}
              placeholder="Access password"
              aria-label="Server access password"
              autoComplete="current-password"
            />
          </label>
          <button
            type="submit"
            className="primary-button"
            disabled={!draftToken.trim() || draftToken.trim() === token.trim()}
          >
            Unlock catalog
          </button>
        </form>
      </section>
    </main>
  );
}

export function App() {
  const queryClient = useQueryClient();
  const [token, setToken] = usePersistentState(tokenStorageKey, '');
  const [query, setQuery] = useState(readInitialQuery);
  const [selectedBookId, setSelectedBookId] = useState<number | null>(null);
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [startupDismissed, setStartupDismissed] = useState(false);
  const [terminalScan, setTerminalScan] = useState<ScanProgress | null>(null);
  const [toasts, setToasts] = useState<ToastMessage[]>([]);
  const [authBlockedToken, setAuthBlockedToken] = useState<string | null>(null);
  const previousScanRef = useRef<Pick<ScanProgress, 'active' | 'status' | 'jobId'> | null>(null);
  const reportedScanErrorRef = useRef<string | null>(null);
  const reportedDetailsErrorRef = useRef<string | null>(null);
  const startupStartAttemptedRef = useRef(false);
  const authBlocked = authBlockedToken === token;

  const blockTokenOnAuthError = useCallback((requestToken: string, error: unknown) => {
    if (error instanceof ApiError && error.status === 401) {
      setAuthBlockedToken(requestToken);
    }
  }, []);

  useEffect(() => {
    writeQueryToUrl(query);
  }, [query]);

  const pushToast = useCallback((tone: ToastMessage['tone'], text: string) => {
    const id = Date.now() + Math.floor(Math.random() * 1000);
    setToasts((current) => [...current.slice(-3), { id, tone, text }]);
  }, []);

  const statusQuery = useQuery({
    queryKey: ['status', token],
    queryFn: async () => {
      try {
        return await apiClient.getStatus(token);
      } catch (error) {
        blockTokenOnAuthError(token, error);
        throw error;
      }
    },
    enabled: !authBlocked,
    refetchInterval: (data) => {
      if (authBlocked) {
        return false;
      }
      const status = data.state.data;
      return !startupDismissed && (!status || status.status !== 'open') ? 1000 : 15000;
    },
    retry: false
  });

  const sourceQuery = useQuery({
    queryKey: ['source', token],
    queryFn: async () => {
      try {
        return await apiClient.getSource(token);
      } catch (error) {
        blockTokenOnAuthError(token, error);
        throw error;
      }
    },
    enabled: !authBlocked,
    refetchInterval: 30000,
    retry: false
  });

  const shouldPollScanProgress = Boolean(
    !authBlocked
      && (
        statusQuery.data?.scan?.active
        || previousScanRef.current?.active
        || startupStartAttemptedRef.current
      )
  );

  const scanQuery = useQuery({
    queryKey: ['scan', token],
    queryFn: async () => {
      try {
        return await apiClient.getScanProgress(token);
      } catch (error) {
        blockTokenOnAuthError(token, error);
        throw error;
      }
    },
    enabled: shouldPollScanProgress,
    refetchInterval: (data) => {
      if (authBlocked) {
        return false;
      }
      return data.state.data?.active ? 1000 : false;
    },
    retry: false
  });
  const authRequired = authBlocked;
  const source = sourceQuery.data?.source ?? null;
  const effectiveSourceStatus = resolveEffectiveSourceStatus(
    source,
    sourceQuery.isSuccess,
    statusQuery.data?.inpxSource
  );
  const sourceResolved = sourceQuery.isSuccess || Boolean(statusQuery.data?.inpxSource);
  const sourceState = resolveSourceState(effectiveSourceStatus, sourceResolved);
  const catalogContextKey = buildCatalogContextKey(
    source,
    sourceState,
    statusQuery.data?.capabilities
  );
  const scan = selectLatestScan(statusQuery.data?.scan, scanQuery.data);

  const booksQuery = useCursorBookPages({
    catalogContextKey,
    query,
    token,
    enabled: startupDismissed && !authBlocked
  });

  const statsQuery = useQuery({
    queryKey: ['stats', catalogContextKey, token],
    queryFn: () => apiClient.getStats(token),
    enabled: (startupDismissed || settingsOpen) && !authBlocked,
    refetchInterval: 45000,
    retry: false
  });

  const detailsQuery = useQuery({
    queryKey: ['book-details', catalogContextKey, selectedBookId, token],
    queryFn: ({ signal }) => apiClient.getBookDetails(selectedBookId ?? 0, token, signal),
    enabled: selectedBookId !== null && !authBlocked,
    retry: false
  });

  const bookPages = booksQuery.pages;

  useEffect(() => {
    if (!(booksQuery.error instanceof ApiError)
      || booksQuery.error.code !== 'catalog_snapshot_changed') {
      return;
    }

    void queryClient.invalidateQueries({ queryKey: ['source'] });
    void queryClient.invalidateQueries({ queryKey: ['status'] });
  }, [booksQuery.error, queryClient]);

  const firstBookPage = bookPages[0];
  const lastFacetsRef = useRef({
    languages: firstBookPage?.facets?.languages ?? [],
    genres: firstBookPage?.facets?.genres ?? []
  });
  const facetsContextRef = useRef(catalogContextKey);

  if (facetsContextRef.current !== catalogContextKey) {
    facetsContextRef.current = catalogContextKey;
    lastFacetsRef.current = { languages: [], genres: [] };
  }

  if (firstBookPage?.facets) {
    lastFacetsRef.current = firstBookPage.facets;
  }

  const totalBookCount = firstBookPage?.totalCount ?? 0;
  const stats = statsQuery.data;

  const startScanMutation = useMutation({
    mutationFn: () => apiClient.startScan(token),
    onSuccess: (result) => {
      queryClient.setQueryData(['scan', token], result.scan);
      void queryClient.invalidateQueries({ queryKey: ['scan'] });
      void queryClient.invalidateQueries({ queryKey: ['status'] });
      void queryClient.invalidateQueries({ queryKey: ['source'] });
    },
    onError: (error) => {
      if (error instanceof ApiError && error.status === 409) {
        void scanQuery.refetch();
        return;
      }
      pushToast('error', errorText(error));
    }
  });

  const cancelScanMutation = useMutation({
    mutationFn: () => apiClient.cancelScan(token),
    onSuccess: (result) => {
      queryClient.setQueryData(['scan', token], result.scan);
      void queryClient.invalidateQueries({ queryKey: ['scan'] });
      void queryClient.invalidateQueries({ queryKey: ['status'] });
    },
    onError: (error) => pushToast('error', errorText(error))
  });

  useEffect(() => {
    if (
      authRequired
      || startupDismissed
      || startScanMutation.isPending
      || terminalScan
      || scan?.active
      || previousScanRef.current?.active
      || startupStartAttemptedRef.current
    ) {
      return;
    }
    if (statusQuery.isLoading || statusQuery.data?.status !== 'open') {
      return;
    }
    setStartupDismissed(true);
  }, [
    authRequired,
    scan,
    startupDismissed,
    startScanMutation.isPending,
    statusQuery.data?.status,
    statusQuery.isLoading,
    terminalScan
  ]);

  useEffect(() => {
    const current = scan;
    if (!current) {
      return;
    }

    const previous = previousScanRef.current;
    const terminalTransition = Boolean(previous?.active && !current.active);
    if (terminalTransition) {
      setTerminalScan(current);
      setStartupDismissed(false);
      void queryClient.resetQueries({ queryKey: ['books'] });
      void queryClient.resetQueries({ queryKey: ['book-details'] });
      void queryClient.invalidateQueries({ queryKey: ['stats'] });
      void queryClient.invalidateQueries({ queryKey: ['source'] });
      void queryClient.invalidateQueries({ queryKey: ['status'] });
    }

    if (!current.active && current.error?.message) {
      const errorKey = `${current.jobId ?? 'latest'}:${current.status}:${current.error.code}:${current.error.message}`;
      if (reportedScanErrorRef.current !== errorKey) {
        reportedScanErrorRef.current = errorKey;
        pushToast('error', current.error.message);
      }
    }

    if (current.active) {
      setTerminalScan(null);
      reportedScanErrorRef.current = null;
    }

    previousScanRef.current = {
      active: current.active,
      status: current.status,
      jobId: current.jobId
    };
  }, [queryClient, scan, pushToast]);

  useEffect(() => {
    if (selectedBookId === null || !detailsQuery.error) {
      reportedDetailsErrorRef.current = null;
      return;
    }

    const message = errorText(detailsQuery.error);
    const errorKey = `${selectedBookId}:${message}`;
    if (reportedDetailsErrorRef.current !== errorKey) {
      reportedDetailsErrorRef.current = errorKey;
      pushToast('error', message);
    }
  }, [detailsQuery.error, pushToast, selectedBookId]);

  const downloadMutation = useMutation({
    mutationFn: async ({ book, format }: { book: BookListItem | BookDetails; format: 'original' | 'epub' }) => {
      const path = format === 'epub' ? book.epubDownloadUrl : book.downloadUrl;
      if (!path) {
        throw new Error('Download is unavailable.');
      }
      return format === 'epub'
        ? apiClient.downloadEpub(path, token)
        : apiClient.downloadOriginal(path, token);
    },
    onSuccess: (result) => {
      triggerBrowserDownload(result);
      pushToast('success', buildDownloadToastText(result));
    },
    onError: (error) => pushToast('error', errorText(error))
  });

  const headerStats = useMemo(() => {
    const queriedBookCount = firstBookPage ? totalBookCount : undefined;
    const total = queriedBookCount
      ?? stats?.bookCount
      ?? statusQuery.data?.inpxSource?.totalBookCount
      ?? source?.totalBookCount
      ?? 0;
    const unavailable = stats?.unavailableBookCount ?? statusQuery.data?.inpxSource?.unavailableBookCount ?? source?.unavailableBookCount ?? 0;
    return { total, unavailable };
  }, [firstBookPage, source, stats, statusQuery.data, totalBookCount]);
  const catalogSizeLabel = stats ? formatBytes(stats.totalCatalogSizeBytes) : 'Catalog cache';
  const downloadModeLabel = statusQuery.data?.capabilities.canDownloadAsEpub
    ? 'EPUB download ready'
    : statusQuery.data?.capabilities.canDownloadOriginal
      ? 'Original downloads'
      : statusQuery.data
        ? 'Downloads unavailable'
        : 'Checking downloads';

  const updateCatalogQuery = (next: BookQuery) => {
    setQuery({ ...next, offset: 0 });
  };

  const continueFromStartup = () => {
    startupStartAttemptedRef.current = false;
    setTerminalScan(null);
    setStartupDismissed(true);
    startScanMutation.reset();
    cancelScanMutation.reset();
    void queryClient.invalidateQueries({ queryKey: ['books'] });
    void queryClient.invalidateQueries({ queryKey: ['stats'] });
    void queryClient.invalidateQueries({ queryKey: ['source'] });
  };

  const startRescan = () => {
    setSettingsOpen(false);
    setStartupDismissed(false);
    setTerminalScan(null);
    startScanMutation.reset();
    cancelScanMutation.reset();
    startupStartAttemptedRef.current = true;
    startScanMutation.mutate();
  };

  const toastRegion = (
    <ToastRegion
      toasts={toasts}
      onDismiss={(id) => setToasts((current) => current.filter((toast) => toast.id !== id))}
    />
  );

  if (authRequired) {
    return (
      <>
        <AuthGate token={token} onTokenSubmit={setToken} />
        {toastRegion}
      </>
    );
  }

  if (!startupDismissed) {
    const displayedScan = terminalScan ?? scan;
    const showStartupScanDialog = Boolean(
      terminalScan
        || displayedScan?.active
        || startScanMutation.isPending
        || startupStartAttemptedRef.current
    );
    if (!showStartupScanDialog) {
      return toastRegion;
    }

    return (
      <>
        <StartupScanDialog
          scan={displayedScan}
          sourceWarning={effectiveSourceStatus?.sourceWarning}
          starting={startScanMutation.isPending || (!displayedScan && !startScanMutation.isError)}
          startError={terminalScan || displayedScan?.active || !startScanMutation.error
            ? undefined
            : errorText(startScanMutation.error)}
          actionError={cancelScanMutation.error ? errorText(cancelScanMutation.error) : undefined}
          onContinue={continueFromStartup}
          onCancel={() => cancelScanMutation.mutate()}
          cancelling={cancelScanMutation.isPending}
        />
        {toastRegion}
      </>
    );
  }

  const details = detailsQuery.data?.book ?? null;
  const detailsError = detailsQuery.error ? errorText(detailsQuery.error) : null;

  return (
    <div className="app-shell">
      <header className="top-bar">
        <div className="top-bar-main">
          <div className="brand-lockup">
            <img src={brandBadgeUrl} alt="" className="brand-mark" />
            <div>
              <span className="eyebrow">InpxWebReader Server</span>
              <h1>InpxWebReader</h1>
              <p>{source?.displayName ?? (sourceState === 'unconfigured' ? 'Cached catalog' : 'INPX source')}</p>
            </div>
          </div>

          <div className="catalog-status-strip" aria-label="Catalog status">
            <span className={`status-chip ${sourceState === 'ready' ? 'ok' : sourceState === 'loading' ? '' : 'warning'}`}>
              <Activity aria-hidden="true" size={15} />
              {sourceHeaderLabels[sourceState]}
            </span>
            <span className="status-chip">
              <HardDrive aria-hidden="true" size={15} />
              {catalogSizeLabel}
            </span>
            <span className="status-chip">{downloadModeLabel}</span>
          </div>

          <div className="top-actions">
            <div className="book-count" aria-label="Book count">
              <strong>{headerStats.total.toLocaleString()}</strong>
              <span>books</span>
              {headerStats.unavailable > 0 && <em>{headerStats.unavailable.toLocaleString()} unavailable</em>}
            </div>
            <button type="button" className="icon-button" onClick={() => setSettingsOpen(true)} aria-label="Settings">
              <Settings aria-hidden="true" size={19} />
            </button>
          </div>
        </div>

        <SearchToolbar
          query={query}
          languageFacets={lastFacetsRef.current.languages}
          genreFacets={lastFacetsRef.current.genres}
          busy={booksQuery.isFetching}
          onQueryChange={updateCatalogQuery}
        />
      </header>

      <main className="workspace">
        {booksQuery.error && (
          <section className="inline-error" role="alert">
            <AlertTriangle aria-hidden="true" size={18} />
            <span>{errorText(booksQuery.error)}</span>
            <button
              type="button"
              className="secondary-button"
              disabled={booksQuery.isFetching}
              onClick={() => void booksQuery.retry()}
            >
              Retry catalog
            </button>
          </section>
        )}

        <BookGrid
          bookPages={bookPages}
          totalCount={totalBookCount}
          loading={booksQuery.isFetchingFirstPage}
          loadingMore={booksQuery.isFetchingNextPage}
          hasMore={booksQuery.hasNextPage}
          hasActiveQuery={hasActiveCatalogQuery(query)}
          hasError={Boolean(booksQuery.error)}
          token={token}
          selectedId={selectedBookId}
          onSelectBook={(book) => setSelectedBookId(book.id)}
          onLoadMore={() => {
            if (booksQuery.hasNextPage && !booksQuery.isFetchingNextPage) {
              void booksQuery.fetchNextPage();
            }
          }}
        />
      </main>

      {selectedBookId !== null && (
        <BookDetailsDrawer
          book={details}
          loading={detailsQuery.isFetching}
          error={detailsError}
          token={token}
          onClose={() => setSelectedBookId(null)}
          onRetry={() => {
            void detailsQuery.refetch();
          }}
          onDownloadOriginal={(book) => downloadMutation.mutate({ book, format: 'original' })}
          onDownloadEpub={(book) => downloadMutation.mutate({ book, format: 'epub' })}
        />
      )}

      {settingsOpen && (
        <SettingsDialog
          status={statusQuery.data}
          source={source}
          sourceState={sourceState}
          statistics={stats}
          rescanBusy={startScanMutation.isPending}
          accessPassword={token}
          onClose={() => setSettingsOpen(false)}
          onRescan={startRescan}
          onAccessPasswordChange={(value) => {
            setAuthBlockedToken(null);
            setToken(value);
            setSettingsOpen(false);
          }}
          onForgetAccessPassword={() => {
            setAuthBlockedToken(null);
            setToken('');
            setSettingsOpen(false);
          }}
        />
      )}

      {toastRegion}
    </div>
  );
}
