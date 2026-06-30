import { createSignal } from 'solid-js';

export type ToastKind = 'success' | 'error' | 'info';

export type ToastItem = {
  id: number;
  kind: ToastKind;
  message: string;
};

const [toasts, setToasts] = createSignal<ToastItem[]>([]);
export const toastList = toasts;

let nextId = 1;

export function pushToast(message: string, kind: ToastKind = 'success', timeoutMs = 3200) {
  if (!message) return;
  const id = nextId++;
  setToasts([...toasts(), { id, kind, message }]);
  if (timeoutMs > 0) {
    window.setTimeout(() => dismissToast(id), timeoutMs);
  }
}

export function dismissToast(id: number) {
  setToasts(toasts().filter((toast) => toast.id !== id));
}
