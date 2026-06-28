import { createSignal, Show, type Component } from 'solid-js';
import { t } from '../i18n';
import { saveConfigPatch, type AppConfig } from '../api/client';
import { pushToast } from '../state/toast';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { TextInput } from '../components/ui/FormField';
import { Button } from '../components/ui/Button';
import { SavePanel } from '../components/ui/SavePanel';
import { Banner } from '../components/ui/Banner';
import { RestartConfirmModal } from '../components/system/RestartConfirmModal';

type MqttForm = {
  mqtt_uri: string;
  mqtt_username: string;
  mqtt_password: string;
  mqtt_jwt: string;
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
      mqtt_jwt: config.mqtt_jwt ?? '',
      mqtt_client_id: config.mqtt_client_id ?? '',
    }),
    fromForm: (form) => ({
      mqtt_uri: form.mqtt_uri.trim(),
      mqtt_username: form.mqtt_username.trim(),
      mqtt_password: form.mqtt_password,
      mqtt_jwt: form.mqtt_jwt.trim(),
      mqtt_client_id: form.mqtt_client_id.trim(),
    }),
  });
  const [confirmOpen, setConfirmOpen] = createSignal(false);

  // Dev mode guards the destructive "clear credentials" action, mirroring the
  // Files page: it starts OFF and asks for confirmation when switched on.
  const [devMode, setDevMode] = createSignal(false);
  const [clearing, setClearing] = createSignal(false);

  const toggleDevMode = () => {
    if (!devMode()) {
      if (!window.confirm(t('mqttDevModeConfirm') as string)) return;
    }
    setDevMode(!devMode());
  };

  const handleSave = async () => {
    await tab.save();
    setConfirmOpen(true);
  };

  const clearCredentials = async () => {
    if (!devMode()) {
      pushToast(t('mqttDevModeRequired') as string, 'error');
      return;
    }
    if (clearing()) return;
    if (!window.confirm(t('mqttClearConfirm') as string)) return;
    setClearing(true);
    try {
      await saveConfigPatch({ mqtt_uri: '', mqtt_username: '', mqtt_password: '', mqtt_jwt: '' });
      await tab.reload();
      pushToast(t('mqttClearDone') as string, 'success');
    } catch (err) {
      pushToast((err as Error).message, 'error');
    } finally {
      setClearing(false);
    }
  };

  return (
    <TabShell>
      <PageHeader
        title={t('navMqtt') as string}
        actions={
          <>
            <Button size="sm" variant="secondary" active={devMode()} onClick={toggleDevMode}>
              {devMode() ? t('mqttDevModeOn') : t('mqttDevMode')}
            </Button>
            <Button
              size="sm"
              variant="secondary"
              onClick={() => clearCredentials().catch(() => undefined)}
              disabled={!devMode() || clearing()}
            >
              {t('mqttClearCreds')}
            </Button>
          </>
        }
      />
      <Show when={tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={tab.error() ?? undefined} />
        </div>
      </Show>
      <Show when={!devMode()}>
        <div class="px-5 pt-4">
          <Banner kind="info" message={t('mqttLocked') as string} />
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
              disabled={!devMode()}
              onInput={(event) => tab.setForm('mqtt_uri', event.currentTarget.value)}
            />
          </div>
          <div class="grid gap-3 sm:grid-cols-2 pt-3">
            <TextInput
              label={t('mqttUsername')}
              value={tab.form.mqtt_username}
              disabled={!devMode()}
              onInput={(event) => tab.setForm('mqtt_username', event.currentTarget.value)}
            />
            <TextInput
              type="password"
              label={t('mqttPassword')}
              value={tab.form.mqtt_password}
              disabled={!devMode()}
              onInput={(event) => tab.setForm('mqtt_password', event.currentTarget.value)}
            />
            <TextInput
              label={t('mqttClientId')}
              placeholder={t('mqttClientIdPlaceholder') as string}
              value={tab.form.mqtt_client_id}
              disabled={!devMode()}
              onInput={(event) => tab.setForm('mqtt_client_id', event.currentTarget.value)}
            />
          </div>
          <div class="pt-3">
            <TextInput
              full
              type="password"
              label={t('mqttJwt')}
              placeholder={t('mqttJwtPlaceholder') as string}
              value={tab.form.mqtt_jwt}
              disabled={!devMode()}
              onInput={(event) => tab.setForm('mqtt_jwt', event.currentTarget.value)}
            />
          </div>
          <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0 pt-2">{t('mqttJwtNote')}</p>
          <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0 pt-3">{t('mqttNote')}</p>
        </StaticConfigBlock>
      </div>
      <SavePanel
        dirty={tab.dirty() && devMode()}
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
