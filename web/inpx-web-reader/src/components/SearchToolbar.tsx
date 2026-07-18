import { ArrowDownAZ, ArrowUpAZ, ChevronDown, Filter, Search, SlidersHorizontal, X } from 'lucide-react';
import type { FormEvent, SyntheticEvent } from 'react';
import { useCallback, useEffect, useMemo, useState } from 'react';

import type { BookQuery, FacetItem } from '../api/types';
import { mobileCatalogMediaQuery, useMediaQuery } from '../hooks/useMediaQuery';
import { useModalA11y } from '../hooks/useModalA11y';

interface SearchToolbarProps {
  query: BookQuery;
  languageFacets: FacetItem[];
  genreFacets: FacetItem[];
  busy: boolean;
  onQueryChange: (query: BookQuery) => void;
}

const searchFields: Array<{ value: BookQuery['fields'][number]; label: string }> = [
  { value: 'title', label: 'Title' },
  { value: 'authors', label: 'Author' },
  { value: 'description', label: 'Annotation' }
];

const sortOptions: Array<{ value: BookQuery['sort']; label: string }> = [
  { value: 'title', label: 'Title' },
  { value: 'authors', label: 'Author' },
  { value: 'added', label: 'Date added' }
];

const searchDebounceMs = 250;
type MobileSheet = 'fields' | 'filters' | 'sort';

function splitCsv(value: string) {
  return value.split(',').map((item) => item.trim()).filter(Boolean);
}

function toggleCsvValue(value: string, nextValue: string) {
  const values = splitCsv(value);
  const exists = values.includes(nextValue);
  const nextValues = exists
    ? values.filter((item) => item !== nextValue)
    : [...values, nextValue];
  return nextValues.join(',');
}

function isCsvChecked(value: string, needle: string) {
  return splitCsv(value).includes(needle);
}

function fieldSummary(fields: BookQuery['fields']) {
  if (fields.length === searchFields.length) {
    return 'All fields';
  }
  return searchFields
    .filter((field) => fields.includes(field.value))
    .map((field) => field.label)
    .join(', ');
}

function filterCount(query: BookQuery) {
  return splitCsv(query.language).length
    + splitCsv(query.genre).length;
}

function filterSummary(count: number) {
  if (count === 0) {
    return 'Filters';
  }
  return count === 1 ? '1 filter' : `${count} filters`;
}

function facetMatchesSearch(value: string, query: string) {
  const normalizedValue = value.trim().toLowerCase();
  const normalizedQuery = query.trim().toLowerCase();
  if (!normalizedQuery) {
    return true;
  }
  if (normalizedValue.includes(normalizedQuery)) {
    return true;
  }
  return normalizedValue
    .split(/[\s,;:/\\|()[\]{}._-]+/)
    .some((token) => token.startsWith(normalizedQuery));
}

function FacetChip({
  label,
  count,
  checked,
  onClick
}: {
  label: string;
  count?: number;
  checked: boolean;
  onClick: () => void;
}) {
  const text = count === undefined ? label : `${label} (${count.toLocaleString()})`;
  return (
    <button
      type="button"
      className={checked ? 'facet-chip selected' : 'facet-chip'}
      aria-pressed={checked}
      onClick={onClick}
    >
      {text}
    </button>
  );
}

export function SearchToolbar({
  query,
  languageFacets,
  genreFacets,
  busy,
  onQueryChange
}: SearchToolbarProps) {
  const [draftText, setDraftText] = useState(query.text);
  const [genreSearchText, setGenreSearchText] = useState('');
  const [mobileSheet, setMobileSheet] = useState<MobileSheet | null>(null);
  const isMobileCatalog = useMediaQuery(mobileCatalogMediaQuery);
  const closeMobileSheet = useCallback(() => setMobileSheet(null), []);
  const mobileSheetRef = useModalA11y<HTMLElement>({
    active: Boolean(mobileSheet),
    onClose: closeMobileSheet
  });
  const activeFilterCount = useMemo(() => filterCount(query), [query]);
  const selectedSort = sortOptions.find((option) => option.value === query.sort)?.label ?? 'Title';
  const searchFieldLabel = fieldSummary(query.fields);
  const SortDirectionIcon = query.direction === 'asc' ? ArrowUpAZ : ArrowDownAZ;
  const filteredGenreFacets = useMemo(
    () => genreFacets.filter((facet) => facetMatchesSearch(facet.value, genreSearchText)),
    [genreFacets, genreSearchText]
  );

  useEffect(() => {
    const closeOpenDropdowns = (except?: HTMLElement | null) => {
      document.querySelectorAll<HTMLDetailsElement>('.toolbar-dropdown[open]').forEach((dropdown) => {
        if (!except || !dropdown.contains(except)) {
          dropdown.open = false;
        }
      });
    };

    const handlePointerDown = (event: PointerEvent) => {
      const target = event.target instanceof HTMLElement ? event.target : null;
      if (!target?.closest('.toolbar-dropdown')) {
        closeOpenDropdowns();
      }
    };

    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        closeOpenDropdowns();
      }
    };

    document.addEventListener('pointerdown', handlePointerDown);
    document.addEventListener('keydown', handleKeyDown);
    return () => {
      document.removeEventListener('pointerdown', handlePointerDown);
      document.removeEventListener('keydown', handleKeyDown);
    };
  }, []);

  useEffect(() => {
    setDraftText(query.text);
  }, [query.text]);

  useEffect(() => {
    if (!isMobileCatalog && mobileSheet) {
      setMobileSheet(null);
    }
  }, [isMobileCatalog, mobileSheet]);

  useEffect(() => {
    const handle = window.setTimeout(() => {
      if (draftText !== query.text) {
        onQueryChange({
          ...query,
          text: draftText,
          offset: 0
        });
      }
    }, searchDebounceMs);

    return () => window.clearTimeout(handle);
  }, [draftText, onQueryChange, query]);

  function submit(event: FormEvent) {
    event.preventDefault();
    if (draftText !== query.text) {
      onQueryChange({
        ...query,
        text: draftText,
        offset: 0
      });
    }
  }

  function setSearchField(field: BookQuery['fields'][number], enabled: boolean) {
    const nextFields = enabled
      ? Array.from(new Set([...query.fields, field]))
      : query.fields.filter((item) => item !== field);
    if (nextFields.length === 0) {
      return;
    }
    onQueryChange({ ...query, fields: nextFields, offset: 0 });
  }

  function closeOtherDropdowns(event: SyntheticEvent<HTMLDetailsElement>) {
    if (!event.currentTarget.open) {
      return;
    }
    document.querySelectorAll<HTMLDetailsElement>('.toolbar-dropdown[open]').forEach((dropdown) => {
      if (dropdown !== event.currentTarget) {
        dropdown.open = false;
      }
    });
  }

  function clearFilters() {
    setGenreSearchText('');
    onQueryChange({ ...query, language: '', genre: '', offset: 0 });
  }

  function renderSearchFields() {
    return searchFields.map((field) => (
      <label key={field.value} className="check-row">
        <input
          type="checkbox"
          checked={query.fields.includes(field.value)}
          onChange={(event) => setSearchField(field.value, event.target.checked)}
        />
        <span>{field.label}</span>
      </label>
    ));
  }

  function renderFilters() {
    return (
      <>
        <div className="filter-group">
          <h3>Languages</h3>
          <div className="facet-flow">
            {languageFacets.length === 0 && <p className="empty-options">No language facets</p>}
            {languageFacets.map((facet) => (
              <FacetChip
                key={facet.value}
                label={facet.value}
                count={facet.count}
                checked={isCsvChecked(query.language, facet.value)}
                onClick={() => onQueryChange({
                    ...query,
                    language: toggleCsvValue(query.language, facet.value),
                    offset: 0
                  })}
              />
            ))}
          </div>
        </div>

        <div className="filter-group">
          <h3>Genres</h3>
          <input
            className="filter-search-field"
            value={genreSearchText}
            onChange={(event) => setGenreSearchText(event.target.value)}
            placeholder="Search genres..."
            aria-label="Search genres"
          />
          <div className="facet-flow facet-flow-scroll">
            {filteredGenreFacets.length === 0 && <p className="empty-options">No genre facets</p>}
            {filteredGenreFacets.map((facet) => (
              <FacetChip
                key={facet.value}
                label={facet.value}
                count={facet.count}
                checked={isCsvChecked(query.genre, facet.value)}
                onClick={() => onQueryChange({
                    ...query,
                    genre: toggleCsvValue(query.genre, facet.value),
                    offset: 0
                  })}
              />
            ))}
          </div>
        </div>

        {activeFilterCount > 0 && (
          <>
            <div className="filter-separator" />
            <div className="filter-footer">
              <span>{activeFilterCount === 1 ? '1 filter active' : `${activeFilterCount} filters active`}</span>
              <button type="button" className="ghost-button compact-action" onClick={clearFilters}>
                Clear all
              </button>
            </div>
          </>
        )}
      </>
    );
  }

  function renderSortControls() {
    return (
      <>
        <div className="mobile-sheet-group">
          <h3>Sort by</h3>
          <div className="checkbox-panel">
            {sortOptions.map((option) => (
              <label key={option.value} className="check-row">
                <input
                  type="radio"
                  name="sort"
                  checked={query.sort === option.value}
                  onChange={() => onQueryChange({ ...query, sort: option.value, offset: 0 })}
                />
                <span>{option.label}</span>
              </label>
            ))}
          </div>
        </div>
        <div className="mobile-sheet-group">
          <h3>Direction</h3>
          <div className="mobile-direction-pair" role="group" aria-label="Sort direction">
            <button
              type="button"
              className={query.direction === 'asc' ? 'mobile-segment selected' : 'mobile-segment'}
              aria-pressed={query.direction === 'asc'}
              onClick={() => onQueryChange({ ...query, direction: 'asc', offset: 0 })}
            >
              <ArrowUpAZ aria-hidden="true" size={17} />
              Ascending
            </button>
            <button
              type="button"
              className={query.direction === 'desc' ? 'mobile-segment selected' : 'mobile-segment'}
              aria-pressed={query.direction === 'desc'}
              onClick={() => onQueryChange({ ...query, direction: 'desc', offset: 0 })}
            >
              <ArrowDownAZ aria-hidden="true" size={17} />
              Descending
            </button>
          </div>
        </div>
      </>
    );
  }

  function renderMobileSheet() {
    if (!mobileSheet) {
      return null;
    }

    return (
      <div
        className="mobile-sheet-backdrop"
        data-modal-root
        onClick={(event) => {
          if (event.target === event.currentTarget) {
            closeMobileSheet();
          }
        }}
      >
        <section
          ref={mobileSheetRef}
          className="mobile-sheet"
          role="dialog"
          aria-modal="true"
          aria-label="Catalog tools"
          tabIndex={-1}
        >
          <div className="mobile-sheet-grabber" />
          <header className="mobile-sheet-header">
            <div>
              <span className="eyebrow">Catalog tools</span>
              <h2>Refine catalog</h2>
            </div>
            <button type="button" className="icon-button tiny" onClick={closeMobileSheet} aria-label="Close catalog tools">
              <X aria-hidden="true" size={17} />
            </button>
          </header>

          <div className="mobile-sheet-tabs" role="group" aria-label="Catalog tool sections">
            <button
              type="button"
              className={mobileSheet === 'fields' ? 'mobile-tab selected' : 'mobile-tab'}
              aria-pressed={mobileSheet === 'fields'}
              onClick={() => setMobileSheet('fields')}
            >
              Fields
            </button>
            <button
              type="button"
              className={mobileSheet === 'filters' ? 'mobile-tab selected' : 'mobile-tab'}
              aria-pressed={mobileSheet === 'filters'}
              onClick={() => setMobileSheet('filters')}
            >
              Filters
            </button>
            <button
              type="button"
              className={mobileSheet === 'sort' ? 'mobile-tab selected' : 'mobile-tab'}
              aria-pressed={mobileSheet === 'sort'}
              onClick={() => setMobileSheet('sort')}
            >
              Sort
            </button>
          </div>

          <div className={mobileSheet === 'filters' ? 'mobile-sheet-body mobile-filters-body' : 'mobile-sheet-body'}>
            {mobileSheet === 'fields' && (
              <div className="mobile-sheet-group">
                <h3>Search in</h3>
                <div className="checkbox-panel">
                  {renderSearchFields()}
                </div>
              </div>
            )}
            {mobileSheet === 'filters' && renderFilters()}
            {mobileSheet === 'sort' && renderSortControls()}
          </div>

          <footer className="mobile-sheet-footer">
            <button type="button" className="primary-button" onClick={closeMobileSheet}>
              Done
            </button>
          </footer>
        </section>
      </div>
    );
  }

  if (isMobileCatalog) {
    return (
      <form className="mobile-search-toolbar" onSubmit={submit} aria-label="Catalog search" aria-busy={busy}>
        <div className="search-box">
          <Search aria-hidden="true" size={18} />
          <input
            value={draftText}
            onChange={(event) => setDraftText(event.target.value)}
            placeholder="Search catalog..."
            aria-label="Search catalog"
          />
        </div>

        <div className="mobile-tool-row">
          <button
            type="button"
            className="mobile-tool-chip"
            aria-label={`Search fields: ${searchFieldLabel}`}
            onClick={() => setMobileSheet('fields')}
          >
            <Search aria-hidden="true" size={16} />
            <span>{searchFieldLabel}</span>
          </button>
          <button
            type="button"
            className={activeFilterCount > 0 ? 'mobile-tool-chip active' : 'mobile-tool-chip'}
            aria-label={activeFilterCount === 0 ? 'Filters' : `${activeFilterCount} filters active`}
            onClick={() => setMobileSheet('filters')}
          >
            <Filter aria-hidden="true" size={16} />
            <span>{filterSummary(activeFilterCount)}</span>
          </button>
          <button
            type="button"
            className="mobile-tool-chip"
            aria-label={`Sort: ${selectedSort}, ${query.direction === 'asc' ? 'ascending' : 'descending'}`}
            onClick={() => setMobileSheet('sort')}
          >
            <SlidersHorizontal aria-hidden="true" size={16} />
            <span>{selectedSort}</span>
          </button>
          <button
            type="button"
            className="mobile-direction-button"
            aria-label={`Sort direction: ${query.direction === 'asc' ? 'Ascending' : 'Descending'}`}
            onClick={() => onQueryChange({
              ...query,
              direction: query.direction === 'asc' ? 'desc' : 'asc',
              offset: 0
            })}
          >
            <SortDirectionIcon aria-hidden="true" size={17} />
          </button>
        </div>

        {renderMobileSheet()}
      </form>
    );
  }

  return (
    <form className="search-toolbar" onSubmit={submit} aria-label="Catalog search" aria-busy={busy}>
      <div className="search-box">
        <Search aria-hidden="true" size={18} />
        <input
          value={draftText}
          onChange={(event) => setDraftText(event.target.value)}
          placeholder="Search titles, authors, annotations..."
          aria-label="Search catalog"
        />
      </div>

      <details className="toolbar-dropdown" onToggle={closeOtherDropdowns}>
        <summary
          className="toolbar-icon-summary"
          aria-label={`Search fields: ${searchFieldLabel}`}
          title={`Search fields: ${searchFieldLabel}`}
        >
          <Search aria-hidden="true" size={20} />
          <ChevronDown aria-hidden="true" size={17} />
        </summary>
        <div className="dropdown-panel checkbox-panel">
          {renderSearchFields()}
        </div>
      </details>

      <details className="toolbar-dropdown filters-dropdown" onToggle={closeOtherDropdowns}>
        <summary>
          <Filter aria-hidden="true" size={16} />
          {activeFilterCount === 0 ? 'No filters active' : `${activeFilterCount} filters active`}
          <ChevronDown aria-hidden="true" size={15} />
        </summary>
        <div className="dropdown-panel filters-panel">
          {renderFilters()}
        </div>
      </details>

      <details className="toolbar-dropdown sort-dropdown" onToggle={closeOtherDropdowns}>
        <summary>
          Sort: {selectedSort}
          <ChevronDown aria-hidden="true" size={15} />
        </summary>
        <div className="dropdown-panel checkbox-panel">
          {sortOptions.map((option) => (
            <label key={option.value} className="check-row">
              <input
                type="radio"
                name="sort"
                checked={query.sort === option.value}
                onChange={() => onQueryChange({ ...query, sort: option.value, offset: 0 })}
              />
              <span>{option.label}</span>
            </label>
          ))}
        </div>
      </details>

      <button
        type="button"
        className="icon-button sort-direction-button"
        aria-label={`Sort direction: ${query.direction === 'asc' ? 'Ascending' : 'Descending'}`}
        title={`Sort direction: ${query.direction === 'asc' ? 'Ascending' : 'Descending'}`}
        onClick={() => onQueryChange({
          ...query,
          direction: query.direction === 'asc' ? 'desc' : 'asc',
          offset: 0
        })}
      >
        <SortDirectionIcon aria-hidden="true" size={18} />
      </button>
    </form>
  );
}
