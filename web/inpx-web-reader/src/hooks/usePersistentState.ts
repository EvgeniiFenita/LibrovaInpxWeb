import { useEffect, useState } from 'react';

export function usePersistentState(key: string, fallback: string) {
  const [value, setValue] = useState(() => {
    try {
      return window.localStorage.getItem(key) ?? fallback;
    } catch {
      return fallback;
    }
  });

  useEffect(() => {
    try {
      if (value) {
        window.localStorage.setItem(key, value);
      } else {
        window.localStorage.removeItem(key);
      }
    } catch {
      // Local storage is optional for the UI; API calls still work without persistence.
    }
  }, [key, value]);

  return [value, setValue] as const;
}
