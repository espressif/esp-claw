import { Show, type Component, type JSX } from 'solid-js';

type BannerProps = {
  kind?: 'success' | 'error' | 'info';
  message?: string;
  children?: JSX.Element;
  class?: string;
};

export const Banner: Component<BannerProps> = (props) => {
  const kind = () => props.kind ?? 'info';
  return (
    <Show when={props.message || props.children}>
      <div
        role={kind() === 'error' ? 'alert' : 'status'}
        class={[
          'px-4 py-3 rounded-[var(--radius-md)] text-[0.85rem] border',
          kind() === 'success'
            ? 'bg-[var(--color-green-dim)] border-[rgba(104,211,145,0.2)] text-[var(--color-green)]'
            : '',
          kind() === 'error'
            ? 'bg-[var(--color-accent-dim)] border-[var(--color-border-accent)] text-[var(--color-danger)]'
            : '',
          kind() === 'info'
            ? 'bg-white/[0.03] border-[var(--color-border-subtle)] text-[var(--color-text-secondary)]'
            : '',
          props.class ?? '',
        ]
          .filter(Boolean)
          .join(' ')}
      >
        {props.children ?? props.message}
      </div>
    </Show>
  );
};
