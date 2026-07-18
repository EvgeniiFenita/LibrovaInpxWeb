import type {
  ApiErrorBody,
  BookDetailsResponse,
  BookListResponse,
  BookQuery,
  DownloadResult,
  CatalogStatistics,
  ScanCancelResponse,
  ScanProgress,
  ScanStartResponse,
  ServerStatus,
  SourceResponse
} from './types';

export class ApiError extends Error {
  readonly status: number;
  readonly code: string;
  readonly requestId?: string;

  constructor(status: number, code: string, message: string, requestId?: string) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.code = code;
    this.requestId = requestId;
  }
}

export interface ApiRequestOptions {
  method?: 'GET' | 'POST';
  token?: string;
  body?: unknown;
  signal?: AbortSignal;
}

export const defaultBookQuery: BookQuery = {
  text: '',
  fields: ['title', 'authors', 'description'],
  language: '',
  genre: '',
  sort: 'title',
  direction: 'asc',
  offset: 0,
  limit: 60,
  includeFacets: true
};

function applyBearerToken(headers: Headers, token?: string) {
  if (token?.trim()) {
    headers.set('Authorization', `Bearer ${token.trim()}`);
  }
}

async function parseError(response: Response): Promise<ApiError> {
  let payload: ApiErrorBody | null = null;
  try {
    payload = (await response.json()) as ApiErrorBody;
  } catch {
    payload = null;
  }

  const error = payload?.error;
  return new ApiError(
    response.status,
    error?.code ?? 'http_error',
    error?.message ?? `Request failed with HTTP ${response.status}.`,
    error?.requestId ?? response.headers.get('X-Request-Id') ?? undefined
  );
}

export async function apiFetch<T>(path: string, options: ApiRequestOptions = {}): Promise<T> {
  const headers = new Headers();
  applyBearerToken(headers, options.token);
  if (options.body !== undefined) {
    headers.set('Content-Type', 'application/json');
  }

  const response = await fetch(path, {
    method: options.method ?? 'GET',
    headers,
    body: options.body === undefined ? undefined : JSON.stringify(options.body),
    signal: options.signal
  });

  if (!response.ok) {
    throw await parseError(response);
  }

  return (await response.json()) as T;
}

export function buildBookListPath(query: BookQuery, cursor?: string): string {
  const params = new URLSearchParams();
  if (query.text.trim()) {
    params.set('text', query.text.trim());
  }
  if (query.fields.length > 0 && query.fields.length < 3) {
    params.set('fields', query.fields.join(','));
  }
  if (query.language) {
    params.set('languages', query.language);
  }
  if (query.genre) {
    params.set('genres', query.genre);
  }
  params.set('sort', query.sort);
  params.set('direction', query.direction);
  if (cursor) {
    params.set('cursor', cursor);
  } else {
    params.set('offset', String(Math.max(0, query.offset)));
  }
  params.set('limit', String(Math.max(1, query.limit)));
  params.set('includeFacets', query.includeFacets ? 'true' : 'false');
  return `/api/books?${params.toString()}`;
}

function parseFileNameFromContentDisposition(value: string | null): string {
  if (!value) {
    return 'book';
  }

  const encoded = /filename\*=UTF-8''([^;]+)/i.exec(value);
  if (encoded?.[1]) {
    try {
      return decodeURIComponent(encoded[1]);
    } catch {
      return encoded[1];
    }
  }

  const plain = /filename="([^"]+)"/i.exec(value);
  return plain?.[1] ?? 'book';
}

async function downloadFile(path: string, token?: string): Promise<DownloadResult> {
  const headers = new Headers();
  applyBearerToken(headers, token);

  const response = await fetch(path, { headers });
  if (!response.ok) {
    throw await parseError(response);
  }

  return {
    blob: await response.blob(),
    fileName: parseFileNameFromContentDisposition(response.headers.get('Content-Disposition'))
  };
}

async function fetchBlob(path: string, token?: string): Promise<Blob> {
  const headers = new Headers();
  applyBearerToken(headers, token);

  const response = await fetch(path, { headers });
  if (!response.ok) {
    throw await parseError(response);
  }

  return response.blob();
}

export const apiClient = {
  getStatus: (token?: string) => apiFetch<ServerStatus>('/api/status', { token }),
  getSource: (token?: string) => apiFetch<SourceResponse>('/api/source', { token }),
  getBooks: (query: BookQuery, token?: string, signal?: AbortSignal, cursor?: string) => apiFetch<BookListResponse>(buildBookListPath(query, cursor), { token, signal }),
  getBookDetails: (id: number, token?: string, signal?: AbortSignal) => apiFetch<BookDetailsResponse>(`/api/books/${id}`, { token, signal }),
  getStats: (token?: string) => apiFetch<CatalogStatistics>('/api/stats', { token }),
  startScan: (token?: string) => apiFetch<ScanStartResponse>('/api/scan/start', {
    method: 'POST',
    token,
    body: { mode: 'rescan', warningLimit: 50 }
  }),
  getScanProgress: (token?: string) => apiFetch<ScanProgress>('/api/scan/progress', { token }),
  cancelScan: (token?: string) => apiFetch<ScanCancelResponse>('/api/scan/cancel', {
    method: 'POST',
    token
  }),
  getCover: (path: string, token?: string) => fetchBlob(path, token),
  downloadOriginal: (path: string, token?: string) => downloadFile(path, token),
  downloadEpub: (path: string, token?: string) => downloadFile(path, token)
};
