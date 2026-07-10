import type { Component, JSX } from 'solid-js';

type TabShellProps = {
  children: JSX.Element;
  class?: string;
};

export const TabShell: Component<TabShellProps> = (props) => {
  return (
    <section
      class={[
        'bg-[var(--color-bg-card)] border border-[var(--color-border-subtle)] rounded-[var(--radius-lg)] overflow-hidden shadow-[0_0_30px_rgba(0,0,0,0.25)]',
        props.class ?? '',
      ]
        .filter(Boolean)
        .join(' ')}
    >
      {props.children}
    </section>
  );
};
