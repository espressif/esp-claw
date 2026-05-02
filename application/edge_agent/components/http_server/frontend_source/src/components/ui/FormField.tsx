import { createSignal, createUniqueId, Show, splitProps, type Component, type JSX } from 'solid-js';
import { t } from '../../i18n';

type CommonFieldProps = {
  label?: JSX.Element;
  hint?: JSX.Element;
  error?: JSX.Element;
  full?: boolean;
  fieldClass?: string;
};

type FieldRendererProps = CommonFieldProps & {
  children: (api: { id: string }) => JSX.Element;
};

export const Field: Component<FieldRendererProps> = (props) => {
  const id = createUniqueId();
  return (
    <div
      class={[
        'flex flex-col gap-1.5',
        props.full ? 'sm:col-span-2' : '',
        props.fieldClass ?? '',
      ]
        .filter(Boolean)
        .join(' ')}
    >
      <Show when={props.label}>
        <label for={id} class="text-[0.8rem] text-[var(--color-text-secondary)] font-medium">
          {props.label}
        </label>
      </Show>
      {props.children({ id })}
      <Show when={props.hint}>
        <small class="text-[0.72rem] leading-snug text-[var(--color-text-secondary)]/85">
          {props.hint}
        </small>
      </Show>
      <Show when={props.error}>
        <small class="text-[0.75rem] text-[var(--color-danger)]">{props.error}</small>
      </Show>
    </div>
  );
};

type TextInputProps = Omit<JSX.InputHTMLAttributes<HTMLInputElement>, 'class'> &
  CommonFieldProps & {
    inputClass?: string;
  };

export const TextInput: Component<TextInputProps> = (props) => {
  const [passwordVisible, setPasswordVisible] = createSignal(false);
  const [local, rest] = splitProps(props, [
    'label',
    'hint',
    'error',
    'full',
    'fieldClass',
    'inputClass',
    'type',
  ]);
  const isPasswordField = () => local.type === 'password';

  return (
    <Field
      label={local.label}
      hint={local.hint}
      error={local.error}
      full={local.full}
      fieldClass={local.fieldClass}
    >
      {({ id }) => (
        <div class="relative">
          <input
            id={id}
            type={isPasswordField() ? (passwordVisible() ? 'text' : 'password') : (local.type ?? 'text')}
            autocomplete="off"
            {...rest}
            class={[
              'w-full rounded-[var(--radius-sm)] border bg-[var(--color-bg-input)] px-3 py-2 text-sm text-[var(--color-text-primary)] transition',
              local.error
                ? 'border-[var(--color-danger)]'
                : 'border-[var(--color-border-subtle)] focus:border-[rgba(232,54,45,0.4)]',
              isPasswordField() ? 'pr-11' : '',
              local.inputClass ?? '',
            ]
              .filter(Boolean)
              .join(' ')}
          />
          <Show when={isPasswordField()}>
            <button
              type="button"
              class="absolute inset-y-0 right-0 flex w-10 items-center justify-center text-[var(--color-text-muted)] transition hover:text-[var(--color-text-primary)] focus:outline-none"
              onClick={() => setPasswordVisible((value) => !value)}
              aria-label={
                passwordVisible()
                  ? (t('passwordHide') as string)
                  : (t('passwordShow') as string)
              }
              title={
                passwordVisible()
                  ? (t('passwordHide') as string)
                  : (t('passwordShow') as string)
              }
            >
              <Show
                when={passwordVisible()}
                fallback={
                  <svg
                    xmlns="http://www.w3.org/2000/svg"
                    viewBox="0 0 24 24"
                    fill="none"
                    stroke="currentColor"
                    stroke-width="1.8"
                    stroke-linecap="round"
                    stroke-linejoin="round"
                    class="h-4.5 w-4.5"
                    aria-hidden="true"
                  >
                    <path d="M2 12s3.5-6 10-6 10 6 10 6-3.5 6-10 6-10-6-10-6Z" />
                    <circle cx="12" cy="12" r="3" />
                  </svg>
                }
              >
                <svg
                  xmlns="http://www.w3.org/2000/svg"
                  viewBox="0 0 24 24"
                  fill="none"
                  stroke="currentColor"
                  stroke-width="1.8"
                  stroke-linecap="round"
                  stroke-linejoin="round"
                  class="h-4.5 w-4.5"
                  aria-hidden="true"
                >
                  <path d="M3 3l18 18" />
                  <path d="M10.6 10.7a3 3 0 0 0 4.2 4.2" />
                  <path d="M9.9 5.2A10.6 10.6 0 0 1 12 5c6.5 0 10 7 10 7a17.2 17.2 0 0 1-3.2 3.8" />
                  <path d="M6.2 6.3A17.3 17.3 0 0 0 2 12s3.5 7 10 7a10.7 10.7 0 0 0 5-1.2" />
                </svg>
              </Show>
            </button>
          </Show>
        </div>
      )}
    </Field>
  );
};

type SelectInputProps = Omit<JSX.SelectHTMLAttributes<HTMLSelectElement>, 'class'> &
  CommonFieldProps & {
    inputClass?: string;
  };

export const SelectInput: Component<SelectInputProps> = (props) => {
  const [local, rest] = splitProps(props, [
    'label',
    'hint',
    'error',
    'full',
    'fieldClass',
    'inputClass',
    'children',
  ]);
  return (
    <Field
      label={local.label}
      hint={local.hint}
      error={local.error}
      full={local.full}
      fieldClass={local.fieldClass}
    >
      {({ id }) => (
        <select
          id={id}
          {...rest}
          class={[
            'w-full rounded-[var(--radius-sm)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-input)] px-3 py-2 text-sm text-[var(--color-text-primary)] transition focus:border-[rgba(232,54,45,0.4)]',
            local.inputClass ?? '',
          ]
            .filter(Boolean)
            .join(' ')}
        >
          {local.children}
        </select>
      )}
    </Field>
  );
};

type TextAreaProps = Omit<JSX.TextareaHTMLAttributes<HTMLTextAreaElement>, 'class'> &
  CommonFieldProps & {
    inputClass?: string;
  };

export const TextArea: Component<TextAreaProps> = (props) => {
  const [local, rest] = splitProps(props, [
    'label',
    'hint',
    'error',
    'full',
    'fieldClass',
    'inputClass',
  ]);
  return (
    <Field
      label={local.label}
      hint={local.hint}
      error={local.error}
      full={local.full}
      fieldClass={local.fieldClass}
    >
      {({ id }) => (
        <textarea
          id={id}
          spellcheck={false}
          {...rest}
          class={[
            'w-full min-h-[200px] rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-input)] px-4 py-3 text-[0.88rem] leading-6 text-[var(--color-text-primary)] transition font-mono focus:border-[rgba(232,54,45,0.4)]',
            local.inputClass ?? '',
          ]
            .filter(Boolean)
            .join(' ')}
        />
      )}
    </Field>
  );
};
