import { createEffect, createSignal, lazy, onCleanup, onMount, Show, Suspense } from 'solid-js';
import type { Component } from 'solid-js';

import { fetchStatus, restartDevice } from './api/client';
import { Layout } from './components/layout/Layout';
import { LEAF_IDS } from './components/layout/Sidebar';
import {
  RestartOverlay,
  type RestartOverlayState,
} from './components/system/RestartOverlay';
import { Banner } from './components/ui/Banner';
import { ToastViewport } from './components/ui/ToastViewport';
import { t } from './i18n';
import { anyDirty, type TabId } from './state/dirty';
import { reloadCapabilities, reloadLuaModules, reloadStatus } from './state/config';
import { pushToast } from './state/toast';

const StatusPage = lazy(() => import('./pages/StatusPage').then((mod) => ({ default: mod.StatusPage })));
const BasicPage = lazy(() => import('./pages/BasicPage').then((mod) => ({ default: mod.BasicPage })));
const SearchPage = lazy(() => import('./pages/SearchPage').then((mod) => ({ default: mod.SearchPage })));
const MemoryPage = lazy(() => import('./pages/MemoryPage').then((mod) => ({ default: mod.MemoryPage })));
const LlmPage = lazy(() => import('./pages/LlmPage').then((mod) => ({ default: mod.LlmPage })));
const ImPage = lazy(() => import('./pages/ImPage').then((mod) => ({ default: mod.ImPage })));
const CapabilitiesPage = lazy(() =>
  import('./pages/CapabilitiesPage').then((mod) => ({ default: mod.CapabilitiesPage })),
);
const SkillsPage = lazy(() => import('./pages/SkillsPage').then((mod) => ({ default: mod.SkillsPage })));
const FilesPage = lazy(() => import('./pages/FilesPage').then((mod) => ({ default: mod.FilesPage })));
const WebImPage = lazy(() => import('./pages/WebImPage').then((mod) => ({ default: mod.WebImPage })));
const SetupWizardPage = lazy(() =>
  import('./pages/SetupWizardPage').then((mod) => ({ default: mod.SetupWizardPage })),
);

type RouteId = TabId | 'start';

function readTabFromHash(): RouteId {
  const hash = window.location.hash.replace(/^#\/?/, '');
  if (hash === 'start') return 'start';
  return LEAF_IDS.includes(hash as TabId) ? (hash as TabId) : 'status';
}

const App: Component = () => {
  const [currentTab, setCurrentTab] = createSignal<RouteId>(readTabFromHash());
  const [bootError, setBootError] = createSignal<string | null>(null);
  const [restartOverlay, setRestartOverlay] = createSignal<RestartOverlayState>({
    open: false,
    phase: 'requesting',
    error: null,
  });
  let restartTimer: ReturnType<typeof setTimeout> | null = null;
  let pollController: AbortController | null = null;
  let restartStartedAt = 0;
  let restartTarget: TabId | null = null;
  let restartActive = false;

  const clearRestartFlow = () => {
    restartActive = false;
    if (restartTimer) {
      clearTimeout(restartTimer);
      restartTimer = null;
    }
    pollController?.abort();
    pollController = null;
  };

  const closeRestartOverlay = () => {
    clearRestartFlow();
    setRestartOverlay({ open: false, phase: 'requesting', error: null });
  };

  const onHashChange = () => {
    const next = readTabFromHash();
    if (next === currentTab()) return;
    if (anyDirty()) {
      const ok = window.confirm(t('unsavedConfirmLeave') as string);
      if (!ok) {
        window.location.hash = '#' + currentTab();
        return;
      }
    }
    setCurrentTab(next);
  };

  onMount(() => {
    window.addEventListener('hashchange', onHashChange);
    void bootstrap();
  });

  onCleanup(() => {
    clearRestartFlow();
    window.removeEventListener('hashchange', onHashChange);
  });

  createEffect(() => {
    window.location.hash = '#' + currentTab();
  });

  const handleSelectTab = (next: TabId) => {
    setCurrentTab(next);
  };

  const bootstrap = async () => {
    const tasks: Array<[string, () => Promise<unknown>]> = [
      ['status', () => reloadStatus()],
      ['capabilities', () => reloadCapabilities()],
      ['luaModules', () => reloadLuaModules()],
    ];
    for (const [label, task] of tasks) {
      try {
        await task();
      } catch (err) {
        const message = (err as Error).message || 'Failed to initialise: ' + label;
        if (label === 'status') {
          setBootError(message);
        } else {
          pushToast(message, 'error', 4500);
        }
      }
    }
  };

  const scheduleNextPoll = () => {
    if (!restartActive) return;
    const deadlineAt = restartStartedAt + 30000;
    if (Date.now() >= deadlineAt) {
      setRestartOverlay({
        open: true,
        phase: 'error',
        error: t('restartOverlayTimeout') as string,
        deadlineAt,
      });
      return;
    }

    restartTimer = setTimeout(async () => {
      if (!restartActive) return;
      pollController?.abort();
      pollController = new AbortController();
      try {
        await fetchStatus(pollController.signal);
        await reloadStatus();
        closeRestartOverlay();
        if (restartTarget) {
          setCurrentTab(restartTarget);
        }
      } catch (err) {
        if (!restartActive) return;
        if ((err as Error).name !== 'AbortError') {
          setRestartOverlay({
            open: true,
            phase: 'polling',
            error: null,
            deadlineAt,
          });
        }
        scheduleNextPoll();
      }
    }, 2000);
  };

  const handleRestartRequest = async (targetTab?: TabId) => {
    clearRestartFlow();
    restartActive = true;
    restartTarget = targetTab ?? null;
    restartStartedAt = Date.now();
    const deadlineAt = restartStartedAt + 30000;
    setRestartOverlay({
      open: true,
      phase: 'requesting',
      error: null,
      deadlineAt,
    });

    try {
      await restartDevice();
      setRestartOverlay({
        open: true,
        phase: 'cooldown',
        error: null,
        deadlineAt,
      });
      restartTimer = setTimeout(() => {
        setRestartOverlay({
          open: true,
          phase: 'polling',
          error: null,
          deadlineAt,
        });
        scheduleNextPoll();
      }, 5000);
    } catch (err) {
      setRestartOverlay({
        open: true,
        phase: 'error',
        error: (err as Error).message,
        deadlineAt,
      });
    }
  };

  return (
    <>
      <Show
        when={currentTab() === 'start'}
        fallback={
          <Layout currentTab={currentTab() as TabId} onSelectTab={handleSelectTab}>
            <Show when={bootError()}>
              <div class="mb-4">
                <Banner kind="error" message={bootError() ?? undefined} />
              </div>
            </Show>
            <Suspense
              fallback={<div class="p-6 text-[var(--color-text-muted)]">{t('statusLoading')}</div>}
            >
              <Show when={currentTab() === 'status'}>
                <StatusPage onRestartRequest={() => void handleRestartRequest()} />
              </Show>
              <Show when={currentTab() === 'basic'}>
                <BasicPage />
              </Show>
              <Show when={currentTab() === 'llm'}>
                <LlmPage />
              </Show>
              <Show when={currentTab() === 'im'}>
                <ImPage />
              </Show>
              <Show when={currentTab() === 'search'}>
                <SearchPage />
              </Show>
              <Show when={currentTab() === 'memory'}>
                <MemoryPage />
              </Show>
              <Show when={currentTab() === 'webim'}>
                <WebImPage />
              </Show>
              <Show when={currentTab() === 'capabilities'}>
                <CapabilitiesPage />
              </Show>
              <Show when={currentTab() === 'skills'}>
                <SkillsPage />
              </Show>
              <Show when={currentTab() === 'files'}>
                <FilesPage />
              </Show>
            </Suspense>
          </Layout>
        }
      >
        <Suspense fallback={<div class="p-6 text-[var(--color-text-muted)]">{t('statusLoading')}</div>}>
          <SetupWizardPage onRestartRequest={(targetTab) => void handleRestartRequest(targetTab)} />
        </Suspense>
      </Show>
      <RestartOverlay state={restartOverlay()} onClose={closeRestartOverlay} />
      <ToastViewport />
    </>
  );
};

export default App;
