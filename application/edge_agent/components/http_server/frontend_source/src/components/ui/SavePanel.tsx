import { Show, type Component, type JSX } from 'solid-js';
import { Button } from './Button';
import { t } from '../../i18n';

type SavePanelProps = {
  dirty: boolean;
  saving: boolean;
  onSave: () => void;
  onDiscard?: () => void;
  message?: JSX.Element;
  note?: JSX.Element;
  saveLabel?: string;
};

export const SavePanel: Component<SavePanelProps> = (props) => {
  return (
    <div class="flex flex-wrap items-center gap-3 border-t border-[var(--color-border-subtle)] px-5 py-4 bg-[var(--color-bg-surface)]/30">
      <Show when={props.onDiscard}>
        <Button
          variant="secondary"
          disabled={props.saving || !props.dirty}
          onClick={props.onDiscard}
        >
          {t('discardBtn')}
        </Button>
      </Show>
      <Show when={props.dirty}>
        <span class="text-[0.78rem] text-[var(--color-orange)] font-medium">●&nbsp;{t('unsavedIndicator')}</span>
      </Show>
      <div class="flex-1" />
      <Show when={props.message}>{props.message}</Show>
      <Show when={props.note}>
        <span class="min-w-[200px] text-right text-[0.78rem] text-[var(--color-text-muted)]">{props.note}</span>
      </Show>
      <Button
        variant="primary"
        onClick={props.onSave}
        disabled={props.saving || !props.dirty}
      >
        {props.saving ? '…' : props.saveLabel ?? (t('saveTabBtn') as string)}
      </Button>
    </div>
  );
};
