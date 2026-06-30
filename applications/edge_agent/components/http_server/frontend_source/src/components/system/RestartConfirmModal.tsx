import type { Component } from 'solid-js';
import { t } from '../../i18n';
import { Button } from '../ui/Button';
import { Modal } from '../ui/Modal';

type RestartConfirmModalProps = {
  open: boolean;
  onClose: () => void;
  onConfirm: () => void;
  subtitle?: string;
};

export const RestartConfirmModal: Component<RestartConfirmModalProps> = (props) => {
  return (
    <Modal
      open={props.open}
      onClose={props.onClose}
      title={t('restartConfirmTitle')}
      subtitle={props.subtitle ?? (t('restartConfirmBody') as string)}
      widthClass="w-full max-w-md"
      actions={
        <>
          <Button variant="ghost" onClick={props.onClose}>
            {t('restartConfirmCancel')}
          </Button>
          <Button variant="primary" onClick={props.onConfirm}>
            {t('restartConfirmAction')}
          </Button>
        </>
      }
    >
      <div class="px-5 pb-2">
        <p class="m-0 text-[0.88rem] text-[var(--color-text-secondary)]">
          {t('restartConfirmHint')}
        </p>
      </div>
    </Modal>
  );
};
