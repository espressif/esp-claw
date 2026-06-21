import type { Component, JSX } from 'solid-js';

type PageHeaderProps = {
  title: string;
  description?: string;
  actions?: JSX.Element;
};

export const PageHeader: Component<PageHeaderProps> = (props) => {
  return (
    <div class="flex flex-wrap items-start justify-between gap-4 px-6 py-5 border-b border-[var(--color-border-subtle)]">
      <div class="flex flex-col gap-1 min-w-0">
        <h2 class="text-lg font-semibold text-[var(--color-text-primary)] m-0">{props.title}</h2>
        {props.description && (
          <p class="text-[0.85rem] text-[var(--color-text-muted)] max-w-2xl m-0">
            {props.description}
          </p>
        )}
      </div>
      {props.actions && <div class="flex items-center gap-2 flex-wrap">{props.actions}</div>}
    </div>
  );
};
