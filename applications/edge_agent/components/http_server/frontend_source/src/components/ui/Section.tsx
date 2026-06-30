import { createSignal, Show, type JSX, type ParentComponent } from 'solid-js';

type Variant = 'primary' | 'nested';

type SectionProps = {
  title: JSX.Element;
  defaultOpen?: boolean;
  variant?: Variant;
  actions?: JSX.Element;
  note?: JSX.Element;
  class?: string;
};

const chevron = (
  <svg
    class="w-4 h-4 text-[var(--color-text-muted)] transition-transform duration-200"
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    stroke-width="2"
    stroke-linecap="round"
    stroke-linejoin="round"
  >
    <path d="M6 9l6 6 6-6" />
  </svg>
);

export const Section: ParentComponent<SectionProps> = (props) => {
  const variant = () => props.variant ?? 'primary';
  const [open, setOpen] = createSignal(props.defaultOpen ?? true);

  const headerClasses = () =>
    variant() === 'primary'
      ? 'flex items-center justify-between gap-3 px-5 py-3 cursor-pointer select-none hover:bg-white/[0.02] transition'
      : 'flex items-center justify-between gap-3 px-4 py-2.5 cursor-pointer select-none text-[var(--color-text-secondary)] hover:text-[var(--color-text-primary)] transition text-[0.82rem] font-semibold';

  const bodyClasses = () => (variant() === 'primary' ? 'px-5 pb-5' : 'px-4 pb-4');

  const rootClasses = () =>
    variant() === 'primary'
      ? ['border-b border-[var(--color-border-subtle)] last:border-b-0', props.class ?? '']
          .filter(Boolean)
          .join(' ')
      : [
          'mt-3 rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-surface)]',
          props.class ?? '',
        ]
          .filter(Boolean)
          .join(' ');

  return (
    <div class={rootClasses()}>
      <button
        type="button"
        class={`${headerClasses()} w-full`}
        onClick={() => setOpen(!open())}
        aria-expanded={open()}
      >
        <Show
          when={variant() === 'primary'}
          fallback={
            <span class="flex items-center gap-2">
              <span>{props.title}</span>
            </span>
          }
        >
          <span class="flex items-center gap-2 text-[0.72rem] font-bold uppercase tracking-[0.1em] text-[var(--color-accent-soft)]">
            <span>{props.title}</span>
            <span class="h-px flex-1 bg-[var(--color-border-accent)] min-w-8" />
          </span>
        </Show>
        <span class="flex items-center gap-2">
          <Show when={props.actions}>{props.actions}</Show>
          <span class={open() ? 'rotate-180' : ''}>{chevron}</span>
        </span>
      </button>
      <Show when={open()}>
        <div class={bodyClasses()}>
          <Show when={props.note}>
            <p class="mb-3 text-[0.78rem] text-[var(--color-text-muted)]">{props.note}</p>
          </Show>
          {props.children}
        </div>
      </Show>
    </div>
  );
};
