export type SortDirection = 'asc' | 'desc';
export type BookSort = 'title' | 'authors' | 'added';
export type BookFormat = 'fb2' | 'epub';
export type ScanStatus = 'idle' | 'pending' | 'running' | 'completed' | 'failed' | 'cancelled' | 'cancelling';

export interface CatalogCapabilities {
  canRescanInpxSource: boolean;
  canDownloadOriginal: boolean;
  canDownloadAsEpub: boolean;
}

export interface InpxSourceStatus {
  available: boolean;
  requiresRescan: boolean;
  sourceWarning: string;
  totalBookCount: number;
  availableBookCount: number;
  unavailableBookCount: number;
  warningCount: number;
}

export interface InpxSourceOverview extends InpxSourceStatus {
  inpxPath: string;
  archiveRoot: string;
  displayName: string;
  lastScanStartedAtUtc: string | null;
  lastScanCompletedAtUtc: string | null;
  lastSeenSnapshotId: string | null;
  recentWarnings: string[];
}

export interface ScanProgress {
  active: boolean;
  jobId?: number;
  status: ScanStatus;
  percent?: number;
  message?: string;
  warnings?: string[];
  totalRecords?: number;
  scannedRecords?: number;
  parsedFb2Records?: number;
  addedRecords?: number;
  updatedRecords?: number;
  markedUnavailableRecords?: number;
  unavailableRecords?: number;
  skippedRecords?: number;
  reusedRecords?: number;
  segmentsTotal?: number;
  segmentsUnchanged?: number;
  segmentsAdded?: number;
  segmentsChanged?: number;
  segmentsRemoved?: number;
  archivesSkipped?: number;
  archivesOpened?: number;
  archiveBytesRead?: number;
  current?: {
    archiveName: string;
    entryName: string;
  };
  result?: {
    totalRecords: number;
    scannedRecords: number;
    parsedFb2Records: number;
    addedRecords: number;
    updatedRecords: number;
    markedUnavailableRecords: number;
    unavailableRecords: number;
    skippedRecords: number;
    reusedRecords: number;
    segmentsTotal: number;
    segmentsUnchanged: number;
    segmentsAdded: number;
    segmentsChanged: number;
    segmentsRemoved: number;
    archivesSkipped: number;
    archivesOpened: number;
    archiveBytesRead: number;
    warningCount: number;
  };
  error?: {
    code: string;
    message: string;
  };
}

export interface ServerStatus {
  version: string;
  status: 'open' | 'closed';
  capabilities: CatalogCapabilities;
  converter: {
    available: boolean;
    canDownloadAsEpub: boolean;
  };
  scan: ScanProgress;
  inpxSource?: InpxSourceStatus;
  runtime: {
    uptimeSeconds: number;
    http: {
      activeWorkers: number;
      queuedRequests: number;
      maxWorkers: number;
      maxQueuedRequests: number;
    };
    backend: {
      activeOperations: number;
      queuedOperations: number;
      maxQueueDepth: number;
    };
    scan: {
      active: boolean;
      activeJobs: number;
      maxConcurrentJobs: number;
      activeWorkers: number;
      maxWorkers: number;
    };
    downloads: {
      active: number;
      maxConcurrent: number;
    };
    storage: {
      cacheRootPresent: boolean;
      cacheDatabasePresent: boolean;
      runtimeWorkspacePresent: boolean;
      coverCacheBytes: number | null;
      inpxScanWorkspaceBytes: number | null;
      downloadWorkspaceBytes: number | null;
    };
    resources: {
      residentMemoryBytes: number | null;
      peakResidentMemoryBytes: number | null;
      maxCoverCacheBytes: number;
      maxSteadyStateMemoryBytes: number;
    };
  };
}

export interface SourceResponse {
  source: InpxSourceOverview | null;
}

export interface CatalogStatistics {
  bookCount: number;
  unavailableBookCount: number;
  inpxSourceSizeBytes: number;
  coverCacheSizeBytes: number;
  databaseSizeBytes: number;
  totalCatalogSizeBytes: number;
}

export interface FacetItem {
  value: string;
  count: number;
}

export interface BookActions {
  canDownloadOriginal: boolean;
  canDownloadAsEpub: boolean;
}

export interface BookListItem {
  id: number;
  title: string;
  authors: string[];
  language: string;
  seriesName: string | null;
  seriesIndex: number | null;
  year: number | null;
  tags: string[];
  genres: string[];
  format: BookFormat;
  sizeBytes: number;
  addedAtUtc: string;
  coverUrl: string | null;
  downloadUrl: string;
  epubDownloadUrl: string | null;
  actions: BookActions;
  isAvailable: boolean;
  availabilityLabel: string;
}

export interface BookDetails extends BookListItem {
  publisher: string | null;
  isbn: string | null;
  description: string | null;
  identifier: string | null;
}

export interface BookListResponse {
  items: BookListItem[];
  catalogSnapshotId: string;
  totalCount: number | null;
  offset: number;
  limit: number;
  nextCursor: string | null;
  facets: {
    languages: FacetItem[];
    genres: FacetItem[];
  } | null;
}

export interface BookDetailsResponse {
  book: BookDetails;
}

export interface ScanStartResponse {
  jobId: number;
  scan: ScanProgress;
}

export interface ScanCancelResponse {
  accepted: boolean;
  scan: ScanProgress;
}

export interface ApiErrorBody {
  error: {
    code: string;
    message: string;
    requestId?: string;
  };
}

export interface BookQuery {
  text: string;
  fields: Array<'title' | 'authors' | 'description'>;
  language: string;
  genre: string;
  sort: BookSort;
  direction: SortDirection;
  offset: number;
  limit: number;
  includeFacets: boolean;
}

export interface DownloadResult {
  blob: Blob;
  fileName: string;
}
