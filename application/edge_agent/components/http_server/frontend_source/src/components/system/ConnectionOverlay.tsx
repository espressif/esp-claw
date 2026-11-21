import { createSignal, createUniqueId, onCleanup, onMount, Show, type Component } from 'solid-js';
import { LoaderCircle, WifiOff } from 'lucide-solid';
import { t } from '../../i18n';
import { appStatus, reloadStatus } from '../../state/config';
import { Button } from '../ui/Button';

export const ConnectionOverlay: Component = () => {
  let dialog!: HTMLDialogElement;
  const titleId = createUniqueId();
  const descriptionId = createUniqueId();
  const [retrying, setRetrying] = createSignal(false);

  // Native modal focus handling also prevents keyboard access to stale settings.
  onMount(() => dialog.showModal());
  onCleanup(() => dialog.close());

  const retry = async () => {
    setRetrying(true);
    try {
      await reloadStatus();
    } catch {
      // Keep the overlay open while automatic polling continues.
    } finally {
      setRetrying(false);
    }
  };

  return (
    <dialog
      ref={dialog}
      aria-labelledby={titleId}
      aria-describedby={descriptionId}
      aria-modal="true"
      onCancel={(event) => event.preventDefault()}
      class="fixed inset-0 m-auto w-[calc(100%_-_2rem)] max-w-md rounded-[var(--radius-lg)] border border-[var(--color-border-accent)] bg-[var(--color-bg-card)] p-6 text-[var(--color-text-primary)] shadow-2xl backdrop:bg-black/75 backdrop:backdrop-blur-sm sm:p-8"
    >
      <div class="flex flex-col items-center gap-5 text-center">
        <div class="flex h-16 w-16 items-center justify-center rounded-full bg-[var(--color-accent-dim)] text-[var(--color-danger)]">
          <Show when={appStatus()} fallback={<LoaderCircle size={32} class="animate-spin motion-reduce:animate-none" aria-hidden="true" />}>
            <WifiOff size={32} aria-hidden="true" />
          </Show>
        </div>
        <div>
          <h2 id={titleId} class="m-0 text-xl font-semibold">
            {appStatus() ? t('deviceDisconnectedTitle') : t('deviceConnectingTitle')}
          </h2>
          <p id={descriptionId} class="mb-0 mt-3 text-sm leading-relaxed text-[var(--color-text-secondary)]">
            {appStatus() ? t('deviceDisconnected') : t('deviceConnectingDescription')}
          </p>
        </div>
        <div role="status" class="flex items-center gap-2 text-sm text-[var(--color-text-secondary)]">
          <LoaderCircle size={16} class="animate-spin motion-reduce:animate-none" aria-hidden="true" />
          {t('deviceReconnecting')}
        </div>
        <Button variant="primary" class="w-full" disabled={retrying()} onClick={() => void retry()}>
          {t('deviceRetry')}
        </Button>
        <p class="m-0 text-xs leading-relaxed text-[var(--color-text-muted)]">{t('deviceConnectionHint')}</p>
      </div>
    </dialog>
  );
};
