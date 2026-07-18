interface CoverPlaceholderProps {
  title: string;
  authors: string;
}

const initialPattern = /[\p{L}\p{N}]/u;

function placeholderInitial(title: string) {
  for (const char of title) {
    if (initialPattern.test(char)) {
      return char.toUpperCase();
    }
  }
  return '?';
}

function placeholderPaletteIndex(title: string, authors: string) {
  const seed = `${title || 'InpxWebReader'}|${authors}`;
  let hash = 17;
  for (let index = 0; index < seed.length; ++index) {
    hash = (hash * 31 + seed.charCodeAt(index)) | 0;
  }
  return Math.abs(hash) % 7;
}

export function CoverPlaceholder({ title, authors }: CoverPlaceholderProps) {
  return (
    <div className={`cover-fallback palette-${placeholderPaletteIndex(title, authors)}`} aria-hidden="true">
      <span>{placeholderInitial(title)}</span>
    </div>
  );
}
