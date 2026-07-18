import { Eye, EyeOff, Info, KeyRound, RefreshCw, X } from 'lucide-react';
import { useState } from 'react';

import brandBadgeUrl from '../assets/brand_badge.png';
import type { InpxSourceOverview, CatalogStatistics, ServerStatus } from '../api/types';
import { resolveSourceState, sourceStateLabel, type SourceState } from '../catalogState';
import { formatBytes, formatDate } from '../format';
import { useModalA11y } from '../hooks/useModalA11y';

interface SettingsDialogProps {
  status: ServerStatus | undefined;
  source: InpxSourceOverview | null;
  statistics: CatalogStatistics | undefined;
  sourceState?: SourceState;
  rescanBusy: boolean;
  accessPassword: string;
  onClose: () => void;
  onRescan: () => void;
  onAccessPasswordChange: (value: string) => void;
  onForgetAccessPassword: () => void;
}

function InfoRow({ label, value }: { label: string; value: string }) {
  return (
    <div className="info-row">
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}

export function SettingsDialog({
  status,
  source,
  statistics,
  sourceState,
  rescanBusy,
  accessPassword,
  onClose,
  onRescan,
  onAccessPasswordChange,
  onForgetAccessPassword
}: SettingsDialogProps) {
  const [draftAccessPassword, setDraftAccessPassword] = useState(accessPassword);
  const [passwordVisible, setPasswordVisible] = useState(false);
  const modalRef = useModalA11y<HTMLElement>({ onClose });
  const totalBooks = statistics?.bookCount
    ?? status?.inpxSource?.totalBookCount
    ?? source?.totalBookCount
    ?? 0;
  const canRescan = status?.capabilities.canRescanInpxSource ?? false;
  const effectiveSourceState = sourceState ?? resolveSourceState(
    source ?? status?.inpxSource ?? null,
    true
  );

  return (
    <div className="modal-backdrop" data-modal-root>
      <section
        ref={modalRef}
        className="settings-dialog modal-card"
        role="dialog"
        aria-modal="true"
        aria-labelledby="settings-title"
        tabIndex={-1}
      >
        <header className="modal-header">
          <div>
            <span className="eyebrow">Settings</span>
            <h2 id="settings-title">Catalog information</h2>
          </div>
          <button type="button" className="icon-button" onClick={onClose} aria-label="Close settings">
            <X aria-hidden="true" size={18} />
          </button>
        </header>

        <div className="settings-content">
          <section className="settings-section">
            <h3>Catalog</h3>
            <InfoRow label="Books" value={totalBooks.toLocaleString()} />
            <InfoRow label="Unavailable" value={(statistics?.unavailableBookCount ?? status?.inpxSource?.unavailableBookCount ?? source?.unavailableBookCount ?? 0).toLocaleString()} />
            <InfoRow label="Catalog size" value={formatBytes(statistics?.totalCatalogSizeBytes ?? 0)} />
            <InfoRow label="Last indexing" value={formatDate(source?.lastScanCompletedAtUtc)} />
          </section>

          <section className="settings-section">
            <h3>Source</h3>
            <InfoRow
              label="Catalog"
              value={source?.displayName || (effectiveSourceState === 'unconfigured' ? 'Not configured' : 'INPX source')}
            />
            <InfoRow label="Status" value={sourceStateLabel(effectiveSourceState)} />
            <InfoRow label="Warnings" value={(status?.inpxSource?.warningCount ?? source?.warningCount ?? 0).toLocaleString()} />
            <button
              type="button"
              className="primary-button"
              onClick={onRescan}
              disabled={rescanBusy || !canRescan}
              title={canRescan ? undefined : 'INPX source is not configured or unavailable.'}
            >
              <RefreshCw aria-hidden="true" size={17} />
              Rescan
            </button>
          </section>

          <section className="settings-section access-section">
            <h3>Server access</h3>
            <form
              className="settings-password-form"
              onSubmit={(event) => {
                event.preventDefault();
                onAccessPasswordChange(draftAccessPassword.trim());
              }}
            >
              <label htmlFor="settings-access-password">Access password</label>
              <div className="auth-token-field settings-password-field">
                <KeyRound aria-hidden="true" size={17} />
                <input
                  id="settings-access-password"
                  type={passwordVisible ? 'text' : 'password'}
                  value={draftAccessPassword}
                  onChange={(event) => setDraftAccessPassword(event.target.value)}
                  autoComplete="current-password"
                />
                <button
                  type="button"
                  className="icon-button"
                  onClick={() => setPasswordVisible((visible) => !visible)}
                  aria-label={passwordVisible ? 'Hide access password' : 'Show access password'}
                >
                  {passwordVisible
                    ? <EyeOff aria-hidden="true" size={17} />
                    : <Eye aria-hidden="true" size={17} />}
                </button>
              </div>
              <p className="quiet-note">Stored only in this browser.</p>
              <div className="modal-actions">
                <button
                  type="button"
                  className="secondary-button"
                  onClick={onForgetAccessPassword}
                  disabled={!accessPassword}
                >
                  Forget on this device
                </button>
                <button
                  type="submit"
                  className="primary-button"
                  disabled={
                    !draftAccessPassword.trim()
                    || draftAccessPassword.trim() === accessPassword.trim()
                  }
                >
                  Save password
                </button>
              </div>
            </form>
          </section>

          <section className="settings-section about-section">
            <div className="about-lockup">
              <img src={brandBadgeUrl} alt="" />
              <div>
                <h3>InpxWebReader</h3>
                <p>{status?.version ?? '1.2.0'}</p>
              </div>
            </div>
            <InfoRow label="Author" value="Evgenii Volokhovich" />
            <InfoRow label="Contact" value="evgenii.github@gmail.com" />
            <p className="quiet-note">
              <Info aria-hidden="true" size={15} />
              Trusted home LAN server UI
            </p>
          </section>
        </div>
      </section>
    </div>
  );
}
