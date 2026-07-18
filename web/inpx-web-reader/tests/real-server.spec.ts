import { copyFileSync, mkdirSync, readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';

import { expect, test } from '@playwright/test';

const token = 'browser-server-contract-token';
const authorization = { Authorization: `Bearer ${token}` };
const initialTitle = 'Browser Server Contract Книга';
const updatedTitle = 'Browser Server Contract Книга — обновлена';
const addedTitle = 'Новая книга после рескана';

function getServerRoot() {
  const serverRoot = process.env.INPX_WEB_READER_WEB_E2E_SERVER_ROOT;
  if (!serverRoot) {
    throw new Error('Real-server fixture root was not provided to Playwright.');
  }
  return serverRoot;
}

function publishUpdatedSource() {
  const serverRoot = getServerRoot();
  const activeSource = join(serverRoot, 'source');
  const updatedSource = join(serverRoot, 'source-update');
  const activeArchives = join(activeSource, 'lib.rus.ec');
  const updatedArchives = join(updatedSource, 'lib.rus.ec');
  mkdirSync(activeArchives, { recursive: true });
  for (const archiveName of readdirSync(updatedArchives).filter((name) => name.endsWith('.zip'))) {
    copyFileSync(join(updatedArchives, archiveName), join(activeArchives, archiveName));
  }
  copyFileSync(join(updatedSource, 'catalog.inpx'), join(activeSource, 'catalog.inpx'));
}

test('built browser UI exercises authenticated catalog details downloads and rescan', async ({ page, request }) => {
  test.setTimeout(120000);

  const unauthorized = await request.get('/api/status');
  expect(unauthorized.status()).toBe(401);

  const startResponse = await request.post('/api/scan/start', {
    headers: authorization,
    data: { mode: 'initial', warningLimit: 5 }
  });
  expect([202, 409]).toContain(startResponse.status());

  await expect.poll(async () => {
    const response = await request.get('/api/scan/progress', { headers: authorization });
    expect(response.ok()).toBe(true);
    const progress = await response.json() as { active: boolean; status: string };
    return progress.active ? 'running' : progress.status;
  }, { timeout: 60000 }).toBe('completed');

  const initialCatalogResponse = await request.get('/api/books?offset=0&limit=10', { headers: authorization });
  expect(initialCatalogResponse.ok()).toBe(true);
  const initialCatalog = await initialCatalogResponse.json() as {
    catalogSnapshotId: string;
    items: Array<{ id: number; title: string }>;
  };
  expect(initialCatalog.catalogSnapshotId).not.toBe('');
  const serverBook = initialCatalog.items.find((item) => item.title === initialTitle);
  expect(serverBook).toBeDefined();

  await page.goto('/');
  await expect(page.getByRole('dialog', { name: 'Server access' })).toBeVisible();
  await page.getByLabel('Server access password').fill(token);
  await page.getByRole('button', { name: 'Unlock catalog' }).click();
  await expect(page.getByRole('heading', { name: 'InpxWebReader' })).toBeVisible();
  const book = page.getByText(initialTitle, { exact: true }).first();
  await expect(book).toBeVisible({ timeout: 30000 });

  await page.getByLabel('Search catalog').fill('Книга');
  await expect(book).toBeVisible();
  await page.locator('.book-card').filter({ hasText: initialTitle }).first().getByRole('button').click();
  const details = page.getByRole('dialog', { name: 'Book details' });
  await expect(details).toContainText('Real browser to server contract fixture.');

  const coverResponse = await request.get(`/api/covers/${serverBook!.id}`, { headers: authorization });
  expect(coverResponse.ok()).toBe(true);
  expect(coverResponse.headers()['content-type']).toContain('image/jpeg');

  const originalDownloadPromise = page.waitForEvent('download');
  await details.getByRole('button', { name: 'Original' }).click();
  const originalDownload = await originalDownloadPromise;
  expect(originalDownload.suggestedFilename()).toContain('.fb2');
  const originalDownloadPath = await originalDownload.path();
  expect(originalDownloadPath).not.toBeNull();
  expect(readFileSync(originalDownloadPath!)).toEqual(
    readFileSync(join(getServerRoot(), 'books', 'browser-server-contract.fb2'))
  );

  await expect(details.getByRole('button', { name: 'EPUB' })).toBeEnabled();
  const epubDownloadPromise = page.waitForEvent('download');
  await details.getByRole('button', { name: 'EPUB' }).click();
  const epubDownload = await epubDownloadPromise;
  expect(epubDownload.suggestedFilename()).toContain('.epub');
  const epubDownloadPath = await epubDownload.path();
  expect(epubDownloadPath).not.toBeNull();
  expect(readFileSync(epubDownloadPath!)).toEqual(
    readFileSync(join(getServerRoot(), 'books', 'browser-server-contract.fb2'))
  );

  await details.getByRole('button', { name: 'Close details' }).click();
  await page.getByRole('button', { name: 'Settings' }).click();
  await expect(page.getByLabel('Access password', { exact: true })).toHaveValue(token);
  await page.getByRole('button', { name: 'Show access password' }).click();
  await expect(page.getByLabel('Access password', { exact: true })).toHaveAttribute('type', 'text');
  const previousProgressResponse = await request.get('/api/scan/progress', { headers: authorization });
  expect(previousProgressResponse.ok()).toBe(true);
  const previousProgress = await previousProgressResponse.json() as { jobId?: number };
  await page.getByRole('button', { name: 'Rescan' }).click();
  await expect.poll(async () => {
    const response = await request.get('/api/scan/progress', { headers: authorization });
    expect(response.ok()).toBe(true);
    const progress = await response.json() as { active: boolean; jobId?: number; status: string };
    if (progress.jobId === previousProgress.jobId) {
      return 'previous';
    }
    return progress.active ? 'running' : progress.status;
  }, { timeout: 60000 }).toBe('completed');

  const rescanProgressResponse = await request.get('/api/scan/progress', { headers: authorization });
  expect(rescanProgressResponse.ok()).toBe(true);
  const rescanProgress = await rescanProgressResponse.json() as {
    result: {
      parsedFb2Records: number;
      reusedRecords: number;
      segmentsUnchanged: number;
      archivesOpened: number;
      archiveBytesRead: number;
    };
  };
  expect(rescanProgress.result.parsedFb2Records).toBe(0);
  expect(rescanProgress.result.reusedRecords).toBe(1);
  expect(rescanProgress.result.segmentsUnchanged).toBe(1);
  expect(rescanProgress.result.archivesOpened).toBe(0);
  expect(rescanProgress.result.archiveBytesRead).toBe(0);

  const unchangedDialog = page.getByRole('dialog', { name: 'Scan completed' });
  await expect(unchangedDialog).toBeVisible();
  await expect(unchangedDialog.locator('.scan-counts span').filter({ hasText: 'reused' })).toHaveText('1reused');
  await expect(unchangedDialog.locator('.scan-counts span').filter({ hasText: 'parsed' })).toHaveText('0parsed');
  await unchangedDialog.getByRole('button', { name: 'Continue' }).click();
  await expect(book).toBeVisible();

  const catalogResponse = await request.get('/api/books?offset=0&limit=10', { headers: authorization });
  expect(catalogResponse.ok()).toBe(true);
  const catalog = await catalogResponse.json() as {
    catalogSnapshotId: string;
    items: Array<{ title: string }>;
  };
  expect(catalog.catalogSnapshotId).not.toBe(initialCatalog.catalogSnapshotId);
  expect(catalog.items.some((item) => item.title === initialTitle)).toBe(true);

  publishUpdatedSource();
  await page.getByRole('button', { name: 'Settings' }).click();
  await page.getByRole('button', { name: 'Rescan' }).click();
  const updatedDialog = page.getByRole('dialog', { name: 'Scan completed' });
  await expect(updatedDialog).toBeVisible({ timeout: 60000 });
  await expect(updatedDialog.locator('.scan-counts span').filter({ hasText: 'total' })).toHaveText('2total');
  await expect(updatedDialog.locator('.scan-counts span').filter({ hasText: 'parsed' })).toHaveText('2parsed');
  await expect(updatedDialog.locator('.scan-counts span').filter({ hasText: 'added' })).toHaveText('1added');
  await expect(updatedDialog.locator('.scan-counts span').filter({ hasText: 'updated' })).toHaveText('1updated');
  await updatedDialog.getByRole('button', { name: 'Continue' }).click();

  await page.getByLabel('Search catalog').fill('обновлена');
  const updatedBook = page.getByText(updatedTitle, { exact: true }).first();
  await expect(updatedBook).toBeVisible();
  await page.locator('.book-card').filter({ hasText: updatedTitle }).first().getByRole('button').click();
  const updatedDetails = page.getByRole('dialog', { name: 'Book details' });
  await expect(updatedDetails).toContainText('metadata published by an incremental rescan');
  await updatedDetails.getByRole('button', { name: 'Close details' }).click();

  await page.getByLabel('Search catalog').fill('Новая');
  await expect(page.getByText(addedTitle, { exact: true }).first()).toBeVisible();
  await expect(page.getByLabel('Book count')).toHaveText('1books');

  const updatedCatalogResponse = await request.get('/api/books?offset=0&limit=10', { headers: authorization });
  expect(updatedCatalogResponse.ok()).toBe(true);
  const updatedCatalog = await updatedCatalogResponse.json() as {
    catalogSnapshotId: string;
    totalCount: number;
    items: Array<{ id: number; title: string }>;
  };
  expect(updatedCatalog.catalogSnapshotId).not.toBe(catalog.catalogSnapshotId);
  expect(updatedCatalog.totalCount).toBe(2);
  expect(updatedCatalog.items.map((item) => item.title)).toEqual(expect.arrayContaining([updatedTitle, addedTitle]));

  const updatedServerBook = updatedCatalog.items.find((item) => item.title === updatedTitle);
  expect(updatedServerBook).toBeDefined();
  expect(updatedServerBook!.id).toBe(serverBook!.id);
  const updatedDownload = await request.get(
    `/api/books/${updatedServerBook!.id}/download?format=original`,
    { headers: authorization }
  );
  expect(updatedDownload.ok()).toBe(true);
  expect(await updatedDownload.body()).toEqual(
    readFileSync(join(getServerRoot(), 'books-update', 'browser-server-contract.fb2'))
  );
});
