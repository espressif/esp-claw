import { createSignal, Show, type Component } from 'solid-js';
import { t } from '../i18n';
import type { AppConfig } from '../api/client';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { TextInput, SelectInput } from '../components/ui/FormField';
import { Switch } from '../components/ui/Switch';
import { SavePanel } from '../components/ui/SavePanel';
import { Banner } from '../components/ui/Banner';
import { RestartConfirmModal } from '../components/system/RestartConfirmModal';

type SttForm = {
  stt_enabled: string;
  stt_backend_type: string;
  stt_api_key: string;
  stt_base_url: string;
  stt_model: string;
  stt_language: string;
  stt_keep_audio: string;
};

const isEnabled = (value: string) => value === 'true' || value === '1';

export const SttPage: Component<{ onRestartRequest: () => void }> = (props) => {
  const tab = createConfigTab<SttForm>({
    tab: 'stt',
    groups: ['stt'],
    toForm: (config: Partial<AppConfig>) => ({
      stt_enabled: config.stt_enabled ?? 'false',
      stt_backend_type: config.stt_backend_type ?? 'openai',
      stt_api_key: config.stt_api_key ?? '',
      stt_base_url: config.stt_base_url ?? 'https://api.openai.com/v1',
      stt_model: config.stt_model ?? 'whisper-1',
      stt_language: config.stt_language ?? '',
      stt_keep_audio: config.stt_keep_audio ?? 'false',
    }),
    fromForm: (form) => ({
      stt_enabled: isEnabled(form.stt_enabled) ? 'true' : 'false',
      stt_backend_type: form.stt_backend_type.trim() || 'openai',
      stt_api_key: form.stt_api_key.trim(),
      stt_base_url: form.stt_base_url.trim(),
      stt_model: form.stt_model.trim(),
      stt_language: form.stt_language.trim(),
      stt_keep_audio: isEnabled(form.stt_keep_audio) ? 'true' : 'false',
    }),
  });
  const [confirmOpen, setConfirmOpen] = createSignal(false);

  const handleSave = async () => {
    await tab.save();
    setConfirmOpen(true);
  };

  const modelPlaceholder = () =>
    tab.form.stt_backend_type === 'deepgram'
      ? (t('sttModelPlaceholderDeepgram') as string)
      : (t('sttModelPlaceholderOpenai') as string);

  return (
    <TabShell>
      <PageHeader
        title={t('navStt') as string}
        description={t('sttNote') as string}
      />
      <Show when={tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={tab.error() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('sectionStt') as string}>
          <div class="pt-2 flex flex-col gap-4">
            <Switch
              label={t('sttEnabled') as string}
              checked={isEnabled(tab.form.stt_enabled)}
              onChange={(checked) =>
                tab.setForm('stt_enabled', checked ? 'true' : 'false')
              }
            />
            <div class="grid gap-3 sm:grid-cols-2">
              <SelectInput
                label={t('sttBackendType') as string}
                value={tab.form.stt_backend_type}
                onChange={(event) => {
                  const value = event.currentTarget.value;
                  tab.setForm('stt_backend_type', value);
                  if (value === 'deepgram') {
                    if (
                      tab.form.stt_base_url === 'https://api.openai.com/v1' ||
                      !tab.form.stt_base_url
                    ) {
                      tab.setForm('stt_base_url', 'https://api.deepgram.com/v1');
                    }
                    if (tab.form.stt_model === 'whisper-1' || !tab.form.stt_model) {
                      tab.setForm('stt_model', 'nova-2');
                    }
                  } else {
                    if (
                      tab.form.stt_base_url === 'https://api.deepgram.com/v1' ||
                      !tab.form.stt_base_url
                    ) {
                      tab.setForm('stt_base_url', 'https://api.openai.com/v1');
                    }
                    if (tab.form.stt_model === 'nova-2' || !tab.form.stt_model) {
                      tab.setForm('stt_model', 'whisper-1');
                    }
                  }
                }}
              >
                <option value="openai">{t('sttBackendOpenai')}</option>
                <option value="deepgram">{t('sttBackendDeepgram')}</option>
              </SelectInput>
              <TextInput
                type="password"
                label={t('sttApiKey')}
                value={tab.form.stt_api_key}
                onInput={(event) => tab.setForm('stt_api_key', event.currentTarget.value)}
              />
              <TextInput
                label={t('sttBaseUrl')}
                placeholder={t('sttBaseUrlPlaceholder') as string}
                value={tab.form.stt_base_url}
                onInput={(event) => tab.setForm('stt_base_url', event.currentTarget.value)}
              />
              <TextInput
                label={t('sttModel')}
                placeholder={modelPlaceholder()}
                value={tab.form.stt_model}
                onInput={(event) => tab.setForm('stt_model', event.currentTarget.value)}
              />
              <TextInput
                label={t('sttLanguage')}
                placeholder={t('sttLanguagePlaceholder') as string}
                value={tab.form.stt_language}
                onInput={(event) => tab.setForm('stt_language', event.currentTarget.value)}
              />
            </div>
            <Switch
              label={t('sttKeepAudio') as string}
              hint={t('sttKeepAudioHint') as string}
              checked={isEnabled(tab.form.stt_keep_audio)}
              onChange={(checked) =>
                tab.setForm('stt_keep_audio', checked ? 'true' : 'false')
              }
            />
          </div>
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
