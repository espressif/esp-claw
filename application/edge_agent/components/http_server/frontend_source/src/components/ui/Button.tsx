import { splitProps, type JSX, type ParentComponent } from 'solid-js';

type Variant = 'primary' | 'secondary' | 'ghost' | 'danger-ghost';
type Size = 'md' | 'sm' | 'xs';

type ButtonProps = JSX.ButtonHTMLAttributes<HTMLButtonElement> & {
  variant?: Variant;
  size?: Size;
  active?: boolean;
};

const baseClasses =
  'inline-flex items-center justify-center gap-1.5 font-medium font-sans whitespace-nowrap rounded-[var(--radius-sm)] transition disabled:cursor-not-allowed disabled:opacity-50';

const variantClasses: Record<Variant, string> = {
  primary:
    'bg-[var(--color-accent)] text-[var(--color-text-on-accent)] shadow-[0_0_20px_rgba(232,54,45,0.1)] hover:enabled:shadow-[0_0_28px_rgba(232,54,45,0.18)] hover:enabled:opacity-90',
  secondary:
    'border border-[var(--color-border-strong)] text-[var(--color-text-secondary)] bg-transparent hover:enabled:bg-white/[0.04] hover:enabled:text-[var(--color-text-primary)] hover:enabled:border-white/[0.14]',
  ghost:
    'bg-transparent text-[var(--color-text-secondary)] border border-transparent hover:enabled:bg-white/[0.04] hover:enabled:text-[var(--color-text-primary)]',
  'danger-ghost':
    'bg-transparent text-[var(--color-text-muted)] border border-transparent hover:enabled:bg-[rgba(248,113,113,0.08)] hover:enabled:text-[var(--color-danger)]',
};

const sizeClasses: Record<Size, string> = {
  md: 'px-4 py-2 text-sm',
  sm: 'px-3 py-1.5 text-[0.8rem]',
  xs: 'px-2 py-1 text-[0.75rem]',
};

export const Button: ParentComponent<ButtonProps> = (props) => {
  const [local, rest] = splitProps(props, ['variant', 'size', 'active', 'class', 'children']);
  return (
    <button
      type={rest.type ?? 'button'}
      {...rest}
      class={[
        baseClasses,
        variantClasses[local.variant ?? 'secondary'],
        sizeClasses[local.size ?? 'md'],
        local.active
          ? 'ring-1 ring-[var(--color-accent-soft)] text-[var(--color-text-primary)]'
          : '',
        local.class ?? '',
      ]
        .filter(Boolean)
        .join(' ')}
    >
      {local.children}
    </button>
  );
};
