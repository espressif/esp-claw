import { Book, House, Languages } from 'lucide-solid';
import { createEffect, createSignal, For, onCleanup } from 'solid-js';
import { currentLocale, LOCALES, setLocale, t } from '../../i18n';

const iconButtonClass =
  'inline-flex items-center justify-center w-8 h-8 rounded-[var(--radius-sm)] border border-[var(--color-border-subtle)] text-[var(--color-text-secondary)] hover:bg-white/[0.04] hover:border-white/[0.12] hover:text-[var(--color-text-primary)] transition';

function GitHubIcon() {
  return (
    <svg class="w-4 h-4" role="img" viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" fill="currentColor" aria-hidden="true">
      <title>GitHub</title>
      <path d="M12 .297c-6.63 0-12 5.373-12 12 0 5.303 3.438 9.8 8.205 11.385.6.113.82-.258.82-.577 0-.285-.01-1.04-.015-2.04-3.338.724-4.042-1.61-4.042-1.61C4.422 18.07 3.633 17.7 3.633 17.7c-1.087-.744.084-.729.084-.729 1.205.084 1.838 1.236 1.838 1.236 1.07 1.835 2.809 1.305 3.495.998.108-.776.417-1.305.76-1.605-2.665-.3-5.466-1.332-5.466-5.93 0-1.31.465-2.38 1.235-3.22-.135-.303-.54-1.523.105-3.176 0 0 1.005-.322 3.3 1.23.96-.267 1.98-.399 3-.405 1.02.006 2.04.138 3 .405 2.28-1.552 3.285-1.23 3.285-1.23.645 1.653.24 2.873.12 3.176.765.84 1.23 1.91 1.23 3.22 0 4.61-2.805 5.625-5.475 5.92.42.36.81 1.096.81 2.22 0 1.606-.015 2.896-.015 3.286 0 .315.21.69.825.57C20.565 22.092 24 17.592 24 12.297c0-6.627-5.373-12-12-12" />
    </svg>
  );
}

export function LanguageSwitcher() {
  const [open, setOpen] = createSignal(false);
  let rootRef: HTMLDivElement | undefined;

  const handleDocClick = (event: MouseEvent) => {
    if (rootRef && !rootRef.contains(event.target as Node)) {
      setOpen(false);
    }
  };

  createEffect(() => {
    if (open()) {
      document.addEventListener('click', handleDocClick);
    } else {
      document.removeEventListener('click', handleDocClick);
    }
  });

  onCleanup(() => document.removeEventListener('click', handleDocClick));

  const currentLabel = () => LOCALES.find((loc) => loc.id === currentLocale())?.label ?? 'English';
  const localePath = () => currentLocale();

  return (
    <div class="flex items-center gap-2">
      <a
        href={`https://esp-claw.com/${localePath()}/`}
        target="_blank"
        rel="noreferrer"
        class={iconButtonClass}
        title={t('externalHome') as string}
        aria-label={t('externalHome') as string}
      >
        <House class="w-4 h-4" />
      </a>
      <a
        href={`https://esp-claw.com/${localePath()}/tutorial`}
        target="_blank"
        rel="noreferrer"
        class={iconButtonClass}
        title={t('externalDocs') as string}
        aria-label={t('externalDocs') as string}
      >
        <Book class="w-4 h-4" />
      </a>
      <a
        href="https://github.com/espressif/esp-claw"
        target="_blank"
        rel="noreferrer"
        class={iconButtonClass}
        title={t('externalGithub') as string}
        aria-label={t('externalGithub') as string}
      >
        <GitHubIcon />
      </a>
      <div class="relative" ref={rootRef}>
        <button
          type="button"
          class="inline-flex items-center gap-1 text-[0.78rem] text-[var(--color-text-secondary)] px-2.5 py-1.5 rounded-[var(--radius-sm)] border border-[var(--color-border-subtle)] hover:bg-white/[0.04] hover:border-white/[0.12]"
          onClick={() => setOpen(!open())}
          aria-expanded={open()}
        >
          <Languages class="w-4 h-4" />
          <span>{currentLabel()}</span>
          <svg width="10" height="10" viewBox="0 0 10 10" fill="none" class="opacity-75">
            <path d="M2.5 4L5 6.5L7.5 4" stroke="currentColor" stroke-width="1.2" stroke-linecap="round" stroke-linejoin="round" />
          </svg>
        </button>
        {open() && (
          <ul class="absolute right-0 top-[calc(100%+6px)] min-w-[9rem] py-1 list-none bg-[var(--color-bg-card)] border border-[var(--color-border-subtle)] rounded-[var(--radius-md)] shadow-[0_12px_40px_rgba(0,0,0,0.45)] z-50">
            <For each={LOCALES}>
              {(locale) => (
                <li>
                  <button
                    type="button"
                    class={[
                      'w-full text-left px-3.5 py-1.5 text-[0.82rem] hover:bg-white/[0.06] hover:text-[var(--color-text-primary)] transition',
                      locale.id === currentLocale()
                        ? 'text-[var(--color-accent-soft)] font-semibold'
                        : 'text-[var(--color-text-secondary)]',
                    ].join(' ')}
                    disabled={locale.id === currentLocale()}
                    onClick={() => {
                      setLocale(locale.id);
                      setOpen(false);
                    }}
                  >
                    {locale.label}
                  </button>
                </li>
              )}
            </For>
          </ul>
        )}
      </div>
    </div>
  );
}
