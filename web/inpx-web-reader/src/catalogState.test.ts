import { describe, expect, it } from 'vitest';

import type { InpxSourceOverview, ScanProgress } from './api/types';
import {
  buildCatalogContextKey,
  resolveEffectiveSourceStatus,
  resolveSourceState,
  selectLatestScan,
  sourceStateLabel
} from './catalogState';

const readySource: InpxSourceOverview = {
  available: true,
  requiresRescan: false,
  sourceWarning: '',
  totalBookCount: 1,
  availableBookCount: 1,
  unavailableBookCount: 0,
  warningCount: 0,
  inpxPath: '/source/catalog.inpx',
  archiveRoot: '/source',
  displayName: 'Catalog',
  lastScanStartedAtUtc: null,
  lastScanCompletedAtUtc: null,
  lastSeenSnapshotId: 'snapshot-7',
  recentWarnings: []
};

describe('catalog state', () => {
  it('represents every source availability state without optimistic defaults', () => {
    expect(resolveSourceState(undefined, false)).toBe('loading');
    expect(resolveSourceState(null, true)).toBe('unconfigured');
    expect(resolveSourceState({ ...readySource, available: false }, true)).toBe('unavailable');
    expect(resolveSourceState({ ...readySource, available: false, requiresRescan: true }, true)).toBe('changed');
    expect(resolveSourceState(readySource, true)).toBe('ready');

    expect(sourceStateLabel('loading')).toBe('Checking source');
    expect(sourceStateLabel('unconfigured')).toBe('Not configured');
    expect(sourceStateLabel('unavailable')).toBe('Unavailable');
    expect(sourceStateLabel('changed')).toBe('Changed — rescan required');
    expect(sourceStateLabel('ready')).toBe('Available');
  });

  it('uses either source endpoint to fail closed on stale or unsafe source state', () => {
    expect(resolveEffectiveSourceStatus(undefined, false, readySource)).toBe(readySource);
    expect(resolveEffectiveSourceStatus(null, true, readySource)).toBeNull();
    expect(resolveEffectiveSourceStatus(readySource, true, undefined)).toBe(readySource);
    expect(resolveEffectiveSourceStatus(readySource, true, {
      ...readySource,
      available: false,
      sourceWarning: 'Source is unavailable.'
    })).toMatchObject({
      available: false,
      sourceWarning: 'Source is unavailable.'
    });
    expect(resolveEffectiveSourceStatus({
      ...readySource,
      requiresRescan: true,
      sourceWarning: 'Source changed.'
    }, true, readySource)).toMatchObject({
      requiresRescan: true,
      sourceWarning: 'Source changed.'
    });
  });

  it('changes catalog context with snapshot, source state, and download capabilities', () => {
    expect(buildCatalogContextKey(readySource, 'ready', {
      canRescanInpxSource: true,
      canDownloadOriginal: true,
      canDownloadAsEpub: false
    })).toBe('snapshot-7:ready:original:no-epub');
    expect(buildCatalogContextKey(null, 'unconfigured', undefined)).toBe(
      'no-snapshot:unconfigured:no-original:no-epub'
    );
  });

  it('selects an active or newer external scan without reviving stale progress', () => {
    const idle: ScanProgress = { active: false, status: 'idle' };
    const activeWithoutId: ScanProgress = { active: true, status: 'running' };
    const activeJob: ScanProgress = { active: true, jobId: 9, status: 'running' };
    const completedJob: ScanProgress = { active: false, jobId: 9, status: 'completed' };
    const previousCompletedJob: ScanProgress = { active: false, jobId: 8, status: 'completed' };

    expect(selectLatestScan(undefined, activeJob)).toBe(activeJob);
    expect(selectLatestScan(idle, undefined)).toBe(idle);
    expect(selectLatestScan(activeWithoutId, previousCompletedJob)).toBe(activeWithoutId);
    expect(selectLatestScan(activeJob, completedJob)).toBe(completedJob);
    expect(selectLatestScan(activeJob, previousCompletedJob)).toBe(activeJob);
    expect(selectLatestScan(activeJob, activeJob)).toBe(activeJob);
  });
});
