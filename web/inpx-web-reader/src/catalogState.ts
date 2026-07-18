import type {
  CatalogCapabilities,
  InpxSourceOverview,
  InpxSourceStatus,
  ScanProgress
} from './api/types';

export type SourceState = 'loading' | 'unconfigured' | 'unavailable' | 'changed' | 'ready';

export function resolveEffectiveSourceStatus(
  source: InpxSourceStatus | null | undefined,
  sourceResolved: boolean,
  statusSource: InpxSourceStatus | undefined
): InpxSourceStatus | null | undefined {
  if (sourceResolved && !source) {
    return null;
  }
  if (!source) {
    return statusSource;
  }
  if (!statusSource) {
    return source;
  }

  return {
    ...source,
    available: source.available && statusSource.available,
    requiresRescan: source.requiresRescan || statusSource.requiresRescan,
    sourceWarning: statusSource.sourceWarning || source.sourceWarning,
    totalBookCount: statusSource.totalBookCount,
    availableBookCount: statusSource.availableBookCount,
    unavailableBookCount: statusSource.unavailableBookCount,
    warningCount: statusSource.warningCount
  };
}

export function resolveSourceState(
  source: InpxSourceStatus | null | undefined,
  resolved: boolean
): SourceState {
  if (!resolved) {
    return 'loading';
  }
  if (!source) {
    return 'unconfigured';
  }
  if (source.requiresRescan) {
    return 'changed';
  }
  if (!source.available) {
    return 'unavailable';
  }
  return 'ready';
}

export function sourceStateLabel(state: SourceState) {
  switch (state) {
    case 'loading':
      return 'Checking source';
    case 'unconfigured':
      return 'Not configured';
    case 'unavailable':
      return 'Unavailable';
    case 'changed':
      return 'Changed — rescan required';
    case 'ready':
      return 'Available';
  }
}

export function buildCatalogContextKey(
  source: InpxSourceOverview | null,
  sourceState: SourceState,
  capabilities: CatalogCapabilities | undefined
) {
  return [
    source?.lastSeenSnapshotId ?? 'no-snapshot',
    sourceState,
    capabilities?.canDownloadOriginal ? 'original' : 'no-original',
    capabilities?.canDownloadAsEpub ? 'epub' : 'no-epub'
  ].join(':');
}

export function selectLatestScan(
  statusScan: ScanProgress | null | undefined,
  progressScan: ScanProgress | null | undefined
) {
  if (!statusScan) {
    return progressScan;
  }
  if (!progressScan) {
    return statusScan;
  }

  const statusJobId = statusScan.jobId ?? -1;
  const progressJobId = progressScan.jobId ?? -1;
  if (statusScan.active !== progressScan.active && (statusJobId < 0 || progressJobId < 0)) {
    return statusScan.active ? statusScan : progressScan;
  }
  if (statusJobId !== progressJobId) {
    return statusJobId > progressJobId ? statusScan : progressScan;
  }

  if (statusScan.active !== progressScan.active) {
    return progressScan;
  }
  return progressScan;
}
