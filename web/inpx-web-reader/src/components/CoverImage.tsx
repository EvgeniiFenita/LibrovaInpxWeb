import type { ReactNode } from 'react';
import { useEffect, useState } from 'react';

import { apiClient } from '../api/client';

interface CoverImageProps {
  coverUrl: string | null;
  token?: string;
  loading?: 'eager' | 'lazy';
  fallback: ReactNode;
}

export function CoverImage({
  coverUrl,
  token,
  loading = 'lazy',
  fallback
}: CoverImageProps) {
  const [objectUrl, setObjectUrl] = useState<string | null>(null);
  const [failed, setFailed] = useState(false);

  useEffect(() => {
    setObjectUrl(null);
    setFailed(false);

    if (!coverUrl) {
      return undefined;
    }

    let cancelled = false;
    let createdUrl: string | null = null;
    void apiClient.getCover(coverUrl, token)
      .then((blob) => {
        createdUrl = URL.createObjectURL(blob);
        if (cancelled) {
          URL.revokeObjectURL(createdUrl);
          return;
        }

        setObjectUrl(createdUrl);
      })
      .catch(() => {
        if (!cancelled) {
          setFailed(true);
        }
      });

    return () => {
      cancelled = true;
      if (createdUrl) {
        URL.revokeObjectURL(createdUrl);
      }
    };
  }, [coverUrl, token]);

  if (!coverUrl || !objectUrl || failed) {
    return <>{fallback}</>;
  }

  return <img src={objectUrl} alt="" loading={loading} onError={() => setFailed(true)} />;
}
