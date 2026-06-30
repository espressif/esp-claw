import { For, type Component, type JSX } from 'solid-js';

export type ConfigTableColumn = {
  label: JSX.Element;
  class?: string;
};

type ConfigTableProps = {
  columns: ConfigTableColumn[];
  children: JSX.Element;
};

export const ConfigTable: Component<ConfigTableProps> = (props) => {
  return (
    <div class="overflow-hidden rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-surface)]">
      <table class="w-full">
        <thead>
          <tr class="bg-[var(--color-bg-card)]">
            <For each={props.columns}>
              {(column) => (
                <th
                  class={[
                    'px-4 py-2.5 text-left text-[0.72rem] font-bold uppercase tracking-wider text-[var(--color-text-muted)]',
                    column.class ?? '',
                  ].join(' ')}
                >
                  {column.label}
                </th>
              )}
            </For>
          </tr>
        </thead>
        <tbody>{props.children}</tbody>
      </table>
    </div>
  );
};
