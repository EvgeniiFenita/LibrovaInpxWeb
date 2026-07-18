import { AlertTriangle, Hash, RotateCcw, X } from 'lucide-react';

import type { BookDetails, BookListItem } from '../api/types';
import { formatBytes, formatDate, joinAuthors } from '../format';
import { mobileCatalogMediaQuery, useMediaQuery } from '../hooks/useMediaQuery';
import { useModalA11y } from '../hooks/useModalA11y';
import { CoverImage } from './CoverImage';
import { CoverPlaceholder } from './CoverPlaceholder';
import { DownloadActions } from './DownloadActions';

interface BookDetailsDrawerProps {
  book: BookDetails | null;
  loading: boolean;
  error?: string | null;
  token?: string;
  onClose: () => void;
  onRetry?: () => void;
  onDownloadOriginal: (book: BookListItem | BookDetails) => void;
  onDownloadEpub: (book: BookListItem | BookDetails) => void;
}

function DetailRow({ label, value }: { label: string; value: string | number | null | undefined }) {
  if (value === null || value === undefined || value === '') {
    return null;
  }

  return (
    <div className="detail-row">
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}

export function BookDetailsDrawer({
  book,
  loading,
  error,
  token,
  onClose,
  onRetry,
  onDownloadOriginal,
  onDownloadEpub
}: BookDetailsDrawerProps) {
  const isMobileCatalog = useMediaQuery(mobileCatalogMediaQuery);
  const modalRef = useModalA11y<HTMLElement>({ onClose });

  if (!book && !loading && !error) {
    return null;
  }
  const authors = book ? joinAuthors(book.authors) : '';

  return (
    <div className="modal-backdrop" data-modal-root>
      <section
        ref={modalRef}
        className="book-details-dialog modal-card"
        role="dialog"
        aria-modal="true"
        aria-label="Book details"
        tabIndex={-1}
      >
        <header className="modal-header details-header">
          <div className="details-title-row">
            <h2>{book?.title ?? (error ? 'Details unavailable' : 'Loading')}</h2>
            {book && (
              <span className={book.isAvailable ? 'details-status-pill ok' : 'details-status-pill warning'}>
                {book.availabilityLabel || (book.isAvailable ? 'Available' : 'Unavailable')}
              </span>
            )}
          </div>
          <button type="button" className="icon-button" onClick={onClose} aria-label="Close details">
            <X aria-hidden="true" size={18} />
          </button>
        </header>

        {error && !book && (
          <div className="details-error" role="alert">
            <AlertTriangle aria-hidden="true" size={22} />
            <div>
              <h3>Could not load details</h3>
              <p>{error}</p>
            </div>
            <div className="modal-actions">
              {onRetry && (
                <button type="button" className="ghost-button" onClick={onRetry}>
                  <RotateCcw aria-hidden="true" size={16} />
                  Retry
                </button>
              )}
              <button type="button" className="primary-button" onClick={onClose}>
                Close
              </button>
            </div>
          </div>
        )}

        {book && (
          <div className="details-layout">
            <aside className="details-cover-panel">
              <div className="details-cover">
                <CoverImage
                  coverUrl={book.coverUrl}
                  token={token}
                  loading="eager"
                  fallback={<CoverPlaceholder title={book.title} authors={authors} />}
                />
              </div>
            </aside>

            <div className="details-body">
              <section className="details-section details-lead">
                {isMobileCatalog && (
                  <div className="details-mobile-cover">
                    <CoverImage
                      coverUrl={book.coverUrl}
                      token={token}
                      loading="eager"
                      fallback={<CoverPlaceholder title={book.title} authors={authors} />}
                    />
                  </div>
                )}
                <p className="authors">{authors}</p>
                <div className="status-line">
                  <span>{book.format.toUpperCase()}</span>
                  <span>{formatBytes(book.sizeBytes)}</span>
                </div>
                <DownloadActions
                  book={book}
                  onDownloadOriginal={onDownloadOriginal}
                  onDownloadEpub={onDownloadEpub}
                />
              </section>

              <section className="details-section compact-list">
                <DetailRow label="Language" value={book.language || 'n/a'} />
                <DetailRow label="Year" value={book.year} />
                <DetailRow label="Series" value={book.seriesName} />
                <DetailRow label="Publisher" value={book.publisher} />
                <DetailRow label="ISBN" value={book.isbn} />
                <DetailRow label="Added" value={formatDate(book.addedAtUtc)} />
              </section>

              {book.description && (
                <section className="details-section">
                  <h3>Annotation</h3>
                  <p className="annotation">{book.description}</p>
                </section>
              )}

              <section className="details-section badge-list" aria-label="Book genres">
                {book.genres.slice(0, 10).map((genre) => (
                  <span key={genre}>
                    <Hash aria-hidden="true" size={15} />
                    {genre}
                  </span>
                ))}
              </section>
            </div>
          </div>
        )}
      </section>
    </div>
  );
}
