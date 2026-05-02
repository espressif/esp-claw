import { ImagePlus, SendHorizontal } from 'lucide-solid';
import { createSignal, For, onCleanup, onMount, Show, type Component } from 'solid-js';
import type { JSX } from 'solid-js';
import {
  createFolder,
  fetchWebimStatus,
  sendWebimMessage,
  uploadFile,
  webimWebSocketUrl,
  type WebImMessage,
} from '../api/client';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { Button } from '../components/ui/Button';
import { Banner } from '../components/ui/Banner';
import { t } from '../i18n';
import { pushToast } from '../state/toast';

const LS_CHAT_ID = 'esp-claw-webim-chat-id';

function loadOrCreateChatId(): string {
  try {
    const saved = localStorage.getItem(LS_CHAT_ID);
    if (saved) return saved;
  } catch {
    /* ignore */
  }
  const id =
    typeof crypto !== 'undefined' && crypto.randomUUID
      ? crypto.randomUUID()
      : 'w-' + Date.now().toString(36) + '-' + Math.random().toString(36).slice(2, 10);
  try {
    localStorage.setItem(LS_CHAT_ID, id);
  } catch {
    /* ignore */
  }
  return id;
}

export const WebImPage: Component = () => {
  const chatId = loadOrCreateChatId();
  /** In-memory transcript only — lost on refresh. */
  const [messages, setMessages] = createSignal<WebImMessage[]>([]);
  let userLocalSeq = 0;
  const [input, setInput] = createSignal('');
  const [pendingPaths, setPendingPaths] = createSignal<string[]>([]);
  const [error, setError] = createSignal<string | null>(null);
  const [bound, setBound] = createSignal<boolean | null>(null);
  const [wsReady, setWsReady] = createSignal(false);
  const [sending, setSending] = createSignal(false);
  let fileRef: HTMLInputElement | undefined;
  let ws: WebSocket | null = null;
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null;

  const clearReconnect = () => {
    if (reconnectTimer !== null) {
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
    }
  };

  const teardownWs = () => {
    clearReconnect();
    if (ws) {
      ws.onopen = null;
      ws.onclose = null;
      ws.onmessage = null;
      ws.onerror = null;
      try {
        ws.close();
      } catch {
        /* ignore */
      }
      ws = null;
    }
    setWsReady(false);
  };

  const scheduleReconnect = () => {
    clearReconnect();
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      connectWs();
    }, 2000);
  };

  const connectWs = () => {
    teardownWs();
    const socket = new WebSocket(webimWebSocketUrl());
    ws = socket;

    socket.onopen = () => {
      setWsReady(true);
      clearReconnect();
    };

    socket.onclose = () => {
      setWsReady(false);
      scheduleReconnect();
    };

    socket.onerror = () => {
      setWsReady(false);
    };

    socket.onmessage = (ev: MessageEvent<string>) => {
      let data: Record<string, unknown>;
      try {
        data = JSON.parse(ev.data) as Record<string, unknown>;
      } catch {
        return;
      }
      const cid = typeof data.chat_id === 'string' ? data.chat_id : '';
      if (!cid || cid !== chatId) {
        return;
      }
      const role = typeof data.role === 'string' ? data.role : '';
      if (role !== 'assistant') {
        return;
      }
      const seq = typeof data.seq === 'number' ? data.seq : 0;
      const text = typeof data.text === 'string' ? data.text : '';
      const ts_ms = typeof data.ts_ms === 'number' ? data.ts_ms : undefined;
      let links: WebImMessage['links'];
      const rawLinks = data.links;
      if (Array.isArray(rawLinks)) {
        links = rawLinks.map((x) => {
          const o = x as Record<string, unknown>;
          return {
            url: typeof o.url === 'string' ? o.url : '',
            label: typeof o.label === 'string' ? o.label : '',
          };
        });
      }
      setMessages((prev) => [...prev, { seq, role: 'assistant', text, ts_ms, links }]);
    };
  };

  onMount(async () => {
    try {
      await createFolder('/inbox/webim');
    } catch {
      /* may already exist */
    }
    try {
      const st = await fetchWebimStatus();
      setBound(!!st.bound);
    } catch {
      setBound(false);
    }
    connectWs();
  });

  onCleanup(() => {
    teardownWs();
  });

  const onPickFile = async (ev: Event) => {
    const inputEl = ev.currentTarget as HTMLInputElement;
    const file = inputEl.files?.[0];
    if (!file) return;
    const name = `${Date.now().toString(36)}_${file.name.replace(/[^a-zA-Z0-9._-]/g, '_')}`;
    const path = '/inbox/webim/' + name;
    try {
      await uploadFile(path, file);
      setPendingPaths((p) => [...p, path]);
      pushToast(t('webimUploaded') as string, 'info', 2500);
    } catch (e) {
      pushToast((e as Error).message, 'error', 4000);
    }
    inputEl.value = '';
  };

  const send = async () => {
    const id = chatId;
    const text = input().trim();
    const files = pendingPaths();
    if (!text && files.length === 0) return;
    if (!wsReady()) return;
    if (bound() === false) {
      pushToast(t('webimNoBind') as string, 'error', 5000);
      return;
    }
    setSending(true);
    setError(null);
    try {
      await sendWebimMessage(id, text, files);
      setInput('');
      setPendingPaths([]);
      userLocalSeq -= 1;
      const useq = userLocalSeq;
      if (text) {
        setMessages((prev) => [...prev, { seq: useq, role: 'user', text }]);
      }
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setSending(false);
    }
  };

  const onInputKeyDown: JSX.EventHandler<HTMLTextAreaElement, KeyboardEvent> = (e) => {
    if (e.ctrlKey && e.key === 'Enter') {
      e.preventDefault();
      if (!sending() && wsReady()) {
        void send();
      }
    }
  };

  return (
    <TabShell>
      <PageHeader
        title={t('navWebIm') as string}
        description={t('webimDesc') as string}
        actions={
          <Show when={wsReady()}>
            <span class="inline-flex items-center gap-2 px-3 py-1 rounded-full border border-[rgba(104,211,145,0.2)] bg-[var(--color-green-dim)] text-[var(--color-green)] text-[0.78rem] font-medium">
              <span class="w-1.5 h-1.5 rounded-full bg-[var(--color-green)] pulse-dot" />
              {t('webimOnline')}
            </span>
          </Show>
        }
      />

      <Show when={error()}>
        <div class="px-5 pt-2">
          <Banner kind="error" message={error() ?? undefined} />
        </div>
      </Show>
      <Show when={bound() === false}>
        <div class="px-5 pt-2">
          <Banner kind="info" message={t('webimNoBind') as string} />
        </div>
      </Show>

      <div class="flex flex-col min-h-[420px]">
        <div class="flex-1 flex flex-col min-w-0 border border-[var(--color-border-subtle)] rounded-none bg-white/[0.02]">
          <div class="relative flex-1 overflow-auto p-4 flex flex-col gap-3">
            <For each={messages()}>
              {(m) => (
                <div
                  class={[
                    'max-w-[min(100%,36rem)] rounded-[var(--radius-md)] px-3 py-2 text-[0.88rem] leading-relaxed',
                    m.role === 'user'
                      ? 'self-end bg-[var(--color-accent)]/18 text-[var(--color-text-primary)]'
                      : 'self-start bg-white/6 text-[var(--color-text-primary)]',
                  ].join(' ')}
                >
                  <p class="m-0 whitespace-pre-wrap break-words">{m.text}</p>
                  <Show when={(m.links?.length ?? 0) > 0}>
                    <ul class="mt-2 space-y-1 list-none m-0 p-0">
                      <For each={m.links ?? []}>
                        {(lnk) => (
                          <li>
                            <a
                              href={lnk.url}
                              download=""
                              class="text-[var(--color-accent-soft)] hover:underline text-[0.82rem]"
                            >
                              {lnk.label || lnk.url}
                            </a>
                          </li>
                        )}
                      </For>
                    </ul>
                  </Show>
                </div>
              )}
            </For>
            <Show when={!wsReady() || messages().length === 0}>
              <div class="absolute inset-0 flex items-center justify-center p-4 pointer-events-none">
                <p
                  class={[
                    'm-0 text-center inline-flex items-center px-4 py-2 rounded-full border text-[0.82rem] font-medium',
                    !wsReady()
                      ? 'border-[rgba(245,158,11,0.28)] bg-[rgba(245,158,11,0.12)] text-[rgb(245,158,11)]'
                      : 'border-[rgba(104,211,145,0.2)] bg-[var(--color-green-dim)] text-[var(--color-green)]',
                  ].join(' ')}
                >
                  {!wsReady() ? (t('webimWsReconnecting') as string) : (t('webimEmpty') as string)}
                </p>
              </div>
            </Show>
          </div>

          <Show when={pendingPaths().length > 0}>
            <div class="px-4 pb-1 text-[0.76rem] text-[var(--color-text-muted)]">
              {t('webimPendingFiles')}: {pendingPaths().length}
            </div>
          </Show>

          <div class="p-3 border-t border-[var(--color-border-subtle)] flex flex-col gap-2">
            <textarea
              class="w-full min-h-[72px] rounded-[var(--radius-sm)] bg-black/25 border border-[var(--color-border-subtle)] px-3 py-2 text-[0.88rem] text-[var(--color-text-primary)] placeholder:text-[var(--color-text-muted)]"
              placeholder={t('webimPlaceholder') as string}
              value={input()}
              onInput={(e) => setInput(e.currentTarget.value)}
              onKeyDown={onInputKeyDown}
            />
            <div class="flex flex-wrap items-center gap-2">
              <input
                ref={fileRef}
                type="file"
                accept="image/*"
                class="hidden"
                onChange={(e) => void onPickFile(e)}
              />
              <Button
                size="sm"
                variant="secondary"
                type="button"
                onClick={() => fileRef?.click()}
                disabled={sending() || !wsReady()}
              >
                <span class="inline-flex items-center gap-1.5">
                  <ImagePlus class="w-4 h-4" />
                  {t('webimAttach')}
                </span>
              </Button>
              <span class="text-[0.76rem] text-[var(--color-text-muted)] sm:ml-auto">
                {t('webimSendShortcut')}
              </span>
              <Button
                size="sm"
                variant="primary"
                onClick={() => void send()}
                disabled={sending() || !wsReady()}
              >
                <span class="inline-flex items-center gap-1.5">
                  <SendHorizontal class="w-4 h-4" />
                  {t('webimSend')}
                </span>
              </Button>
            </div>
          </div>
        </div>
      </div>
    </TabShell>
  );
};
