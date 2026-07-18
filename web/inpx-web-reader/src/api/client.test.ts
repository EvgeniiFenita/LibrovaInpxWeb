import { describe, expect, it, vi } from 'vitest';

import { ApiError, apiClient, apiFetch, buildBookListPath, defaultBookQuery } from './client';

describe('api client', () => {
  it('maps catalog query state to server parameters', () => {
    const path = buildBookListPath({
      ...defaultBookQuery,
      text: 'Тестовая книга',
      fields: ['title', 'authors'],
      language: 'ru',
      genre: 'Science Fiction',
      sort: 'added',
      direction: 'desc',
      limit: 999
    });

    const params = new URLSearchParams(path.split('?')[1]);
    expect(path.startsWith('/api/books?')).toBe(true);
    expect(params.get('text')).toBe('Тестовая книга');
    expect(params.get('fields')).toBe('title,authors');
    expect(params.get('languages')).toBe('ru');
    expect(params.get('genres')).toBe('Science Fiction');
    expect(params.has('format')).toBe(false);
    expect(params.get('sort')).toBe('added');
    expect(params.get('direction')).toBe('desc');
    expect(params.get('offset')).toBe('0');
    expect(params.get('limit')).toBe('999');
  });

  it('uses an opaque cursor without repeating the offset parameter', () => {
    const path = buildBookListPath(defaultBookQuery, 'opaque-cursor');
    const params = new URLSearchParams(path.split('?')[1]);

    expect(params.get('cursor')).toBe('opaque-cursor');
    expect(params.has('offset')).toBe(false);
    expect(params.get('limit')).toBe('60');
  });

  it('sends bearer token and parses structured errors', async () => {
    const abortController = new AbortController();
    const fetchMock = vi.fn().mockResolvedValue(
      new Response(
        JSON.stringify({
          error: {
            code: 'unauthorized',
            message: 'Token required.',
            requestId: 'req-7'
          }
        }),
        {
          status: 401,
          headers: {
            'Content-Type': 'application/json',
            'X-Request-Id': 'req-7'
          }
        }
      )
    );
    vi.stubGlobal('fetch', fetchMock);

    await expect(apiFetch('/api/status', {
      token: 'secret',
      signal: abortController.signal
    })).rejects.toMatchObject({
      status: 401,
      code: 'unauthorized',
      requestId: 'req-7'
    });

    expect(fetchMock).toHaveBeenCalledWith('/api/status', expect.objectContaining({
      headers: expect.any(Headers)
    }));
    const headers = fetchMock.mock.calls[0][1].headers as Headers;
    expect(headers.get('Authorization')).toBe('Bearer secret');
    expect(fetchMock.mock.calls[0][1].signal).toBe(abortController.signal);
  });

  it('downloads files with authenticated fetch and filename parsing', async () => {
    const fetchMock = vi.fn().mockResolvedValue(
      new Response('payload', {
        status: 200,
        headers: {
          'Content-Disposition': "attachment; filename=\"book.fb2\"; filename*=UTF-8''%D0%9A%D0%BD%D0%B8%D0%B3%D0%B0.fb2"
        }
      })
    );
    vi.stubGlobal('fetch', fetchMock);

    const result = await apiClient.downloadOriginal('/api/books/7/download?format=original', 'secret');

    expect(result.fileName).toBe('Книга.fb2');
    expect(result.blob.size).toBeGreaterThan(0);
    const headers = fetchMock.mock.calls[0][1].headers as Headers;
    expect(headers.get('Authorization')).toBe('Bearer secret');
  });

  it('uses the shared authenticated download contract for EPUB files', async () => {
    const fetchMock = vi.fn().mockResolvedValue(
      new Response('epub-payload', {
        status: 200,
        headers: {
          'Content-Disposition': "attachment; filename=\"book.epub\"; filename*=UTF-8''%D0%9A%D0%BD%D0%B8%D0%B3%D0%B0.epub"
        }
      })
    );
    vi.stubGlobal('fetch', fetchMock);

    const result = await apiClient.downloadEpub('/api/books/7/download?format=epub', 'secret');

    expect(result.fileName).toBe('Книга.epub');
    const headers = fetchMock.mock.calls[0][1].headers as Headers;
    expect(headers.get('Authorization')).toBe('Bearer secret');
  });
});
