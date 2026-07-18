import { AlertCircle, CheckCircle2, X } from 'lucide-react';
import { useEffect, useRef } from 'react';

export interface ToastMessage {
  id: number;
  tone: 'success' | 'error' | 'info';
  text: string;
}

export const toastAutoDismissMs = 4500;

interface ToastRegionProps {
  toasts: ToastMessage[];
  onDismiss: (id: number) => void;
}

export function ToastRegion({ toasts, onDismiss }: ToastRegionProps) {
  const onDismissRef = useRef(onDismiss);
  const dismissTimersRef = useRef(new Map<number, number>());

  useEffect(() => {
    onDismissRef.current = onDismiss;
  }, [onDismiss]);

  useEffect(() => {
    const activeIds = new Set(toasts.map((toast) => toast.id));

    for (const [id, timer] of dismissTimersRef.current) {
      if (!activeIds.has(id)) {
        window.clearTimeout(timer);
        dismissTimersRef.current.delete(id);
      }
    }

    for (const toast of toasts) {
      if (!dismissTimersRef.current.has(toast.id)) {
        const timer = window.setTimeout(() => {
          dismissTimersRef.current.delete(toast.id);
          onDismissRef.current(toast.id);
        }, toastAutoDismissMs);
        dismissTimersRef.current.set(toast.id, timer);
      }
    }
  }, [toasts]);

  useEffect(() => {
    return () => {
      for (const timer of dismissTimersRef.current.values()) {
        window.clearTimeout(timer);
      }
      dismissTimersRef.current.clear();
    };
  }, []);

  return (
    <div className="toast-region" aria-live="polite" aria-label="Notifications">
      {toasts.map((toast) => (
        <div key={toast.id} className={`toast ${toast.tone}`}>
          {toast.tone === 'success' ? <CheckCircle2 aria-hidden="true" size={18} /> : <AlertCircle aria-hidden="true" size={18} />}
          <span>{toast.text}</span>
          <button type="button" className="icon-button tiny" onClick={() => onDismiss(toast.id)} aria-label="Dismiss notification">
            <X aria-hidden="true" size={16} />
          </button>
        </div>
      ))}
    </div>
  );
}
