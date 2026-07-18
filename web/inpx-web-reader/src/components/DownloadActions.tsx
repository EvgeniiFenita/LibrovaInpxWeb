import { Download, FileArchive } from 'lucide-react';

import type { BookDetails, BookListItem } from '../api/types';

interface DownloadActionsProps {
  book: BookListItem | BookDetails;
  compact?: boolean;
  onDownloadOriginal: (book: BookListItem | BookDetails) => void;
  onDownloadEpub: (book: BookListItem | BookDetails) => void;
}

function originalReason(book: BookListItem | BookDetails) {
  if (!book.isAvailable) {
    return book.availabilityLabel || 'Book is unavailable.';
  }
  if (!book.actions.canDownloadOriginal) {
    return 'Original download is unavailable.';
  }
  return '';
}

function epubReason(book: BookListItem | BookDetails) {
  if (!book.isAvailable) {
    return book.availabilityLabel || 'Book is unavailable.';
  }
  if (!book.actions.canDownloadAsEpub || !book.epubDownloadUrl) {
    return 'EPUB download is unavailable.';
  }
  return '';
}

export function DownloadActions({
  book,
  compact = false,
  onDownloadOriginal,
  onDownloadEpub
}: DownloadActionsProps) {
  const originalDisabledReason = originalReason(book);
  const epubDisabledReason = epubReason(book);

  return (
    <div className={compact ? 'download-actions compact' : 'download-actions'}>
      <button
        type="button"
        className="ghost-button"
        disabled={Boolean(originalDisabledReason)}
        title={originalDisabledReason || 'Download original'}
        onClick={() => onDownloadOriginal(book)}
      >
        <Download aria-hidden="true" size={17} />
        Original
      </button>
      <button
        type="button"
        className="ghost-button"
        disabled={Boolean(epubDisabledReason)}
        title={epubDisabledReason || 'Download EPUB'}
        onClick={() => onDownloadEpub(book)}
      >
        <FileArchive aria-hidden="true" size={17} />
        EPUB
      </button>
    </div>
  );
}
