import type { JSX, ParentComponent } from 'solid-js';

type LabelLinkProps = Omit<JSX.AnchorHTMLAttributes<HTMLAnchorElement>, 'onClick'> & {
  onClick?: JSX.EventHandler<HTMLAnchorElement, MouseEvent>;
};

export const LabelLink: ParentComponent<LabelLinkProps> = (props) => (
  <a
    {...props}
    target={props.target ?? '_blank'}
    rel={props.rel ?? 'noopener noreferrer'}
    class={['ml-2 text-[var(--color-accent-soft)] hover:underline', props.class ?? '']
      .filter(Boolean)
      .join(' ')}
    onClick={(event) => {
      event.stopPropagation();
      props.onClick?.(event);
    }}
  >
    {props.children}
  </a>
);
