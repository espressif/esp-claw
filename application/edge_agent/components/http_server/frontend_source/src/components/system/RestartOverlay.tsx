import { Match, Show, Switch, type Component } from 'solid-js';
import { Portal } from 'solid-js/web';
import { t } from '../../i18n';
import { Button } from '../ui/Button';
import { Banner } from '../ui/Banner';

export type RestartOverlayState = {
  open: boolean;
  phase: 'requesting' | 'cooldown' | 'polling' | 'error';
  error?: string | null;
  deadlineAt?: number;
};

type RestartOverlayProps = {
  state: RestartOverlayState;
  onClose: () => void;
};

export const RestartOverlay: Component<RestartOverlayProps> = (props) => {
  return (
    <Show when={props.state.open}>
      <Portal>
        <div class="fixed inset-0 z-[1200] flex items-center justify-center p-6 bg-black/60 backdrop-blur-sm">
          <div class="w-full max-w-md rounded-[var(--radius-lg)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-card)] shadow-[0_24px_60px_rgba(0,0,0,0.35)] p-6">
            <div class="flex flex-col gap-4">
              <div class="flex items-center gap-3">
                <div class="w-10 h-10 rounded-full border border-[var(--color-border-accent)] bg-[var(--color-accent)]/10 flex items-center justify-center">
                  <div class="w-4 h-4 rounded-full border-2 border-[var(--color-accent)] border-t-transparent animate-spin" />
                </div>
                <div>
                  <h2 class="m-0 text-base font-semibold text-[var(--color-text-primary)]">
                    {t('restartOverlayTitle')}
                  </h2>
                  <p class="m-0 mt-1 text-[0.84rem] text-[var(--color-text-muted)]">
                    <Switch>
                      <Match when={props.state.phase === 'requesting'}>
                        {t('restartOverlayRequesting')}
                      </Match>
                      <Match when={props.state.phase === 'cooldown'}>
                        {t('restartOverlayCooldown')}
                      </Match>
                      <Match when={props.state.phase === 'polling'}>
                        {t('restartOverlayPolling')}
                      </Match>
                      <Match when={props.state.phase === 'error'}>
                        {t('restartOverlayErrorTitle')}
                      </Match>
                    </Switch>
                  </p>
                </div>
              </div>

              <Show when={props.state.phase !== 'error'}>
                <Banner kind="info" message={t('restartOverlayHint') as string} />
              </Show>

              <Show when={props.state.phase === 'error'}>
                <Banner
                  kind="error"
                  message={props.state.error ?? (t('restartOverlayTimeout') as string)}
                />
              </Show>

              <Show when={props.state.phase === 'error'}>
                <div class="flex justify-end">
                  <Button variant="secondary" onClick={props.onClose}>
                    {t('restartOverlayClose')}
                  </Button>
                </div>
              </Show>
            </div>
          </div>
        </div>
      </Portal>
    </Show>
  );
};
