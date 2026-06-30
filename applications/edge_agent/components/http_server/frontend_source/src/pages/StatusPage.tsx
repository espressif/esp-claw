import { createSignal, type Component } from 'solid-js';
import { t } from '../i18n';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { Button } from '../components/ui/Button';
import { RestartConfirmModal } from '../components/system/RestartConfirmModal';
import { appStatus, reloadStatus } from '../state/config';
import { pushToast } from '../state/toast';

const InfoRow: Component<{ label: string; value?: string; mono?: boolean }> = (props) => {
  const none = t('sysInfoNone') as string;
  return (
    <div class="flex items-center justify-between py-2 px-3 rounded-[var(--radius-sm)] bg-white/[0.02] border border-transparent hover:border-[var(--color-border-subtle)] gap-3">
      <span class="text-[0.78rem] uppercase tracking-wider text-[var(--color-text-muted)] font-semibold">
        {props.label}
      </span>
      <span
        class={[
          'text-[0.88rem] text-[var(--color-text-primary)] text-right break-all',
          props.mono ? 'font-mono' : '',
        ].join(' ')}
      >
        {props.value || none}
      </span>
    </div>
  );
};

export const StatusPage: Component<{ onRestartRequest: () => void }> = (props) => {
  const [confirmOpen, setConfirmOpen] = createSignal(false);

  const reload = async () => {
    try {
      await reloadStatus();
      pushToast(t('sysInfoReload') as string, 'success');
    } catch (err) {
      pushToast((err as Error).message, 'error');
    }
  };

  return (
    <TabShell>
      <PageHeader
        title={t('navStatus') as string}
        description={t('pageSubtitle') as string}
        actions={
          <>
            <Button size="sm" variant="secondary" onClick={reload}>
              {t('sysInfoReload')}
            </Button>
            <Button size="sm" variant="secondary" onClick={() => setConfirmOpen(true)}>
              {t('sysInfoRestart')}
            </Button>
          </>
        }
      />
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('sectionStatusNetwork') as string}>
          <div class="grid gap-2 sm:grid-cols-2 pt-2">
            <InfoRow
              label={t('sysInfoWifi') as string}
              value={
                appStatus()?.wifi_connected
                  ? (t('statusOnline') as string)
                  : appStatus()?.ap_active
                    ? (t('statusApActive') as string)
                    : (t('statusOffline') as string)
              }
            />
            <InfoRow label={t('sysInfoIp') as string} value={appStatus()?.ip} mono />
            <InfoRow label={t('sysInfoMode') as string} value={appStatus()?.wifi_mode} mono />
            <InfoRow label={t('sysInfoApSsid') as string} value={appStatus()?.ap_ssid} mono />
            <InfoRow label={t('sysInfoApIp') as string} value={appStatus()?.ap_ip} mono />
          </div>
        </StaticConfigBlock>
        <StaticConfigBlock title={t('sectionStatusStorage') as string}>
          <div class="grid gap-2 sm:grid-cols-2 pt-2">
            <InfoRow
              label={t('sysInfoStorage') as string}
              value={appStatus()?.storage_base_path}
              mono
            />
          </div>
        </StaticConfigBlock>
      </div>
      <RestartConfirmModal
        open={confirmOpen()}
        onClose={() => setConfirmOpen(false)}
        onConfirm={() => {
          setConfirmOpen(false);
          props.onRestartRequest();
        }}
      />
    </TabShell>
  );
};
