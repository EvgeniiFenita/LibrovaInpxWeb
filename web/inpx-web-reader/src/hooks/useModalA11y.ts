import { useCallback, useEffect, useRef, useState } from 'react';

interface ModalA11yOptions {
  active?: boolean;
  onClose?: () => void;
}

const focusableSelector = [
  'a[href]',
  'button:not([disabled])',
  'input:not([disabled])',
  'select:not([disabled])',
  'textarea:not([disabled])',
  '[tabindex]:not([tabindex="-1"])'
].join(',');

function focusableElements(root: HTMLElement) {
  return Array.from(root.querySelectorAll<HTMLElement>(focusableSelector))
    .filter((element) => !element.hasAttribute('disabled') && element.getAttribute('aria-hidden') !== 'true');
}

function collectSiblingElements(modalRoot: HTMLElement) {
  const siblings = new Set<HTMLElement>();
  let current: HTMLElement | null = modalRoot;

  while (current && current !== document.body) {
    const parent: HTMLElement | null = current.parentElement;
    if (!parent) {
      break;
    }

    for (const child of Array.from(parent.children)) {
      if (child instanceof HTMLElement && child !== current) {
        siblings.add(child);
      }
    }

    current = parent;
  }

  return Array.from(siblings);
}

type InertElement = HTMLElement & { inert: boolean };

export function useModalA11y<T extends HTMLElement>({ active = true, onClose }: ModalA11yOptions = {}) {
  const [dialog, setDialog] = useState<T | null>(null);
  const modalRef = useCallback((element: T | null) => {
    setDialog(element);
  }, []);
  const onCloseRef = useRef(onClose);

  useEffect(() => {
    onCloseRef.current = onClose;
  }, [onClose]);

  useEffect(() => {
    if (!active || !dialog) {
      return undefined;
    }

    const modalRoot = dialog.closest<HTMLElement>('[data-modal-root]') ?? dialog;
    const previouslyFocused = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    const siblingStates = collectSiblingElements(modalRoot).map((element) => ({
      element,
      ariaHidden: element.getAttribute('aria-hidden'),
      inert: 'inert' in element ? (element as InertElement).inert : undefined
    }));

    for (const { element } of siblingStates) {
      element.setAttribute('aria-hidden', 'true');
      if ('inert' in element) {
        (element as InertElement).inert = true;
      }
    }

    const focusInitialElement = () => {
      const [firstFocusable] = focusableElements(dialog);
      (firstFocusable ?? dialog).focus({ preventScroll: true });
    };
    const animationFrame = window.requestAnimationFrame(focusInitialElement);

    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape' && onCloseRef.current) {
        event.preventDefault();
        onCloseRef.current();
        return;
      }

      if (event.key !== 'Tab') {
        return;
      }

      const elements = focusableElements(dialog);
      if (elements.length === 0) {
        event.preventDefault();
        dialog.focus({ preventScroll: true });
        return;
      }

      const firstElement = elements[0];
      const lastElement = elements[elements.length - 1];
      const activeElement = document.activeElement;
      if (event.shiftKey && activeElement === firstElement) {
        event.preventDefault();
        lastElement.focus({ preventScroll: true });
      } else if (!event.shiftKey && activeElement === lastElement) {
        event.preventDefault();
        firstElement.focus({ preventScroll: true });
      }
    };

    document.addEventListener('keydown', handleKeyDown, true);
    return () => {
      window.cancelAnimationFrame(animationFrame);
      document.removeEventListener('keydown', handleKeyDown, true);
      for (const { element, ariaHidden, inert } of siblingStates) {
        if (ariaHidden === null) {
          element.removeAttribute('aria-hidden');
        } else {
          element.setAttribute('aria-hidden', ariaHidden);
        }
        if (inert !== undefined) {
          (element as InertElement).inert = inert;
        }
      }
      if (previouslyFocused?.isConnected) {
        previouslyFocused.focus({ preventScroll: true });
      }
    };
  }, [active, dialog]);

  return modalRef;
}
