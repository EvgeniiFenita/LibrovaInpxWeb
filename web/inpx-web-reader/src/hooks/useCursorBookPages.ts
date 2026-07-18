import { useQuery, useQueryClient } from '@tanstack/react-query';
import { useCallback, useEffect, useMemo, useState } from 'react';

import { ApiError, apiClient } from '../api/client';
import type { BookQuery } from '../api/types';
import { CatalogPageStore } from '../catalogPages';

interface CursorBookPagesOptions {
  catalogContextKey: string;
  query: BookQuery;
  token: string;
  enabled: boolean;
}

interface PaginationSession {
  active: boolean;
  controller: AbortController | null;
  loading: boolean;
  nextCursor: string | null;
  pageStore: CatalogPageStore;
}

interface ContinuationState {
  session: PaginationSession;
  loading: boolean;
  error: unknown;
}

function isAbortError(error: unknown) {
  return error instanceof Error && error.name === 'AbortError';
}

function isRecoverableCursorError(error: unknown) {
  return error instanceof ApiError
    && (error.code === 'catalog_snapshot_changed' || error.code === 'catalog_cursor_expired');
}

export function useCursorBookPages({
  catalogContextKey,
  query,
  token,
  enabled
}: CursorBookPagesOptions) {
  const queryClient = useQueryClient();
  const queryKey = useMemo(
    () => ['books', catalogContextKey, query, token] as const,
    [catalogContextKey, query, token]
  );
  const firstPageQuery = useQuery({
    queryKey,
    queryFn: ({ signal }) => apiClient.getBooks({ ...query, offset: 0 }, token, signal),
    enabled,
    staleTime: Infinity,
    retry: false
  });
  const session = useMemo<PaginationSession>(() => {
    const pageStore = new CatalogPageStore();
    pageStore.replaceFirstPage(firstPageQuery.data);
    return {
      active: true,
      controller: null,
      loading: false,
      nextCursor: firstPageQuery.data?.nextCursor ?? null,
      pageStore
    };
  }, [catalogContextKey, firstPageQuery.data, query, token]);
  const [, setPageRevision] = useState(0);
  const [continuationState, setContinuationState] = useState<ContinuationState | null>(null);

  useEffect(() => {
    session.active = true;
    setContinuationState((current) => (
      current?.session === session
        ? { ...current, loading: false }
        : { session, loading: false, error: null }
    ));
    return () => {
      session.active = false;
      session.controller?.abort();
      session.controller = null;
      session.loading = false;
    };
  }, [session]);

  const fetchNextPage = useCallback(async () => {
    const cursor = session.nextCursor;
    if (!session.active || session.loading || !cursor) {
      return;
    }

    const controller = new AbortController();
    session.controller = controller;
    session.loading = true;
    setContinuationState({ session, loading: true, error: null });

    try {
      const page = await apiClient.getBooks(
        { ...query, offset: 0 },
        token,
        controller.signal,
        cursor
      );
      if (!session.active || session.controller !== controller) {
        return;
      }
      if (!session.pageStore.appendContinuation(page)) {
        throw new ApiError(
          409,
          'catalog_snapshot_changed',
          'The catalog changed while more books were loading.'
        );
      }

      session.nextCursor = page.nextCursor;
      setPageRevision((revision) => revision + 1);
      setContinuationState({ session, loading: false, error: null });
    } catch (error) {
      if (!session.active || session.controller !== controller || isAbortError(error)) {
        return;
      }

      setContinuationState({ session, loading: false, error });
      if (isRecoverableCursorError(error)) {
        session.nextCursor = null;
        session.pageStore.replaceFirstPage();
        setPageRevision((revision) => revision + 1);
        void queryClient.resetQueries({ queryKey, exact: true });
      }
    } finally {
      if (session.controller === controller) {
        session.controller = null;
        session.loading = false;
      }
    }
  }, [query, queryClient, queryKey, session, token]);

  const currentContinuation = continuationState?.session === session
    ? continuationState
    : null;
  const continuationError = currentContinuation?.error ?? null;
  const error = continuationError ?? firstPageQuery.error;
  const retry = useCallback(async () => {
    if (continuationError && session.nextCursor) {
      await fetchNextPage();
      return;
    }
    await firstPageQuery.refetch();
  }, [continuationError, fetchNextPage, firstPageQuery.refetch, session]);

  return {
    pages: session.pageStore.pages,
    error,
    isFetching: firstPageQuery.isFetching || Boolean(currentContinuation?.loading),
    isFetchingFirstPage: firstPageQuery.isFetching,
    isFetchingNextPage: Boolean(currentContinuation?.loading),
    hasNextPage: Boolean(session.nextCursor) && !continuationError,
    fetchNextPage,
    retry
  };
}
