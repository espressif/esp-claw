import { createEffect, onCleanup, Show, type JSX, type ParentComponent } from 'solid-js';
import { Portal } from 'solid-js/web';

type ModalProps = {
  open: boolean;
  onClose: () => void;
  title?: JSX.Element;
  subtitle?: JSX.Element;
  actions?: JSX.Element;
  widthClass?: string;
};

export const Modal: ParentComponent<ModalProps> = (props) => {
  createEffect(() => {
    if (props.open) {
      document.body.style.overflow = 'hidden';
    } else {
      document.body.style.overflow = '';
    }
  });
  onCleanup(() => {
    document.body.style.overflow = '';
  });

  return (
    <Show when={props.open}>
      <Portal>
        <div
          class="fixed inset-0 z-[1000] flex items-center justify-center p-6 bg-black/60 backdrop-blur-sm"
          onClick={(event) => {
            if (event.target === event.currentTarget) props.onClose();
          }}
          onKeyDown={(event) => {
            if (event.key === 'Escape') props.onClose();
          }}
          tabindex={-1}
          role="dialog"
          aria-modal="true"
        >
          <div
            class={[
              'flex flex-col rounded-[var(--radius-lg)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-card)] shadow-[0_24px_60px_rgba(0,0,0,0.35)] overflow-hidden',
              props.widthClass ?? 'w-full max-w-3xl max-h-[calc(100vh-3rem)]',
            ].join(' ')}
          >
            <Show when={props.title || props.subtitle}>
              <div class="flex items-start justify-between gap-4 p-5 pb-3 flex-wrap">
                <div class="flex flex-col gap-1">
                  <Show when={props.title}>
                    <h2 class="text-base font-bold text-[var(--color-text-primary)] m-0">
                      {props.title}
                    </h2>
                  </Show>
                  <Show when={props.subtitle}>
                    <div class="text-[0.8rem] text-[var(--color-text-muted)]">{props.subtitle}</div>
                  </Show>
                </div>
                <button
                  type="button"
                  onClick={props.onClose}
                  class="text-[var(--color-text-secondary)] hover:text-[var(--color-text-primary)] text-[0.85rem] px-2 py-1 rounded-[var(--radius-sm)] hover:bg-white/5"
                >
                  ✕
                </button>
              </div>
            </Show>
            <div class="flex-1 overflow-auto">{props.children}</div>
            <Show when={props.actions}>
              <div class="flex flex-wrap gap-2 justify-end px-5 py-4 border-t border-[var(--color-border-subtle)]">
                {props.actions}
              </div>
            </Show>
          </div>
        </div>
      </Portal>
    </Show>
  );
};
