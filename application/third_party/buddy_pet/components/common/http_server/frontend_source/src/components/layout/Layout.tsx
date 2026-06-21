import { Show, createEffect, createSignal, onCleanup, type Component, type JSX } from 'solid-js';
import { Sidebar } from './Sidebar';
import { StatusBar } from './StatusBar';
import { LanguageSwitcher } from './LanguageSwitcher';
import { t } from '../../i18n';
import { confirmTabSwitch, type TabId } from '../../state/dirty';

type LayoutProps = {
  currentTab: TabId;
  onSelectTab: (id: TabId) => void;
  children: JSX.Element;
};

const SIDEBAR_STORAGE_KEY = 'esp-claw-sidebar-collapsed';

export const Layout: Component<LayoutProps> = (props) => {
  const [collapsed, setCollapsed] = createSignal(
    (() => {
      try {
        return localStorage.getItem(SIDEBAR_STORAGE_KEY) === '1';
      } catch {
        return false;
      }
    })(),
  );
  const [mobileOpen, setMobileOpen] = createSignal(false);

  createEffect(() => {
    try {
      localStorage.setItem(SIDEBAR_STORAGE_KEY, collapsed() ? '1' : '0');
    } catch {
      /* ignore */
    }
  });

  createEffect(() => {
    if (!mobileOpen()) return;
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') setMobileOpen(false);
    };
    document.addEventListener('keydown', onKeyDown);
    onCleanup(() => document.removeEventListener('keydown', onKeyDown));
  });

  const handleSelect = (next: TabId) => {
    if (next === props.currentTab) return;
    if (!confirmTabSwitch()) return;
    props.onSelectTab(next);
    setMobileOpen(false);
  };

  return (
    <div class="flex h-screen min-h-screen overflow-hidden">
      <Show when={mobileOpen()}>
        <button
          type="button"
          class="fixed inset-0 z-40 bg-black/55 lg:hidden"
          aria-label={t('closeMenu') as string}
          onClick={() => setMobileOpen(false)}
        />
      </Show>
      <Sidebar
        current={props.currentTab}
        onSelect={handleSelect}
        collapsed={collapsed()}
        onToggleCollapsed={() => setCollapsed(!collapsed())}
        mobileOpen={mobileOpen()}
        onCloseMobile={() => setMobileOpen(false)}
      />
      <Sidebar
        current={props.currentTab}
        onSelect={handleSelect}
        collapsed={collapsed()}
        onToggleCollapsed={() => setCollapsed(!collapsed())}
      />
      <div class="flex-1 flex flex-col min-w-0">
        <StatusBar
          leadingSlot={
            <button
              type="button"
              class="inline-flex lg:hidden items-center justify-center w-9 h-9 rounded-[var(--radius-sm)] text-[var(--color-text-muted)] hover:bg-white/5 hover:text-[var(--color-text-primary)] transition shrink-0"
              aria-label={t('openMenu') as string}
              aria-expanded={mobileOpen()}
              onClick={() => setMobileOpen(true)}
            >
              <svg
                class="w-5 h-5"
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                stroke-width="2"
                stroke-linecap="round"
                stroke-linejoin="round"
              >
                <path d="M3 6h18" />
                <path d="M3 12h18" />
                <path d="M3 18h18" />
              </svg>
            </button>
          }
          slot={() => <LanguageSwitcher />}
        />
        <main class="flex-1 overflow-auto">
          <div class="max-w-5xl mx-auto w-full p-4 sm:p-6">{props.children}</div>
        </main>
      </div>
    </div>
  );
};
