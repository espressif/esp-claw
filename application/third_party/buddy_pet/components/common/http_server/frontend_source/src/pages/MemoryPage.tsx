import { createEffect, createSignal, For, onCleanup, Show, type Component } from 'solid-js';
import { createStore } from 'solid-js/store';
import { t } from '../i18n';
import { fetchFileContent, fetchFileList, saveFileContent, type FileEntry } from '../api/client';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { CollapsibleConfigBlock } from '../components/ui/ConfigBlocks';
import { Button } from '../components/ui/Button';
import { Banner } from '../components/ui/Banner';
import { markDirty } from '../state/dirty';
import { pushToast } from '../state/toast';

type MemoryFile = {
  path: string;
  titleKey: 'memoryCardLongTerm' | 'memoryCardSoul' | 'memoryCardIdentity' | 'memoryCardUserInfo';
  editable: boolean;
};

const MEMORY_FILES: MemoryFile[] = [
  { path: '/memory/memory.md', titleKey: 'memoryCardLongTerm', editable: false },
  { path: '/memory/soul.md', titleKey: 'memoryCardSoul', editable: true },
  { path: '/memory/identity.md', titleKey: 'memoryCardIdentity', editable: true },
  { path: '/memory/user.md', titleKey: 'memoryCardUserInfo', editable: true },
];

type MemoryStateItem = {
  exists: boolean;
  baseline: string;
  current: string;
  loading: boolean;
  saving: boolean;
  error: string | null;
};

function initialState(): Record<string, MemoryStateItem> {
  const out: Record<string, MemoryStateItem> = {};
  for (const file of MEMORY_FILES) {
    out[file.path] = {
      exists: false,
      baseline: '',
      current: '',
      loading: false,
      saving: false,
      error: null,
    };
  }
  return out;
}

export const MemoryPage: Component = () => {
  const [state, setState] = createStore<Record<string, MemoryStateItem>>(initialState());
  const [globalError, setGlobalError] = createSignal<string | null>(null);
  const [refreshing, setRefreshing] = createSignal(false);
  const [initialized, setInitialized] = createSignal(false);

  const isDirty = () =>
    MEMORY_FILES.some((file) => state[file.path]!.current !== state[file.path]!.baseline);

  createEffect(() => {
    markDirty('memory', isDirty());
  });

  onCleanup(() => markDirty('memory', false));

  const loadAll = async () => {
    setGlobalError(null);
    setRefreshing(true);
    try {
      let entries: FileEntry[] = [];
      try {
        const dir = await fetchFileList('/memory');
        entries = dir.entries ?? [];
      } catch (err) {
        if (String((err as Error).message || '').includes('404')) {
          entries = [];
        } else {
          throw err;
        }
      }
      const names = new Set(entries.map((entry) => entry.name));
      for (const file of MEMORY_FILES) {
        const fileName = file.path.split('/').pop()!;
        if (!names.has(fileName)) {
          setState(file.path, { exists: false, baseline: '', current: '', error: null });
          continue;
        }
        const { content } = await fetchFileContent(file.path);
        setState(file.path, {
          exists: true,
          baseline: content,
          current: content,
          error: null,
        });
      }
      setInitialized(true);
    } catch (err) {
      setGlobalError((err as Error).message);
    } finally {
      setRefreshing(false);
    }
  };

  const refreshOne = async (path: string) => {
    setState(path, 'loading', true);
    setState(path, 'error', null);
    try {
      const { content, missing } = await fetchFileContent(path, { allowMissing: true });
      if (missing) {
        setState(path, { exists: false, baseline: '', current: '', loading: false, error: null });
      } else {
        setState(path, {
          exists: true,
          baseline: content,
          current: content,
          loading: false,
          error: null,
        });
      }
    } catch (err) {
      setState(path, 'error', (err as Error).message);
      setState(path, 'loading', false);
    }
  };

  const saveOne = async (path: string) => {
    setState(path, 'saving', true);
    setState(path, 'error', null);
    try {
      await saveFileContent(path, state[path]!.current);
      setState(path, 'baseline', state[path]!.current);
      setState(path, 'exists', true);
      pushToast(t('fileEditorSaved') as string, 'success');
    } catch (err) {
      setState(path, 'error', (err as Error).message);
      pushToast((err as Error).message, 'error');
    } finally {
      setState(path, 'saving', false);
    }
  };

  const discardOne = (path: string) => {
    setState(path, 'current', state[path]!.baseline);
  };

  createEffect(() => {
    if (!initialized()) {
      loadAll();
    }
  });

  return (
    <TabShell>
      <PageHeader
        title={t('memoryTitle') as string}
        description={t('memoryDescription') as string}
        actions={
          <Button size="sm" variant="secondary" onClick={loadAll} disabled={refreshing()}>
            {t('memoryRefreshAll')}
          </Button>
        }
      />
      <Show when={globalError()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={globalError() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <For each={MEMORY_FILES}>
          {(file) => {
            const data = () => state[file.path]!;
            const dirty = () => data().current !== data().baseline;
            return (
              <CollapsibleConfigBlock
                title={t(file.titleKey) as string}
                defaultOpen
                headerEnd={
                  <Button
                    size="sm"
                    variant="secondary"
                    onClick={() => refreshOne(file.path)}
                    disabled={data().loading}
                  >
                    {t('memoryRefresh')}
                  </Button>
                }
              >
                <div class="flex flex-col gap-3 pt-2">
                  <Show when={data().error}>
                    <Banner kind="error" message={data().error ?? undefined} />
                  </Show>
                  <Show
                    when={data().exists || file.editable}
                    fallback={
                      <div class="rounded-[var(--radius-sm)] border border-dashed border-[var(--color-border-subtle)] bg-white/[0.02] p-3 text-[0.82rem] text-[var(--color-text-muted)]">
                        {file.titleKey === 'memoryCardLongTerm'
                          ? t('memoryMissing')
                          : t('memoryAuxMissing')}
                      </div>
                    }
                  >
                    <textarea
                      spellcheck={false}
                      readonly={!file.editable}
                      class={[
                        'w-full min-h-[220px] rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-input)] p-3 text-[0.88rem] leading-6 font-mono text-[var(--color-text-primary)] focus:outline-none focus:border-[rgba(232,54,45,0.4)]',
                        file.titleKey === 'memoryCardLongTerm' ? 'min-h-[300px]' : '',
                      ].join(' ')}
                      value={data().current}
                      onInput={(event) => {
                        if (!file.editable) return;
                        setState(file.path, 'current', event.currentTarget.value);
                      }}
                    />
                  </Show>
                  <Show when={file.editable}>
                    <div class="flex flex-wrap items-center justify-end gap-2">
                      <Show when={dirty()}>
                        <span class="text-[0.78rem] text-[var(--color-orange)] font-medium mr-auto sm:mr-0">
                          ●&nbsp;{t('unsavedIndicator')}
                        </span>
                      </Show>
                      <Button
                        size="sm"
                        variant="secondary"
                        disabled={!dirty() || data().saving}
                        onClick={() => discardOne(file.path)}
                      >
                        {t('discardBtn')}
                      </Button>
                      <Button
                        size="sm"
                        variant="primary"
                        disabled={!dirty() || data().saving}
                        onClick={() => saveOne(file.path)}
                      >
                        {data().saving ? '…' : t('fileEditorSave')}
                      </Button>
                    </div>
                  </Show>
                </div>
              </CollapsibleConfigBlock>
            );
          }}
        </For>
      </div>
    </TabShell>
  );
};
