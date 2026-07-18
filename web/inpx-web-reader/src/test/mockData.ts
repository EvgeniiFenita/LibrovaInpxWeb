import type {
  BookDetails,
  BookListItem,
  BookListResponse,
  ScanProgress,
  ServerStatus,
  SourceResponse
} from '../api/types';

export const mockBook: BookListItem = {
  id: 7,
  title: 'Test Book',
  authors: ['Ada Reader'],
  language: 'en',
  seriesName: 'Smoke',
  seriesIndex: 1,
  year: 2026,
  tags: [],
  genres: ['Science Fiction'],
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
};

export const mockDetails: BookDetails = {
  ...mockBook,
  publisher: 'InpxWebReader Test Press',
  isbn: '9780000000002',
  description: 'A compact browser smoke fixture.',
  identifier: 'fixture-7'
};

export const mockBookList: BookListResponse = {
  items: [mockBook],
  catalogSnapshotId: 'fixture',
  totalCount: 1,
  offset: 0,
  limit: 60,
  nextCursor: null,
  facets: {
    languages: [{ value: 'en', count: 1 }],
    genres: [{ value: 'Science Fiction', count: 1 }]
  }
};

export const mockStats = {
  bookCount: 1,
  unavailableBookCount: 0,
  inpxSourceSizeBytes: 2048,
  coverCacheSizeBytes: 1024,
  databaseSizeBytes: 1024,
  totalCatalogSizeBytes: 4096
};

export const mockScan: ScanProgress = {
  active: false,
  status: 'idle'
};

export const activeScan: ScanProgress = {
  active: true,
  jobId: 3,
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

export const mockStatus: ServerStatus = {
  version: '1.2.0',
  status: 'open',
  capabilities: {
    canRescanInpxSource: true,
    canDownloadOriginal: true,
    canDownloadAsEpub: false
  },
  converter: {
    available: false,
    canDownloadAsEpub: false
  },
  scan: mockScan,
  inpxSource: {
    available: true,
    requiresRescan: false,
    sourceWarning: '',
    totalBookCount: 1,
    availableBookCount: 1,
    unavailableBookCount: 0,
    warningCount: 0
  },
  runtime: {
    uptimeSeconds: 15,
    http: {
      activeWorkers: 1,
      queuedRequests: 0,
      maxWorkers: 4,
      maxQueuedRequests: 32
    },
    backend: {
      activeOperations: 0,
      queuedOperations: 0,
      maxQueueDepth: 64
    },
    scan: {
      active: false,
      activeJobs: 0,
      maxConcurrentJobs: 1,
      activeWorkers: 0,
      maxWorkers: 4
    },
    downloads: {
      active: 0,
      maxConcurrent: 2
    },
    storage: {
      cacheRootPresent: true,
      cacheDatabasePresent: true,
      runtimeWorkspacePresent: true,
      coverCacheBytes: 0,
      inpxScanWorkspaceBytes: 0,
      downloadWorkspaceBytes: 0
    },
    resources: {
      residentMemoryBytes: null,
      peakResidentMemoryBytes: null,
      maxCoverCacheBytes: 128 * 1024 * 1024,
      maxSteadyStateMemoryBytes: 1024 * 1024 * 1024
    }
  }
};

export const mockSource: SourceResponse = {
  source: {
    ...mockStatus.inpxSource!,
    inpxPath: '/source/catalog.inpx',
    archiveRoot: '/source',
    displayName: 'Fixture catalog',
    lastScanStartedAtUtc: '2026-05-20T09:00:00Z',
    lastScanCompletedAtUtc: '2026-05-20T09:02:00Z',
    lastSeenSnapshotId: 'fixture',
    recentWarnings: []
  }
};
