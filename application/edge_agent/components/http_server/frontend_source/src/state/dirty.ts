import { createEffect, createSignal, onCleanup } from 'solid-js';
import { t } from '../i18n';

export type TabId =
  | 'status'
  | 'basic'
  | 'llm'
  | 'im'
  | 'webreq'
  | 'memory'
  | 'webim'
  | 'capabilities'
  | 'skills'
  | 'files';

const [dirtyTabs, setDirtyTabs] = createSignal<Record<TabId, boolean>>({
  status: false,
  basic: false,
  llm: false,
  im: false,
  webreq: false,
  memory: false,
  webim: false,
  capabilities: false,
  skills: false,
  files: false,
});

export function isDirty(tab: TabId) {
  return dirtyTabs()[tab];
}

export function anyDirty() {
  return Object.values(dirtyTabs()).some(Boolean);
}

export function markDirty(tab: TabId, dirty: boolean) {
  if (dirtyTabs()[tab] === dirty) return;
  setDirtyTabs({ ...dirtyTabs(), [tab]: dirty });
}

export function dirtySnapshot() {
  return dirtyTabs();
}

export function useDirtyTracker(tab: TabId, dirty: () => boolean) {
  createEffect(() => {
    markDirty(tab, dirty());
  });
  onCleanup(() => {
    markDirty(tab, false);
  });
}

export function installUnsavedGuard() {
  window.addEventListener('beforeunload', (event) => {
    if (!anyDirty()) return;
    event.preventDefault();
    event.returnValue = t('unsavedConfirmReload');
  });
}

export function confirmTabSwitch() {
  if (!anyDirty()) return true;
  const message = t('unsavedConfirmLeave') as string;
  return window.confirm(message);
}
