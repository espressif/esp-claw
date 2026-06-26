import { createSignal, Show, type Component } from 'solid-js';
import { t } from '../i18n';
import type { AppConfig } from '../api/client';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { TextInput } from '../components/ui/FormField';
import { SavePanel } from '../components/ui/SavePanel';
import { Banner } from '../components/ui/Banner';
import { RestartConfirmModal } from '../components/system/RestartConfirmModal';

type MqttForm = {
  mqtt_uri: string;
  mqtt_username: string;
  mqtt_password: string;
  mqtt_client_id: string;
};

export const MqttPage: Component<{ onRestartRequest: () => void }> = (props) => {
  const tab = createConfigTab<MqttForm>({
    tab: 'mqtt',
    groups: ['mqtt'],
    toForm: (config: Partial<AppConfig>) => ({
      mqtt_uri: config.mqtt_uri ?? '',
      mqtt_username: config.mqtt_username ?? '',
      mqtt_password: config.mqtt_password ?? '',
      mqtt_client_id: config.mqtt_client_id ?? '',
    }),
    fromForm: (form) => ({
      mqtt_uri: form.mqtt_uri.trim(),
      mqtt_username: form.mqtt_username.trim(),
      mqtt_password: form.mqtt_password,
      mqtt_client_id: form.mqtt_client_id.trim(),
    }),
  });
  const [confirmOpen, setConfirmOpen] = createSignal(false);

  const handleSave = async () => {
    await tab.save();
    setConfirmOpen(true);
  };

  return (
    <TabShell>
      <PageHeader title={t('navMqtt') as string} />
      <Show when={tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={tab.error() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('sectionMqttBroker') as string}>
          <div class="pt-2">
            <TextInput
              full
              label={t('mqttUri')}
              placeholder={t('mqttUriPlaceholder') as string}
              value={tab.form.mqtt_uri}
              onInput={(event) => tab.setForm('mqtt_uri', event.currentTarget.value)}
            />
          </div>
          <div class="grid gap-3 sm:grid-cols-2 pt-3">
            <TextInput
              label={t('mqttUsername')}
              value={tab.form.mqtt_username}
              onInput={(event) => tab.setForm('mqtt_username', event.currentTarget.value)}
            />
            <TextInput
              type="password"
              label={t('mqttPassword')}
              value={tab.form.mqtt_password}
              onInput={(event) => tab.setForm('mqtt_password', event.currentTarget.value)}
            />
            <TextInput
              label={t('mqttClientId')}
              placeholder={t('mqttClientIdPlaceholder') as string}
              value={tab.form.mqtt_client_id}
              onInput={(event) => tab.setForm('mqtt_client_id', event.currentTarget.value)}
            />
          </div>
          <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0 pt-3">{t('mqttNote')}</p>
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
