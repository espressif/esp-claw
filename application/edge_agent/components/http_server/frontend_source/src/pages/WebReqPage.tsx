import { createSignal, Show, type Component } from 'solid-js';
import { t } from '../i18n';
import type { AppConfig } from '../api/client';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { TextInput } from '../components/ui/FormField';
import { SearchFields, type SearchConfig } from '../components/system/SearchFields';
import { SavePanel } from '../components/ui/SavePanel';
import { Banner } from '../components/ui/Banner';
import { RestartConfirmModal } from '../components/system/RestartConfirmModal';

type WebReqForm = SearchConfig & {
  search_http_allowlist: string;
};

export const WebReqPage: Component<{ onRestartRequest: () => void }> = (props) => {
  const tab = createConfigTab<WebReqForm>({
    tab: 'webreq',
    groups: ['search'],
    toForm: (config: Partial<AppConfig>) => ({
      search_provider: config.search_provider ?? 'tavily',
      search_bocha_key: config.search_bocha_key ?? '',
      search_brave_key: config.search_brave_key ?? '',
      search_tavily_key: config.search_tavily_key ?? '',
      search_http_allowlist: config.search_http_allowlist ?? '',
    }),
    fromForm: (form) => ({
      search_provider: form.search_provider,
      search_bocha_key: form.search_bocha_key.trim(),
      search_brave_key: form.search_brave_key.trim(),
      search_tavily_key: form.search_tavily_key.trim(),
      search_http_allowlist: form.search_http_allowlist.trim(),
    }),
  });
  const [confirmOpen, setConfirmOpen] = createSignal(false);

  const handleSave = async () => {
    await tab.save();
    setConfirmOpen(true);
  };

  return (
    <TabShell>
      <PageHeader title={t('navWebReq') as string} />
      <Show when={tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={tab.error() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('sectionWebReqSearch') as string}>
          <div class="grid gap-3 sm:grid-cols-2 pt-2">
            <SearchFields form={tab.form} onChange={(field, value) => tab.setForm(field, value)} />
          </div>
        </StaticConfigBlock>
        <StaticConfigBlock title={t('sectionWebReqNetwork') as string}>
          <div class="pt-2">
            <TextInput
              full
              label={t('webreqHttpAllowlist')}
              placeholder={t('webreqHttpAllowlistPlaceholder') as string}
              value={tab.form.search_http_allowlist}
              onInput={(event) => tab.setForm('search_http_allowlist', event.currentTarget.value)}
            />
          </div>
          <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0 pt-3">
            {t('webreqHttpAllowlistNote')}
          </p>
        </StaticConfigBlock>
      </div>
      <SavePanel
        dirty={tab.dirty()}
        saving={tab.saving()}
        onSave={() => handleSave().catch(() => undefined)}
        onDiscard={tab.discard}
        note={t('restartHint') as string}
      />
      <RestartConfirmModal
        open={confirmOpen()}
        onClose={() => setConfirmOpen(false)}
        onConfirm={() => {
          setConfirmOpen(false);
          props.onRestartRequest();
        }}
        subtitle={t('restartHint') as string}
      />
    </TabShell>
  );
};
