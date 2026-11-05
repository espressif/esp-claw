import { createSignal, onCleanup } from 'solid-js';
import { createStore } from 'solid-js/store';
import { createWebimSession, deleteWebimSession, fetchWebimHistory, fetchWebimSession, fetchWebimSessions, renameWebimSession, sendWebimMessage, webimWebSocketUrl, type WebImMessage, type WebImSession } from '../api/client';
import { t } from '../i18n';

type Thread = { messages: WebImMessage[]; draft: string; files: string[]; before: number; more: boolean; loading: boolean; scroll: number; loaded: boolean; error: string; pending?: { id: string; text: string; files: string[]; state: 'sending' | 'accepted' | 'failed' }; run: WebImSession['run_state']; delivery: string };
const emptyThread = (): Thread => ({ messages: [], draft: '', files: [], before: 0, more: false, loading: false, scroll: -1, loaded: false, error: '', run: 'idle', delivery: '' });
const STORAGE_KEY = 'esp-claw-active-session';

export function createWebChat() {
  const [sessions, setSessions] = createSignal<WebImSession[]>([]);
  const [active, setActive] = createSignal('');
  const [source, setSource] = createSignal('all');
  const [more, setMore] = createSignal(false);
  const [online, setOnline] = createSignal(true);
  const [error, setError] = createSignal('');
  const [threads, setThreads] = createStore<Record<string, Thread>>({ '': emptyThread() });
  let cursor = 0, boot = '', disposed = false, refreshing = false, creating = false;
  let listGeneration = 0;
  let socket: WebSocket | undefined;
  const generations = new Map<string, number>();
  const thread = (): Thread => threads[active()] ?? threads['']!;
  const session = () => sessions().find(s => s.session === active());
  const ensure = (id: string) => { if (!threads[id]) setThreads(id, emptyThread()); };
  const remember = (id: string) => { try { localStorage.setItem(STORAGE_KEY, id); } catch { /* Storage can be disabled. */ } };
  const checkBoot = (next: string) => {
    if (boot && boot !== next) {
      for (const id of Object.keys(threads)) {
        const pending = threads[id]!.pending;
        setThreads(id, { messages: [], loaded: false, loading: false, run: 'idle', more: false, pending: pending ? { ...pending, state: 'failed' } : undefined });
        generations.set(id, (generations.get(id) ?? 0) + 1);
      }
      setError(t('chatRestarted') as string);
    }
    boot = next;
  };
  const list = async (append = false) => {
    const generation = ++listGeneration;
    const page = await fetchWebimSessions(source(), append ? cursor : 0);
    if (disposed || generation !== listGeneration) return;
    checkBoot(page.boot_id);
    cursor = page.next_cursor; setMore(page.has_more);
    const old = sessions();
    const retained = !append && !page.has_more ? old.filter(s => source() !== 'all' && s.source !== source() || s.session === active() && !threads[s.session]?.loaded) : old;
    const merged = new Map(retained.map(s => [s.session, s]));
    page.items.forEach(s => merged.set(s.session, s));
    setSessions([...merged.values()].sort((a, b) => b.activity_order - a.activity_order || a.session.localeCompare(b.session)));
    setOnline(true); setError('');
  };
  const load = async (id: string, older = false) => {
    if (!id || disposed) return;
    ensure(id);
    if (threads[id]!.loading) return;
    const generation = (generations.get(id) ?? 0) + 1;
    generations.set(id, generation);
    setThreads(id, { loading: true });
    try {
      const page = await fetchWebimHistory(id, older ? threads[id]!.before : 0);
      if (disposed || generations.get(id) !== generation) return;
      checkBoot(page.boot_id);
      const messages = new Map(threads[id]!.messages.map(m => [m.seq, m]));
      page.messages.forEach(m => messages.set(m.seq, m));
      const pending = threads[id]!.pending;
      setThreads(id, {
        messages: [...messages.values()].filter(m => m.seq <= page.total).sort((a, b) => a.seq - b.seq),
        loaded: true, loading: false, error: '', run: page.run_state, delivery: page.delivery_state ?? '',
        ...(older || !threads[id]!.loaded ? { before: page.next_before, more: page.has_more } : {}),
        pending: pending?.state === 'accepted' && page.run_state === 'idle' ? undefined : pending,
      });
      setOnline(true);
    } catch (e) {
      if (!disposed && generations.get(id) === generation) {
        const message = (e as Error).message;
        setThreads(id, { loading: false, error: message });
        if (message.includes('session_not_found')) {
          setSessions(prev => prev.filter(s => s.session !== id));
          if (active() === id) {
            const next = sessions()[0]?.session ?? '';
            ensure(next); setActive(next); remember(next); void load(next);
          }
        }
      }
    }
  };
  const select = async (id: string) => {
    ensure(id); setActive(id); remember(id);
    await load(id);
  };
  const fresh = () => { setActive(''); remember(''); };
  const refresh = async () => {
    if (refreshing || disposed) return;
    refreshing = true;
    try { await list(); await load(active()); }
    catch (e) { if (!disposed) { setOnline(false); setError((e as Error).message); } }
    finally { refreshing = false; }
  };
  const send = async (retry = false) => {
    let id = active();
    if (retry && !thread().pending) return;
    if (creating || thread().pending?.state === 'sending') return;
    const draft = thread();
    const text = retry ? draft.pending?.text ?? '' : draft.draft.trim();
    const files = retry ? draft.pending?.files ?? [] : [...draft.files];
    if (!text && !files.length) return;
    const messageId = retry ? draft.pending!.id : globalThis.crypto?.randomUUID?.() ?? `w-${Date.now()}-${Math.random().toString(36).slice(2)}`;
    try {
      if (!id) {
        creating = true;
        const created = await createWebimSession();
        if (disposed) return;
        id = created.session;
        setThreads(id, { ...emptyThread(), draft: text, files });
        setThreads('', emptyThread());
        setSessions(prev => [created, ...prev]);
        if (active() === '') { setActive(id); remember(id); }
      }
      generations.set(id, (generations.get(id) ?? 0) + 1);
      setThreads(id, { loading: false, draft: '', files: [], error: '', pending: { id: messageId, text, files, state: 'sending' } });
      const receipt = await sendWebimMessage(id, messageId, text, files);
      if (disposed) return;
      checkBoot(receipt.boot_id);
      setThreads(id, { run: 'queued', pending: { id: messageId, text, files, state: 'accepted' } });
      await load(id);
    } catch (e) {
      if (!disposed) { ensure(id); setThreads(id, { ...(!threads[id]!.draft ? { draft: text, files } : {}), error: (e as Error).message, pending: { id: messageId, text, files, state: 'failed' } }); }
    } finally { creating = false; }
  };
  const rename = async (id: string, title: string) => {
    const updated = await renameWebimSession(id, title);
    if (!disposed) setSessions(prev => prev.map(s => s.session === id ? updated : s));
  };
  const remove = async (id: string) => {
    await deleteWebimSession(id);
    if (disposed) return;
    generations.set(id, (generations.get(id) ?? 0) + 1);
    setSessions(prev => prev.filter(s => s.session !== id));
    if (active() === id) await select(sessions()[0]?.session ?? '');
    await list();
  };
  const connect = () => {
    if (disposed || (socket && socket.readyState < WebSocket.CLOSING)) return;
    socket = new WebSocket(webimWebSocketUrl());
    socket.onopen = () => { socket?.send(JSON.stringify({ type: 'subscribe', scope: 'webim' })); void refresh(); };
    socket.onmessage = () => void refresh();
    socket.onerror = () => socket?.close();
  };
  const start = async () => {
    try {
      await list();
      let saved = '';
      try { saved = localStorage.getItem(STORAGE_KEY) ?? ''; } catch { /* Optional preference. */ }
      if (saved && !sessions().some(s => s.session === saved)) {
        try { const found = await fetchWebimSession(saved); setSessions(prev => [...prev, found]); }
        catch { saved = ''; }
      }
      await select(saved || sessions()[0]?.session || '');
      connect();
    } catch (e) { setError((e as Error).message); setOnline(false); }
  };
  const timer = setInterval(() => { connect(); void refresh(); }, 4000);
  const focus = () => void refresh();
  window.addEventListener('focus', focus);
  onCleanup(() => { disposed = true; clearInterval(timer); window.removeEventListener('focus', focus); socket?.close(); });
  return { sessions, active, source, more, online, error, thread, session, threads, start, select, fresh, send, rename, remove, load, refresh,
    loadMore: () => list(true),
    filter: async (value: string) => { setSource(value); await list(); },
    draft: (value: string) => setThreads(active(), 'draft', value),
    files: (id: string, values: string[]) => { ensure(id); setThreads(id, 'files', values); },
    scroll: (value: number) => setThreads(active(), 'scroll', value),
  };
}
