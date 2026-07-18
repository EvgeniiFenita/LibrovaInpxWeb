import { expect, Page, test } from '@playwright/test';

const transparentPng = Buffer.from(
  'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=',
  'base64'
);

const bookFixture = (id: number, title: string) => ({
  id,
  title,
  authors: ['Ada Reader'],
  language: 'en',
  seriesName: null,
  seriesIndex: null,
  year: 2026,
  tags: [],
  genres: ['Science Fiction'],
  format: 'fb2',
  sizeBytes: 2048,
  addedAtUtc: '2026-05-20T10:00:00Z',
  coverUrl: `/api/covers/${id}`,
  downloadUrl: `/api/books/${id}/download?format=original`,
  epubDownloadUrl: null,
  actions: {
    canDownloadOriginal: true,
    canDownloadAsEpub: false
  },
  isAvailable: true,
  availabilityLabel: 'Available'
});

async function routeApi(page: Page) {
  let searchRequests = 0;
  let downloadRequests = 0;
  let coverRequests = 0;

  await page.route('**/api/status', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({
        version: '1.2.0',
        status: 'open',
        capabilities: {
          canRescanInpxSource: true,
          canDownloadOriginal: true,
          canDownloadAsEpub: false
        },
        converter: { available: false, canDownloadAsEpub: false },
        scan: { active: false, status: 'idle' },
        inpxSource: {
          available: true,
          requiresRescan: false,
          sourceWarning: '',
          totalBookCount: 1,
          availableBookCount: 1,
          unavailableBookCount: 0,
          warningCount: 0
        }
      })
    });
  });

  await page.route('**/api/source', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({
        source: {
          inpxPath: '/source/catalog.inpx',
          archiveRoot: '/source',
          displayName: 'Smoke catalog',
          available: true,
          requiresRescan: false,
          sourceWarning: '',
          lastScanStartedAtUtc: '2026-05-20T09:00:00Z',
          lastScanCompletedAtUtc: '2026-05-20T09:01:00Z',
          lastSeenSnapshotId: 'fixture',
          totalBookCount: 1,
          availableBookCount: 1,
          unavailableBookCount: 0,
          warningCount: 0,
          recentWarnings: []
        }
      })
    });
  });

  await page.route('**/api/scan/progress', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({
        active: false,
        status: 'completed',
        result: {
          totalRecords: 1,
          scannedRecords: 1,
          parsedFb2Records: 1,
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
          archiveBytesRead: 512,
          warningCount: 0
        }
      })
    });
  });

  await page.route('**/api/scan/start', async (route) => {
    await route.fulfill({
      status: 202,
      contentType: 'application/json',
      body: JSON.stringify({
        jobId: 9,
        scan: {
          active: true,
          jobId: 9,
          status: 'running',
          percent: 15,
          message: 'Scanning archive',
          scannedRecords: 15,
          parsedFb2Records: 14,
          addedRecords: 10,
          updatedRecords: 4,
          warnings: []
        }
      })
    });
  });

  await page.route('**/api/stats', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({
        bookCount: 1,
        unavailableBookCount: 0,
        inpxSourceSizeBytes: 2048,
        coverCacheSizeBytes: 1024,
        databaseSizeBytes: 1024,
        totalCatalogSizeBytes: 4096
      })
    });
  });

  await page.route('**/api/books?**', async (route) => {
    searchRequests += 1;
    const baseTitle = searchRequests > 1 ? 'Filtered Smoke Book With Long Wrapped Title' : 'Smoke Book';
    const items = Array.from({ length: 8 }, (_, index) => bookFixture(
      7 + index,
      index === 0 ? baseTitle : `${baseTitle} ${index + 1}`
    ));
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({
        items,
        catalogSnapshotId: 'fixture',
        totalCount: items.length,
        offset: 0,
        limit: 60,
        nextCursor: null,
        facets: {
          languages: [
            'en',
            'ru',
            'uk',
            'de',
            'fr',
            'es',
            'it',
            'pl',
            'pt',
            'nl',
            'sv',
            'no',
            'fi',
            'cs',
            'tr',
            'ja'
          ].map((value, index) => ({
            value,
            count: index + 1
          })),
          genres: Array.from({ length: 24 }, (_, index) => ({
            value: `Genre ${index + 1}`,
            count: index + 1
          }))
        },
        statistics: {
          bookCount: items.length,
          unavailableBookCount: 0,
          inpxSourceSizeBytes: 2048,
          coverCacheSizeBytes: 1024,
          databaseSizeBytes: 1024,
          totalCatalogSizeBytes: 4096
        }
      })
    });
  });

  await page.route('**/api/books/7/download?format=original', async (route) => {
    downloadRequests += 1;
    await route.fulfill({
      status: 200,
      headers: {
        'Content-Disposition': "attachment; filename=\"Smoke_Book.fb2\"; filename*=UTF-8''Smoke%20Book.fb2"
      },
      body: 'download'
    });
  });

  await page.route('**/api/books/7', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({
        book: {
          id: 7,
          title: 'Smoke Book',
          authors: ['Ada Reader'],
          language: 'en',
          seriesName: null,
          seriesIndex: null,
          publisher: 'InpxWebReader Test Press',
          year: 2026,
          isbn: null,
          tags: [],
          genres: ['Science Fiction'],
          description: 'Smoke annotation',
          identifier: 'smoke-7',
          format: 'fb2',
          sizeBytes: 2048,
          addedAtUtc: '2026-05-20T10:00:00Z',
          coverUrl: '/api/covers/7',
          downloadUrl: '/api/books/7/download?format=original',
          epubDownloadUrl: null,
          actions: {
            canDownloadOriginal: true,
            canDownloadAsEpub: false
          },
          isAvailable: true,
          availabilityLabel: 'Available'
        }
      })
    });
  });

  await page.route('**/api/covers/**', async (route) => {
    coverRequests += 1;
    expect(route.request().headers().authorization).toBe('Bearer smoke-token');
    await route.fulfill({
      status: 200,
      contentType: 'image/png',
      body: transparentPng
    });
  });

  return {
    getDownloadRequests: () => downloadRequests,
    getCoverRequests: () => coverRequests
  };
}

test('catalog browser supports search details download and narrow layout', async ({ page }) => {
  const api = await routeApi(page);

  await page.addInitScript(() => {
    localStorage.setItem('inpx-web-reader.access-token', 'smoke-token');
  });
  await page.goto('/');
  await expect(page.locator('.startup-dialog')).toHaveCount(0);
  await expect(page.getByRole('heading', { name: 'InpxWebReader' })).toBeVisible();
  await expect(page.locator('.book-card').filter({ hasText: 'Smoke Book' }).first()).toBeVisible();
  await expect(page.locator('.book-card').first()).toBeVisible();
  await expect(page.locator('.mobile-book-row')).toHaveCount(0);
  await expect.poll(() => api.getCoverRequests()).toBeGreaterThan(0);

  await page.getByLabel('Search catalog').fill('filtered');
  await expect(page.locator('.book-card').filter({ hasText: 'Filtered Smoke Book' }).first()).toBeVisible();
  const cardTextLayout = await page.locator('.book-card').filter({ hasText: 'Filtered Smoke Book' }).first().evaluate((card) => {
    const main = card.querySelector<HTMLElement>('.book-card-main');
    const meta = card.querySelector<HTMLElement>('.book-card-meta');
    if (!main || !meta) {
      return { fits: false };
    }

    const mainBounds = main.getBoundingClientRect();
    const metaBounds = meta.getBoundingClientRect();
    return {
      fits: metaBounds.bottom <= mainBounds.bottom - 8
    };
  });
  expect(cardTextLayout.fits).toBe(true);

  await page.getByText('No filters active').click();
  const filtersPanel = page.locator('.filters-panel');
  await expect(filtersPanel).toBeVisible();
  const filterScrollState = await filtersPanel.evaluate((panel) => ({
    overflowY: getComputedStyle(panel).overflowY,
    scrolls: panel.scrollHeight > panel.clientHeight
  }));
  expect(filterScrollState).toEqual({ overflowY: 'visible', scrolls: false });
  const genreListScrolls = await page.locator('.facet-flow-scroll').evaluate(
    (list) => list.scrollHeight > list.clientHeight
  );
  expect(genreListScrolls).toBe(true);
  await page.keyboard.press('Escape');

  const selectedCard = page.locator('.book-card').filter({ hasText: 'Filtered Smoke Book' }).first().getByRole('button');
  await selectedCard.click();
  const details = page.getByRole('dialog', { name: 'Book details' });
  await expect(details).toContainText('Smoke annotation');
  await expect(details.getByRole('button', { name: 'EPUB' })).toBeDisabled();

  const closeDetails = details.getByRole('button', { name: 'Close details' });
  await expect(closeDetails).toBeFocused();
  await expect(page.locator('.top-bar')).toHaveAttribute('aria-hidden', 'true');
  await page.keyboard.press('Shift+Tab');
  await expect(details.getByRole('button', { name: 'Original' })).toBeFocused();
  await page.keyboard.press('Tab');
  await expect(closeDetails).toBeFocused();
  await page.keyboard.press('Escape');
  await expect(details).toHaveCount(0);
  await expect(selectedCard).toBeFocused();
  await expect(page.locator('.top-bar')).not.toHaveAttribute('aria-hidden', 'true');

  await selectedCard.click();
  await expect(details).toBeVisible();

  await details.getByRole('button', { name: 'Original' }).click();
  await expect.poll(() => api.getDownloadRequests()).toBe(1);
  await details.getByRole('button', { name: 'Close details' }).click();
  await expect(details).toHaveCount(0);

  await page.setViewportSize({ width: 390, height: 844 });
  await expect(page.locator('.app-shell')).toBeVisible();
  await expect(page.locator('.mobile-book-row').first()).toBeVisible();
  await expect(page.locator('.book-card')).toHaveCount(0);
  await expect(page.getByRole('button', { name: 'Filters' })).toBeVisible();
  await page.locator('.mobile-book-row').first().getByRole('button').click();
  const mobileDetails = page.getByRole('dialog', { name: 'Book details' });
  await expect(mobileDetails).toBeVisible();
  const mobileStatusLayout = await mobileDetails.locator('.details-title-row').evaluate((row) => {
    const pill = row.querySelector<HTMLElement>('.details-status-pill');
    const rowBounds = row.getBoundingClientRect();
    const pillBounds = pill?.getBoundingClientRect();
    return {
      pillWidth: pillBounds?.width ?? 0,
      rowWidth: rowBounds.width
    };
  });
  expect(mobileStatusLayout.pillWidth).toBeLessThan(mobileStatusLayout.rowWidth * 0.55);
  await mobileDetails.getByRole('button', { name: 'Close details' }).click();
  const topBarHeight = await page.locator('.top-bar').evaluate((element) => element.getBoundingClientRect().height);
  expect(topBarHeight).toBeLessThan(170);
  await page.getByRole('button', { name: 'Filters' }).click();
  const mobileTools = page.getByRole('dialog', { name: 'Catalog tools' });
  await expect(mobileTools).toBeVisible();
  await expect(mobileTools.getByRole('heading', { name: 'Languages' })).toBeVisible();
  const mobileFilterScrollState = await page.locator('.mobile-sheet').evaluate((sheet) => {
    const body = sheet.querySelector<HTMLElement>('.mobile-filters-body');
    const languages = sheet.querySelector<HTMLElement>('.mobile-filters-body .filter-group:first-of-type .facet-flow');
    const languageHeading = sheet.querySelector<HTMLElement>('.mobile-filters-body .filter-group:first-of-type h3');
    const genres = sheet.querySelector<HTMLElement>('.mobile-filters-body .facet-flow-scroll');
    const tabs = sheet.querySelector<HTMLElement>('.mobile-sheet-tabs');
    const footer = sheet.querySelector<HTMLElement>('.mobile-sheet-footer');
    const languageHeadingBounds = languageHeading?.getBoundingClientRect();
    const genreBounds = genres?.getBoundingClientRect();
    const tabsBounds = tabs?.getBoundingClientRect();
    const footerBounds = footer?.getBoundingClientRect();
    return {
      bodyScrolls: body ? body.scrollHeight > body.clientHeight + 1 : true,
      languageScrolls: languages ? languages.scrollHeight > languages.clientHeight + 1 : false,
      genreScrolls: genres ? genres.scrollHeight > genres.clientHeight + 1 : false,
      languageHeadingClearsTabs: Boolean(languageHeadingBounds && tabsBounds && languageHeadingBounds.top >= tabsBounds.bottom),
      genreClearsTabs: Boolean(genreBounds && tabsBounds && genreBounds.top >= tabsBounds.bottom),
      genreClearsFooter: Boolean(genreBounds && footerBounds && genreBounds.bottom <= footerBounds.top)
    };
  });
  expect(mobileFilterScrollState).toEqual({
    bodyScrolls: false,
    languageScrolls: true,
    genreScrolls: true,
    languageHeadingClearsTabs: true,
    genreClearsTabs: true,
    genreClearsFooter: true
  });
  await mobileTools.getByRole('button', { name: 'en (1)' }).click();
  await expect(mobileTools.getByRole('button', { name: 'Clear all' })).toBeVisible();
  const activeFilterLayout = await page.locator('.mobile-sheet').evaluate((sheet) => {
    const body = sheet.querySelector<HTMLElement>('.mobile-filters-body');
    const genres = sheet.querySelector<HTMLElement>('.mobile-filters-body .facet-flow-scroll');
    const filterFooter = sheet.querySelector<HTMLElement>('.filter-footer');
    const sheetFooter = sheet.querySelector<HTMLElement>('.mobile-sheet-footer');
    const genreBounds = genres?.getBoundingClientRect();
    const filterFooterBounds = filterFooter?.getBoundingClientRect();
    const sheetFooterBounds = sheetFooter?.getBoundingClientRect();
    return {
      bodyScrolls: body ? body.scrollHeight > body.clientHeight + 1 : true,
      genreScrolls: genres ? genres.scrollHeight > genres.clientHeight + 1 : false,
      filterFooterClearsGenres: Boolean(filterFooterBounds && genreBounds && filterFooterBounds.top >= genreBounds.bottom),
      filterFooterClearsDone: Boolean(filterFooterBounds && sheetFooterBounds && filterFooterBounds.bottom <= sheetFooterBounds.top)
    };
  });
  expect(activeFilterLayout).toEqual({
    bodyScrolls: false,
    genreScrolls: true,
    filterFooterClearsGenres: true,
    filterFooterClearsDone: true
  });
  await mobileTools.getByRole('button', { name: 'Done' }).click();
  await expect(page.locator('body')).not.toHaveJSProperty('scrollWidth', 0);
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth > document.documentElement.clientWidth);
  expect(overflow).toBe(false);
});

test('iPad-sized browser keeps the desktop catalog surface', async ({ page }) => {
  await routeApi(page);

  await page.addInitScript(() => {
    localStorage.setItem('inpx-web-reader.access-token', 'smoke-token');
  });
  await page.setViewportSize({ width: 820, height: 1180 });
  await page.goto('/');

  await expect(page.getByRole('heading', { name: 'InpxWebReader' })).toBeVisible();
  await expect(page.locator('.book-card').first()).toBeVisible();
  await expect(page.locator('.mobile-book-row')).toHaveCount(0);
  await expect(page.getByText('No filters active')).toBeVisible();
});

test('catalog pagination follows the opaque cursor and keeps the first-page summary', async ({ page }) => {
  await routeApi(page);
  await page.unroute('**/api/books?**');
  const requests: URL[] = [];
  await page.route('**/api/books?**', async (route) => {
    const requestUrl = new URL(route.request().url());
    requests.push(requestUrl);
    const cursor = requestUrl.searchParams.get('cursor');
    const continuation = cursor !== null;
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({
        items: [bookFixture(
          continuation ? 102 : 101,
          continuation ? 'Cursor second page' : 'Cursor first page'
        )],
        catalogSnapshotId: 'cursor-fixture',
        totalCount: continuation ? null : 2,
        offset: continuation ? 1 : 0,
        limit: 60,
        nextCursor: continuation ? null : 'opaque-continuation-token',
        facets: continuation ? null : {
          languages: [{ value: 'en', count: 2 }],
          genres: [{ value: 'Science Fiction', count: 2 }]
        }
      })
    });
  });

  await page.addInitScript(() => {
    localStorage.setItem('inpx-web-reader.access-token', 'smoke-token');
  });
  await page.goto('/');
  await expect(page.getByText('Cursor first page')).toBeVisible();
  await page.locator('.book-grid-scroll').evaluate((element) => {
    element.scrollTo(0, element.scrollHeight);
  });
  await expect(page.getByText('Cursor second page')).toBeVisible();
  await expect(page.getByLabel('Book count')).toContainText('2');

  const initialRequest = requests.find((request) => !request.searchParams.has('cursor'));
  const continuationRequest = requests.find(
    (request) => request.searchParams.get('cursor') === 'opaque-continuation-token'
  );
  expect(initialRequest?.searchParams.get('offset')).toBe('0');
  expect(continuationRequest).toBeDefined();
  expect(continuationRequest?.searchParams.has('offset')).toBe(false);
});

test('phone landscape uses the mobile catalog surface', async ({ browser }) => {
  const context = await browser.newContext({
    viewport: { width: 852, height: 393 },
    deviceScaleFactor: 3,
    isMobile: true,
    hasTouch: true,
    userAgent: 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1'
  });
  const page = await context.newPage();
  await routeApi(page);

  await page.addInitScript(() => {
    localStorage.setItem('inpx-web-reader.access-token', 'smoke-token');
  });
  await page.goto('/');

  await expect(page.locator('.mobile-book-row').first()).toBeVisible();
  await expect(page.getByRole('button', { name: 'Filters' })).toBeVisible();
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth > document.documentElement.clientWidth);
  expect(overflow).toBe(false);

  await context.close();
});

test('startup scan remains semantic and actionable in a short zoomed landscape viewport', async ({ page }) => {
  await routeApi(page);
  await page.unroute('**/api/status');
  await page.unroute('**/api/scan/progress');
  await page.route('**/api/status', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({
        version: '1.2.0',
        status: 'open',
        capabilities: {
          canRescanInpxSource: true,
          canDownloadOriginal: true,
          canDownloadAsEpub: false
        },
        converter: { available: false, canDownloadAsEpub: false },
        scan: { ...activeScanFixture(), jobId: 41 }
      })
    });
  });
  await page.route('**/api/scan/progress', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ ...activeScanFixture(), jobId: 41 })
    });
  });

  await page.addInitScript(() => {
    localStorage.setItem('inpx-web-reader.access-token', 'smoke-token');
  });
  await page.setViewportSize({ width: 852, height: 393 });
  await page.goto('/');
  await page.evaluate(() => {
    document.body.style.zoom = '2';
  });

  const dialog = page.locator('.startup-dialog');
  const progress = page.getByRole('progressbar', { name: 'Scan completion' });
  await expect(dialog).toBeVisible();
  await expect(progress).toHaveAttribute('aria-valuenow', '42');
  await expect(page.getByRole('status', { name: 'Scan progress status' })).toContainText('42% complete');
  expect(await dialog.evaluate((element) => getComputedStyle(element).overflowY)).toBe('auto');

  const cancel = page.getByRole('button', { name: 'Cancel scan' });
  await cancel.scrollIntoViewIfNeeded();
  await expect(cancel).toBeVisible();
  const cancelBounds = await cancel.boundingBox();
  expect(cancelBounds).not.toBeNull();
  expect(cancelBounds!.y + cancelBounds!.height).toBeLessThanOrEqual(393);
});

function activeScanFixture() {
  return {
    active: true,
    status: 'running',
    percent: 42,
    message: 'Scanning archive',
    totalRecords: 100,
    scannedRecords: 42,
    parsedFb2Records: 40,
    addedRecords: 21,
    updatedRecords: 19,
    markedUnavailableRecords: 0,
    skippedRecords: 2,
    warnings: ['One warning'],
    current: {
      archiveName: 'fb2-main.zip',
      entryName: 'book.fb2'
    }
  };
}
