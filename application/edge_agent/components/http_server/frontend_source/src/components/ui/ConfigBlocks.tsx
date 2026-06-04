import {
  createSignal,
  createEffect,
  Show,
  type Component,
  type JSX,
  type ParentComponent,
} from 'solid-js';

/* Aligns with ImPage: flat list + row header + tinted body. */

const ROW_CLASS =
  'flex items-center gap-3 py-3 px-5 hover:bg-white/[0.015] transition-colors select-none';

const BODY_CLASS = 'px-5 pb-5 pt-1 bg-white/[0.015] border-t border-[var(--color-border-subtle)]';

export const ChevronIcon: Component<{ open: boolean }> = (props) => (
  <svg
    class={[
      'w-3.5 h-3.5 transition-transform duration-150 shrink-0',
      props.open ? 'rotate-90' : '',
    ].join(' ')}
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    stroke-width="2.5"
    stroke-linecap="round"
    stroke-linejoin="round"
    aria-hidden="true"
  >
    <path d="M9 6l6 6-6 6" />
  </svg>
);

type StaticConfigBlockProps = { title: string; class?: string };

/** Non-collapsible block: title row + always-visible body (ImPage-style, no chevron). */
export const StaticConfigBlock: ParentComponent<StaticConfigBlockProps> = (props) => (
  <div class={props.class}>
    <div class="flex items-center gap-3 py-3 px-5 select-none">
      <span class="text-[0.9rem] font-semibold text-[var(--color-text-primary)] truncate">
        {props.title}
      </span>
    </div>
    <div class={BODY_CLASS}>{props.children}</div>
  </div>
);

type CollapsibleConfigBlockProps = {
  title: string;
  defaultOpen?: boolean;
  open?: boolean;
  onOpenChange?: (open: boolean) => void;
  class?: string;
  /** Rendered to the right of the title, before the chevron; clicks do not toggle the block. */
  headerEnd?: JSX.Element;
};

/** Collapsible block: clickable row + chevron (same interaction pattern as ImPage PlatformRow). */
export const CollapsibleConfigBlock: ParentComponent<CollapsibleConfigBlockProps> = (props) => {
  const [open, setOpen] = createSignal(props.defaultOpen ?? true);

  createEffect(() => {
    if (props.open !== undefined) {
      setOpen(props.open);
    }
  });

  const applyOpen = (next: boolean) => {
    if (props.open === undefined) {
      setOpen(next);
    }
    props.onOpenChange?.(next);
  };

  const toggle = () => applyOpen(!open());

  return (
    <div class={props.class}>
      <div
        class={[ROW_CLASS, 'cursor-pointer'].join(' ')}
        onClick={toggle}
        role="button"
        aria-expanded={open()}
        tabIndex={0}
        onKeyDown={(e) => {
          if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            toggle();
          }
        }}
      >
        <span class="flex-1 min-w-0 text-left text-[0.9rem] font-semibold text-[var(--color-text-primary)] truncate">
          {props.title}
        </span>
        <Show when={props.headerEnd != null}>
          <div
            class="flex items-center gap-2 shrink-0"
            onClick={(e) => e.stopPropagation()}
            onKeyDown={(e) => e.stopPropagation()}
          >
            {props.headerEnd}
          </div>
        </Show>
        <div class="flex items-center text-[var(--color-text-muted)] shrink-0">
          <ChevronIcon open={open()} />
        </div>
      </div>
      <Show when={open()}>
        <div class={BODY_CLASS}>{props.children}</div>
      </Show>
    </div>
  );
};
