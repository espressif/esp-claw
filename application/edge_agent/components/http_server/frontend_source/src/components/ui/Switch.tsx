import type { Component } from 'solid-js';

type SwitchProps = {
  checked: boolean;
  onChange?: (checked: boolean) => void;
  label?: string;
  labelClass?: string;
  hint?: string;
  disabled?: boolean;
  class?: string;
};

export const Switch: Component<SwitchProps> = (props) => {
  return (
    <label
      class={[
        'flex items-center gap-2 text-[0.82rem] text-[var(--color-text-secondary)] select-none',
        props.disabled ? 'opacity-50 cursor-not-allowed' : 'cursor-pointer',
        props.class ?? '',
      ]
        .filter(Boolean)
        .join(' ')}
    >
      <span
        class={[
          'relative inline-flex h-5 w-9 shrink-0 rounded-full transition border',
          props.checked
            ? 'bg-[var(--color-accent)]/80 border-[var(--color-accent-soft)]'
            : 'bg-white/5 border-[var(--color-border-subtle)]',
        ].join(' ')}
      >
        <span
          class={[
            'absolute left-px top-1/2 h-4 w-4 -translate-y-1/2 rounded-full bg-white shadow transition-transform',
            props.checked ? 'translate-x-4' : 'translate-x-0',
          ].join(' ')}
        />
        <input
          type="checkbox"
          class="sr-only"
          checked={props.checked}
          disabled={props.disabled}
          onChange={(event) => props.onChange?.(event.currentTarget.checked)}
        />
      </span>
      <span class="flex flex-col">
        {props.label && (
          <span class={[props.labelClass ?? 'text-[var(--color-text-primary)]', 'text-[0.82rem]'].join(' ')}>
            {props.label}
          </span>
        )}
        {props.hint && <span class="text-[0.7rem] text-[var(--color-text-muted)]">{props.hint}</span>}
      </span>
    </label>
  );
};
