import { useEffect, useState } from 'react';

export const mobileCatalogMediaQuery = '(max-width: 600px), ((max-width: 960px) and (max-height: 520px) and (pointer: coarse))';

function readMediaQuery(query: string) {
  return typeof window !== 'undefined'
    && typeof window.matchMedia === 'function'
    && window.matchMedia(query).matches;
}

export function useMediaQuery(query: string) {
  const [matches, setMatches] = useState(() => readMediaQuery(query));

  useEffect(() => {
    if (typeof window === 'undefined' || typeof window.matchMedia !== 'function') {
      return undefined;
    }

    const media = window.matchMedia(query);
    const update = () => setMatches(media.matches);
    update();

    media.addEventListener('change', update);
    return () => media.removeEventListener('change', update);
  }, [query]);

  return matches;
}
