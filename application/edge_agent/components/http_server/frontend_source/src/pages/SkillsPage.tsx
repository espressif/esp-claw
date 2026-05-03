import { createMemo, createSignal, For, Show, type Component } from 'solid-js';
import { t, tf } from '../i18n';
import type { AppConfig, LuaModuleItem } from '../api/client';
import { appCapabilities, appConfig, appLuaModules } from '../state/config';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { SavePanel } from '../components/ui/SavePanel';
import { Button } from '../components/ui/Button';
import { Switch } from '../components/ui/Switch';
import { Banner } from '../components/ui/Banner';
import { ConfigTable } from '../components/ui/ConfigTable';

const SENTINEL_NONE = '__none__';

type SkillForm = { enabled: string[] };

function parseModules(serialized: string, items: LuaModuleItem[], fallbackAll: boolean): string[] {
  const raw = (serialized || '').trim();
  if (!raw) {
    return fallbackAll ? items.map((item) => item.module_id) : [];
  }
  if (raw === SENTINEL_NONE || raw === 'none') return [];
  return raw
    .split(',')
    .map((token) => token.trim())
    .filter((token) => token && token !== SENTINEL_NONE && token !== 'none');
}

function serializeModules(selected: string[], items: LuaModuleItem[]): string {
  if (items.length === 0) return '';
  const set = new Set(selected);
  if (set.size === items.length) return '';
  if (set.size === 0) return SENTINEL_NONE;
  return Array.from(set).join(',');
}

function isCapLuaEnabled(): boolean {
  const raw = (appConfig().enabled_cap_groups || '').trim();
  if (!raw) {
    return appCapabilities().some((item) => item.group_id === 'cap_lua');
  }
  if (raw === SENTINEL_NONE || raw === 'none') return false;
  return raw
    .split(',')
    .map((token) => token.trim())
    .includes('cap_lua');
}

export const SkillsPage: Component = () => {
  const tab = createConfigTab<SkillForm>({
    tab: 'skills',
    groups: ['skills', 'capabilities'],
    toForm: (config: Partial<AppConfig>) => ({
      enabled: parseModules(config.enabled_lua_modules ?? '', appLuaModules(), true),
    }),
    fromForm: (form) => ({
      enabled_lua_modules: serializeModules(form.enabled, appLuaModules()),
    }),
    isDirty: (form, baseline) => {
      const a = new Set(form.enabled);
      const b = new Set(baseline.enabled);
      if (a.size !== b.size) return true;
      for (const v of a) if (!b.has(v)) return true;
      return false;
    },
  });

  const [search, setSearch] = createSignal('');
  const enabledSet = createMemo(() => new Set(tab.form.enabled));

  const filtered = createMemo(() => {
    const keyword = search().trim().toLowerCase();
    return appLuaModules().filter((item) => {
      if (!keyword) return true;
      return (
        item.module_id.toLowerCase().includes(keyword) ||
        (item.display_name ?? '').toLowerCase().includes(keyword)
      );
    });
  });

  const toggle = (id: string, checked: boolean) => {
    const set = new Set(tab.form.enabled);
    if (checked) set.add(id);
    else set.delete(id);
    tab.setForm('enabled', Array.from(set));
  };

  const selectAll = () => {
    tab.setForm('enabled', appLuaModules().map((item) => item.module_id));
  };
  const clearAll = () => {
    tab.setForm('enabled', []);
  };

  const summary = () =>
    tf('luaModulesSummary', {
      enabled: tab.form.enabled.length,
      total: appLuaModules().length,
    });

  return (
    <TabShell>
      <PageHeader
        title={t('sectionLuaModules') as string}
        description={t('luaModulesDescription') as string}
      />
      <div class="px-5 py-4 flex flex-col gap-3 border-b border-[var(--color-border-subtle)]">
        <Show when={!isCapLuaEnabled()}>
          <Banner kind="info" message={t('luaModulesCapabilityRequired') as string} />
        </Show>
        <div class="flex flex-wrap gap-2 items-center">
          <input
            type="search"
            placeholder={t('luaModulesSearchPlaceholder') as string}
            class="flex-1 min-w-[160px] max-w-xs"
            value={search()}
            onInput={(event) => setSearch(event.currentTarget.value)}
          />
          <Button size="sm" variant="secondary" onClick={selectAll} disabled={!isCapLuaEnabled()}>
            {t('luaModulesSelectAll')}
          </Button>
          <Button size="sm" variant="secondary" onClick={clearAll} disabled={!isCapLuaEnabled()}>
            {t('luaModulesClearAll')}
          </Button>
        </div>
        <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0">{summary()}</p>
      </div>

      <div class="p-5">
        <Show when={appLuaModules().length === 0}>
          <Banner kind="info" message={t('luaModulesLoading') as string} />
        </Show>
        <Show when={appLuaModules().length > 0 && filtered().length === 0}>
          <Banner kind="info" message={t('luaModulesNoResult') as string} />
        </Show>
        <ConfigTable
          columns={[
            { label: t('luaModulesNameCol') as string },
            { label: t('idCol') as string },
            { label: t('capabilityEnabled') as string, class: 'w-24 text-center' },
          ]}
        >
          <For each={filtered()}>
            {(item) => (
              <tr class="border-t border-[var(--color-border-subtle)] hover:bg-white/[0.02]">
                <td class="px-4 py-2.5 text-[0.88rem] text-[var(--color-text-primary)]">
                  <div class="flex flex-col gap-0.5">
                    <span class="font-semibold">{item.display_name || item.module_id}</span>
                  </div>
                </td>
                <td class="px-4 py-2.5 font-mono text-[0.78rem] text-[var(--color-text-muted)] break-all">
                  {item.module_id}
                </td>
                <td class="px-4 py-2.5 text-center">
                  <Switch
                    checked={enabledSet().has(item.module_id)}
                    disabled={!isCapLuaEnabled()}
                    onChange={(checked) => toggle(item.module_id, checked)}
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
