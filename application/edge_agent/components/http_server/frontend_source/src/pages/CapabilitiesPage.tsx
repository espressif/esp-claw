import { createMemo, createSignal, For, Show, type Component } from 'solid-js';
import { t, tf } from '../i18n';
import type { AppConfig, CapabilityItem } from '../api/client';
import { appCapabilities } from '../state/config';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { SavePanel } from '../components/ui/SavePanel';
import { Button } from '../components/ui/Button';
import { Switch } from '../components/ui/Switch';
import { Banner } from '../components/ui/Banner';
import { ConfigTable } from '../components/ui/ConfigTable';

const SENTINEL_NONE = '__none__';

type CapForm = {
  enabled: string[];
  llmVisible: string[];
};

function parseSelection(
  serialized: string,
  items: CapabilityItem[],
  fallbackAll: boolean,
  defaultPredicate?: (item: CapabilityItem) => boolean,
): string[] {
  const raw = (serialized || '').trim();
  if (!raw) {
    if (fallbackAll) return items.map((item) => item.group_id);
    if (defaultPredicate) {
      return items.filter(defaultPredicate).map((item) => item.group_id);
    }
    return [];
  }
  if (raw === SENTINEL_NONE || raw === 'none') return [];
  return raw
    .split(',')
    .map((token) => token.trim())
    .filter((token) => token && token !== SENTINEL_NONE && token !== 'none');
}

function serializeSelection(
  selected: string[],
  items: CapabilityItem[],
  mode: 'all' | 'default',
  defaultPredicate?: (item: CapabilityItem) => boolean,
): string {
  if (items.length === 0) return '';
  const set = new Set(selected);
  if (mode === 'all' && set.size === items.length) return '';
  if (mode === 'default' && defaultPredicate) {
    const defaults = items.filter(defaultPredicate).map((item) => item.group_id);
    if (defaults.length === set.size && defaults.every((id) => set.has(id))) {
      return '';
    }
  }
  if (set.size === 0) return SENTINEL_NONE;
  return Array.from(set).join(',');
}

type FilterMode = 'all' | 'enabled' | 'llm';

export const CapabilitiesPage: Component = () => {
  const tab = createConfigTab<CapForm>({
    tab: 'capabilities',
    groups: ['capabilities'],
    toForm: (config: Partial<AppConfig>) => ({
      enabled: parseSelection(config.enabled_cap_groups ?? '', appCapabilities(), true),
      llmVisible: parseSelection(
        config.llm_visible_cap_groups ?? '',
        appCapabilities(),
        false,
        (item) => item.default_llm_visible,
      ),
    }),
    fromForm: (form) => {
      const items = appCapabilities();
      const enabledSet = new Set(form.enabled);
      const visible = form.llmVisible.filter((id) => enabledSet.has(id));
      return {
        enabled_cap_groups: serializeSelection(form.enabled, items, 'all'),
        llm_visible_cap_groups: serializeSelection(
          visible,
          items,
          'default',
          (item) => item.default_llm_visible,
        ),
      };
    },
    isDirty: (form, baseline) => {
      const enabledA = new Set(form.enabled);
      const enabledB = new Set(baseline.enabled);
      if (!setEqual(enabledA, enabledB)) return true;
      const llmA = new Set(form.llmVisible);
      const llmB = new Set(baseline.llmVisible);
      return !setEqual(llmA, llmB);
    },
  });

  const [filter, setFilter] = createSignal<FilterMode>('all');
  const [search, setSearch] = createSignal('');

  const enabledSet = createMemo(() => new Set(tab.form.enabled));
  const llmSet = createMemo(() => new Set(tab.form.llmVisible));

  const filtered = createMemo(() => {
    const keyword = search().trim().toLowerCase();
    return appCapabilities().filter((item) => {
      if (filter() === 'enabled' && !enabledSet().has(item.group_id)) return false;
      if (filter() === 'llm' && !llmSet().has(item.group_id)) return false;
      if (!keyword) return true;
      return (
        item.group_id.toLowerCase().includes(keyword) ||
        (item.display_name ?? '').toLowerCase().includes(keyword)
      );
    });
  });

  const toggleEnabled = (id: string, checked: boolean) => {
    const enabled = new Set(tab.form.enabled);
    const visible = new Set(tab.form.llmVisible);
    if (checked) enabled.add(id);
    else {
      enabled.delete(id);
      visible.delete(id);
    }
    tab.setForm('enabled', Array.from(enabled));
    tab.setForm('llmVisible', Array.from(visible));
  };

  const toggleLlm = (id: string, checked: boolean) => {
    if (!enabledSet().has(id)) return;
    const visible = new Set(tab.form.llmVisible);
    if (checked) visible.add(id);
    else visible.delete(id);
    tab.setForm('llmVisible', Array.from(visible));
  };

  const selectAll = () => {
    tab.setForm('enabled', appCapabilities().map((item) => item.group_id));
  };

  const clearAll = () => {
    tab.setForm('enabled', []);
    tab.setForm('llmVisible', []);
  };

  const restoreLlmDefaults = () => {
    const ids = appCapabilities()
      .filter((item) => item.default_llm_visible && enabledSet().has(item.group_id))
      .map((item) => item.group_id);
    tab.setForm('llmVisible', ids);
  };

  const clearLlm = () => {
    tab.setForm('llmVisible', []);
  };

  const summary = () =>
    tf('capSummary', {
      enabled: tab.form.enabled.length,
      llm: tab.form.llmVisible.length,
      total: appCapabilities().length,
    });

  return (
    <TabShell>
      <PageHeader
        title={t('sectionCapabilities') as string}
        description={t('capabilitiesDescription') as string}
      />
      <div class="px-5 py-4 flex flex-col gap-3 border-b border-[var(--color-border-subtle)]">
        <div class="flex flex-wrap gap-2 items-center">
          <div class="inline-flex rounded-[var(--radius-sm)] border border-[var(--color-border-subtle)] overflow-hidden">
            <For
              each={[
                { id: 'all' as const, label: t('capFilterAll') as string },
                { id: 'enabled' as const, label: t('capFilterEnabled') as string },
                { id: 'llm' as const, label: t('capFilterLlm') as string },
              ]}
            >
              {(item) => (
                <button
                  type="button"
                  class={[
                    'px-3 py-1.5 text-[0.8rem] transition',
                    filter() === item.id
                      ? 'bg-[var(--color-accent)]/15 text-[var(--color-text-primary)]'
                      : 'text-[var(--color-text-secondary)] hover:text-[var(--color-text-primary)] hover:bg-white/5',
                  ].join(' ')}
                  onClick={() => setFilter(item.id)}
                >
                  {item.label}
                </button>
              )}
            </For>
          </div>
          <input
            type="search"
            placeholder={t('capSearchPlaceholder') as string}
            class="flex-1 min-w-[160px] max-w-xs"
            value={search()}
            onInput={(event) => setSearch(event.currentTarget.value)}
          />
          <div class="flex flex-wrap gap-2">
            <Button size="sm" variant="secondary" onClick={selectAll}>
              {t('capabilitySelectAll')}
            </Button>
            <Button size="sm" variant="secondary" onClick={clearAll}>
              {t('capabilityClearAll')}
            </Button>
            <Button size="sm" variant="secondary" onClick={restoreLlmDefaults}>
              {t('capabilityLlmDefaults')}
            </Button>
            <Button size="sm" variant="secondary" onClick={clearLlm}>
              {t('capabilityLlmClear')}
            </Button>
          </div>
        </div>
        <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0">{summary()}</p>
      </div>

      <div class="p-5">
        <Show when={appCapabilities().length === 0}>
          <Banner kind="info" message={t('capabilityLoading') as string} />
        </Show>
        <Show when={appCapabilities().length > 0 && filtered().length === 0}>
          <Banner kind="info" message={t('capabilityNoResult') as string} />
        </Show>
        <ConfigTable
          columns={[
            { label: t('capabilityNameCol') as string },
            { label: t('idCol') as string },
            { label: t('capabilityEnabled') as string, class: 'w-24 text-center' },
            { label: t('capabilityLlmVisible') as string, class: 'w-28 text-center' },
          ]}
        >
          <For each={filtered()}>
            {(item) => (
              <tr class="border-t border-[var(--color-border-subtle)] hover:bg-white/[0.02]">
                <td class="px-4 py-2.5 text-[0.88rem] text-[var(--color-text-primary)]">
                  <div class="flex flex-col gap-0.5">
                    <span class="font-semibold">{item.display_name || item.group_id}</span>
                  </div>
                </td>
                <td class="px-4 py-2.5 font-mono text-[0.78rem] text-[var(--color-text-muted)] break-all">
                  {item.group_id}
                </td>
                <td class="px-4 py-2.5 text-center">
                  <Switch
                    checked={enabledSet().has(item.group_id)}
                    onChange={(checked) => toggleEnabled(item.group_id, checked)}
                  />
                </td>
                <td class="px-4 py-2.5 text-center">
                  <Switch
                    checked={llmSet().has(item.group_id)}
                    disabled={!enabledSet().has(item.group_id)}
                    onChange={(checked) => toggleLlm(item.group_id, checked)}
                  />
                </td>
              </tr>
            )}
          </For>
        </ConfigTable>
      </div>

      <SavePanel
        dirty={tab.dirty()}
        saving={tab.saving()}
        onSave={() => tab.save().catch(() => undefined)}
        onDiscard={tab.discard}
        note={t('restartHint') as string}
      />
    </TabShell>
  );
};

function setEqual<T>(a: Set<T>, b: Set<T>) {
  if (a.size !== b.size) return false;
  for (const value of a) {
    if (!b.has(value)) return false;
  }
  return true;
}
