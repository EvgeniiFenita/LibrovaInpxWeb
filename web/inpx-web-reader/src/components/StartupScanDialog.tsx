import type { ScanProgress } from '../api/types';
import { useModalA11y } from '../hooks/useModalA11y';

interface StartupScanDialogProps {
  scan: ScanProgress | null | undefined;
  sourceWarning?: string;
  starting: boolean;
  startError?: string;
  actionError?: string;
  onContinue: () => void;
  onCancel: () => void;
  cancelling?: boolean;
}

function metric(label: string, value?: number) {
  return (
    <span>
      <strong>{(value ?? 0).toLocaleString()}</strong>
      {label}
    </span>
  );
}

function statusTitle(scan: ScanProgress | null | undefined, starting: boolean, startError?: string) {
  if (startError) {
    return 'Scan could not start';
  }
  if (starting || scan?.active) {
    return scan?.message || 'Scanning INPX catalog';
  }
  if (scan?.status === 'failed') {
    return 'Scan completed with errors';
  }
  if (scan?.status === 'cancelled') {
    return 'Scan was cancelled';
  }
  if (scan?.status === 'completed') {
    return 'Scan completed';
  }
  return 'Preparing catalog scan';
}

export function StartupScanDialog({
  scan,
  sourceWarning,
  starting,
  startError,
  actionError,
  onContinue,
  onCancel,
  cancelling = false
}: StartupScanDialogProps) {
  const modalRef = useModalA11y<HTMLElement>();
  const active = Boolean(scan?.active || starting);
  const percent = Math.max(0, Math.min(100, scan?.percent ?? 0));
  const terminal = Boolean(!active && scan && scan.status !== 'idle');
  const progressValue = active || scan?.status !== 'completed' ? percent : 100;
  const warnings = [
    ...(scan?.warnings ?? []),
    ...(sourceWarning ? [sourceWarning] : [])
  ];
  const error = startError || actionError || (!active ? scan?.error?.message : undefined);
  const liveStatus = active
    ? `${scan?.message || 'Scanning INPX catalog'}. ${percent.toFixed(0)}% complete.`
    : statusTitle(scan, starting, startError);

  return (
    <main className="startup-stage" aria-label="Server startup scan" data-modal-root>
      <section
        ref={modalRef}
        className="startup-dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="startup-scan-title"
        tabIndex={-1}
      >
        <div className="startup-copy">
          <span className="eyebrow">INPX source</span>
          <h1 id="startup-scan-title">{statusTitle(scan, starting, startError)}</h1>
          <p role="status" aria-label="Scan progress status" aria-live="polite" aria-atomic="true">
            {active ? `${percent.toFixed(0)}% complete` : statusTitle(scan, starting, startError)}
          </p>
        </div>

        <div
          className="progress-meter"
          role="progressbar"
          aria-label="Scan completion"
          aria-valuemin={0}
          aria-valuemax={100}
          aria-valuenow={Math.round(progressValue)}
          aria-valuetext={liveStatus}
        >
          <span style={{ width: `${progressValue}%` }} />
        </div>

        <div className="scan-counts">
          {metric('total', scan?.totalRecords ?? scan?.result?.totalRecords)}
          {metric('scanned', scan?.scannedRecords ?? scan?.result?.scannedRecords)}
          {metric('reused', scan?.reusedRecords ?? scan?.result?.reusedRecords)}
          {metric('parsed', scan?.parsedFb2Records ?? scan?.result?.parsedFb2Records)}
          {metric('added', scan?.addedRecords ?? scan?.result?.addedRecords)}
          {metric('updated', scan?.updatedRecords ?? scan?.result?.updatedRecords)}
          {metric('unavailable', scan?.markedUnavailableRecords ?? scan?.result?.markedUnavailableRecords)}
          {metric('skipped', scan?.skippedRecords ?? scan?.result?.skippedRecords)}
        </div>

        {scan?.current && (scan.current.archiveName || scan.current.entryName) && (
          <p className="current-source">
            {scan.current.archiveName}
            {scan.current.entryName ? ` / ${scan.current.entryName}` : ''}
          </p>
        )}

        {(error || warnings.length > 0) && (
          <div className="scan-result-box" role={error ? 'alert' : 'status'}>
            {error && <p className="result-error">{error}</p>}
            {warnings.slice(0, 8).map((warning) => (
              <p key={warning}>{warning}</p>
            ))}
          </div>
        )}

        <div className="startup-actions">
          {active && (
            <button type="button" className="secondary-button" onClick={onCancel} disabled={cancelling || scan?.status === 'cancelling'}>
              {cancelling || scan?.status === 'cancelling' ? 'Cancelling…' : 'Cancel scan'}
            </button>
          )}
          <button type="button" className="primary-button" onClick={onContinue} disabled={active || (!terminal && !startError)}>
            Continue
          </button>
        </div>
      </section>
    </main>
  );
}
