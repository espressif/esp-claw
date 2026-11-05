import { DropdownMenu } from '@kobalte/core/dropdown-menu';
import { For, Show, createEffect, createSignal, onMount, type Component } from 'solid-js';
import { ChevronDown, ImagePlus, LoaderCircle, MessageSquare, MoreHorizontal, PanelLeft, Plus, SendHorizontal, X } from 'lucide-solid';
import { createFolder, uploadFile, type WebImSession } from '../api/client';
import { createWebChat } from '../state/webim';
import { t } from '../i18n';
import { Button } from '../components/ui/Button';
import { Modal } from '../components/ui/Modal';
import { MarkdownMessage, loadMarkdownRuntime } from '../components/webim/MarkdownMessage';

export const WebImPage: Component = () => {
  const chat = createWebChat();
  const [sidebar, setSidebar] = createSignal(true);
  const [markdown, setMarkdown] = createSignal(false);
  const [menu, setMenu] = createSignal('');
  const [dialog, setDialog] = createSignal<{ kind: 'rename' | 'delete'; session: WebImSession }>();
  const [title, setTitle] = createSignal('');
  const [actionError, setActionError] = createSignal('');
  const [saving, setSaving] = createSignal(false);
  const [uploading, setUploading] = createSignal(false);
  const [newMessages, setNewMessages] = createSignal(false);
  let messagesEl: HTMLDivElement | undefined, inputEl: HTMLTextAreaElement | undefined, fileEl: HTMLInputElement | undefined;
  let renderedSession = '', renderedCount = 0;
  const label = (s?: WebImSession) => s?.title || (s?.source && s.source !== 'web' ? `${s.source} · ${s.alias}` : t('chatNew') as string);
  const repliesToSource = () => !!chat.session()?.reply_channel && chat.session()!.reply_channel !== 'web';
  const pending = () => chat.thread().pending;
  const running = () => chat.thread().run !== 'idle';
  const canSend = () => chat.online() && (!chat.active() || chat.session()?.can_send) && !uploading() && !running() && pending()?.state !== 'sending';
  const bottom = () => { if (messagesEl) { messagesEl.scrollTop = messagesEl.scrollHeight; chat.scroll(messagesEl.scrollTop); } setNewMessages(false); };
  const select = (id: string) => { setMenu(''); if (window.innerWidth < 768) setSidebar(false); setNewMessages(false); void chat.select(id); };
  const fresh = () => { chat.fresh(); if (window.innerWidth < 768) setSidebar(false); setMenu(''); inputEl?.focus(); };
  const openDialog = (kind: 'rename' | 'delete', session: WebImSession) => { setDialog({ kind, session }); setTitle(label(session)); setMenu(''); setActionError(''); };
  const commit = async () => {
    const current = dialog(); if (!current || saving()) return;
    setSaving(true); setActionError('');
    try {
      if (current.kind === 'rename') await chat.rename(current.session.session, title().trim());
      else await chat.remove(current.session.session);
      setDialog(undefined);
    } catch (e) { setActionError((e as Error).message); }
    finally { setSaving(false); }
  };
  const upload = async (event: Event) => {
    const target = event.currentTarget as HTMLInputElement, file = target.files?.[0];
    if (!file) return;
    const id = chat.active();
    const path = `/inbox/webim/${Date.now().toString(36)}_${file.name.replace(/[^a-zA-Z0-9._-]/g, '_')}`;
    setUploading(true); setActionError('');
    try { await createFolder('/inbox/webim'); await uploadFile(path, file); chat.files(id, [...(chat.threads[id]?.files ?? []), path]); }
    catch (e) { setActionError((e as Error).message); }
    finally { setUploading(false); target.value = ''; }
  };
  onMount(() => {
    if (window.innerWidth < 768) setSidebar(false);
    void chat.start();
    void loadMarkdownRuntime().then(() => setMarkdown(true)).catch(() => { /* Plain text remains available offline. */ });
  });
  createEffect(() => {
    const id = chat.active(), count = chat.thread().messages.length, savedScroll = chat.thread().scroll;
    const switched = renderedSession !== id;
    const added = count > renderedCount;
    renderedSession = id; renderedCount = count;
    requestAnimationFrame(() => {
      if (!messagesEl) return;
      if (switched) { messagesEl.scrollTop = savedScroll < 0 ? messagesEl.scrollHeight : savedScroll; setNewMessages(false); }
      else if (added && (savedScroll < 0 || messagesEl.scrollHeight - messagesEl.clientHeight - savedScroll < 180)) bottom();
      else if (added) setNewMessages(true);
    });
  });
  const older = async () => {
    if (!messagesEl) return;
    const id = chat.active(), height = messagesEl.scrollHeight, top = messagesEl.scrollTop;
    await chat.load(id, true);
    requestAnimationFrame(() => { if (messagesEl && chat.active() === id) { messagesEl.scrollTop = top + messagesEl.scrollHeight - height; chat.scroll(messagesEl.scrollTop); } });
  };

  return (
    <div class="web-chat relative flex h-[calc(100dvh-5.5rem)] min-h-[420px] overflow-hidden rounded-xl border border-[var(--color-border-subtle)] bg-[var(--color-bg-card)] sm:h-[calc(100dvh-6.5rem)]">
      <Show when={sidebar()}>
        <button class="absolute inset-0 z-20 bg-black/45 md:hidden" aria-label={t('chatCancel')} onClick={() => setSidebar(false)} />
        <aside class="absolute inset-y-0 left-0 z-30 flex w-60 shrink-0 flex-col border-r border-[var(--color-border-subtle)] bg-[var(--color-bg-card)] md:relative md:z-auto">
          <div class="flex items-center justify-between px-3 pt-3">
            <Button class="flex-1 justify-start" variant="ghost" onClick={fresh}><Plus size={17} />{t('chatNew')}</Button>
            <Button variant="ghost" size="xs" aria-label={t('chatSessions')} onClick={() => setSidebar(false)}><PanelLeft size={17} /></Button>
          </div>
          <div class="px-4 py-4"><select aria-label={t('chatAll')} class="w-full bg-transparent text-xs text-[var(--color-text-muted)] outline-none" value={chat.source()} onChange={e => void chat.filter(e.currentTarget.value).catch(err => setActionError(err.message))}>
            <option value="all">{t('chatAll')}</option>
            <For each={[...new Set(['web', 'wechat', 'telegram', 'feishu', 'qq', ...chat.sessions().map(s => s.source)])]}>{source => <option value={source}>{source === 'web' ? 'Web' : source}</option>}</For>
          </select></div>
          <nav class="min-h-0 flex-1 overflow-y-auto px-2 pb-3" aria-label={t('chatSessions')}>
            <Show when={!chat.sessions().length}><p class="px-3 text-xs leading-6 text-[var(--color-text-muted)]">{t('chatEmptyList')}</p></Show>
            <For each={chat.sessions().filter(s => chat.source() === 'all' || s.source === chat.source())}>{s => (
              <div class="relative mb-1">
                <div class={`flex items-center rounded-lg transition ${chat.active() === s.session ? 'bg-white/8' : 'hover:bg-white/4'}`}>
                  <button class="min-w-0 flex-1 px-3 py-3 text-left" onClick={() => select(s.session)} aria-current={chat.active() === s.session ? 'page' : undefined}>
                    <div class="truncate text-[0.82rem] text-[var(--color-text-primary)]">{label(s)}</div>
                    <div class="mt-1 flex items-center gap-1.5 text-[0.68rem] text-[var(--color-text-muted)]"><span>{s.source === 'web' ? 'Web' : s.source}</span><Show when={s.run_state !== 'idle'}><LoaderCircle size={10} class="animate-spin" /></Show></div>
                  </button>
                  <DropdownMenu open={menu() === s.session} onOpenChange={open => setMenu(open ? s.session : '')} placement="bottom-end" gutter={4} modal={false}>
                    <DropdownMenu.Trigger as={Button} size="xs" variant="ghost" class="mr-1" aria-label={t('chatActions')}><MoreHorizontal size={16} /></DropdownMenu.Trigger>
                    <DropdownMenu.Portal>
                      <DropdownMenu.Content class="z-50 min-w-36 rounded-lg border border-[var(--color-border-subtle)] bg-[var(--color-bg-card)] p-1 text-xs shadow-lg outline-none">
                        <DropdownMenu.Item class="cursor-pointer rounded px-3 py-2 outline-none data-[highlighted]:bg-white/5" onSelect={() => openDialog('rename', s)}>{t('chatRename')}</DropdownMenu.Item>
                        <DropdownMenu.Item class="cursor-pointer rounded px-3 py-2 text-red-400 outline-none data-[highlighted]:bg-white/5" onSelect={() => openDialog('delete', s)}>{t('chatDelete')}</DropdownMenu.Item>
                      </DropdownMenu.Content>
                    </DropdownMenu.Portal>
                  </DropdownMenu>
                </div>
              </div>
            )}</For>
            <Show when={chat.more()}><Button variant="ghost" size="sm" class="w-full" onClick={() => void chat.loadMore().catch(e => setActionError(e.message))}>{t('chatMore')}</Button></Show>
          </nav>
        </aside>
      </Show>
      <main class="flex min-w-0 flex-1 flex-col">
        <header class="flex h-14 shrink-0 items-center gap-3 border-b border-[var(--color-border-subtle)] px-4">
          <Show when={!sidebar()}><Button variant="ghost" size="xs" aria-label={t('chatSessions')} onClick={() => setSidebar(true)}><PanelLeft size={18} /></Button></Show>
          <h2 class="m-0 min-w-0 flex-1 truncate text-sm font-medium">{label(chat.session())}</h2>
          <Show when={!chat.online()}><span class="text-xs text-amber-400">{t('chatDisconnected')}</span></Show>
        </header>
        <div ref={messagesEl} class="min-h-0 flex-1 overflow-y-auto px-4 py-6 sm:px-8" onScroll={() => { if (messagesEl) chat.scroll(messagesEl.scrollTop); }}>
          <div class="mx-auto flex min-h-full max-w-[760px] flex-col gap-6">
            <Show when={chat.thread().more}><Button variant="ghost" size="sm" class="mx-auto" disabled={chat.thread().loading} onClick={() => void older()}>{t('chatEarlier')}</Button></Show>
            <Show when={chat.thread().loading && !chat.thread().loaded}><div class="flex items-center gap-2 text-sm text-[var(--color-text-muted)]"><LoaderCircle size={15} class="animate-spin" />{t('chatLoading')}</div></Show>
            <Show when={!chat.thread().loading && !chat.thread().messages.length && !pending()}><div class="my-auto flex flex-col items-center gap-3 py-16 text-center"><MessageSquare size={28} class="text-[var(--color-text-muted)]" /><h3 class="m-0 text-xl font-medium">{t('chatWelcome')}</h3><p class="m-0 text-sm text-[var(--color-text-muted)]">{t('chatWelcomeHint')}</p></div></Show>
            <For each={chat.thread().messages}>{message => <article class={`min-w-0 text-[0.9rem] leading-7 ${message.role === 'user' ? 'max-w-[88%] self-end rounded-2xl bg-white/7 px-4 py-2.5' : 'w-full self-start'}`} data-role={message.role}>
              <MarkdownMessage preview={markdown()} text={message.text} />
            </article>}</For>
            <Show when={pending()}><div class="rounded-lg border border-[var(--color-border-subtle)] px-3 py-2 text-xs text-[var(--color-text-muted)]"><span>{pending()?.state === 'sending' ? t('chatSending') : pending()?.state === 'accepted' ? t('chatAccepted') : t('chatUnknown')}</span><Show when={pending()?.state === 'failed'}><Button size="xs" variant="ghost" onClick={() => void chat.send(true)} disabled={!canSend()}>{t('chatRetry')}</Button></Show></div></Show>
            <Show when={repliesToSource() && chat.thread().delivery === 'failed'}><p role="alert" class="text-xs text-amber-400">{t('chatDeliveryFailed')}</p></Show>
            <Show when={running()}><div class="flex items-center gap-2 text-xs text-[var(--color-text-muted)]"><LoaderCircle size={14} class="animate-spin" />{chat.thread().run === 'queued' ? t('chatQueued') : t('chatRunning')}</div></Show>
            <Show when={chat.thread().error || actionError() || chat.error()}><div role="alert" class="rounded-lg bg-red-400/8 px-3 py-2 text-xs text-red-300">{chat.thread().error || actionError() || chat.error()}<Button variant="ghost" size="xs" onClick={() => void chat.refresh()}>{t('chatRetry')}</Button></div></Show>
          </div>
        </div>
        <Show when={newMessages()}><Button variant="secondary" size="sm" class="mx-auto mb-2" onClick={bottom}><ChevronDown size={14} />{t('chatNewMessages')}</Button></Show>
        <div class="mx-auto w-full max-w-[824px] px-4 pb-4 sm:px-8">
          <div class="mb-2 truncate text-[0.7rem] text-[var(--color-text-muted)]">{repliesToSource() ? `${t('chatReplyTo')} ${chat.session()!.reply_channel} · ${chat.session()!.chat_id}` : chat.session()?.source && chat.session()!.source !== 'web' ? t('chatDeviceLocal') : t('chatLocal')}</div>
          <Show when={chat.session() && !chat.session()!.can_send}><p class="text-xs text-amber-400">{t('chatUnavailable')}</p></Show>
          <div class="rounded-xl border border-[var(--color-border-strong)] bg-white/3 p-3 focus-within:border-[var(--color-accent)]">
            <textarea ref={inputEl} aria-label={t('webimPlaceholder')} class="block min-h-16 w-full resize-none border-0 bg-transparent text-sm leading-6 outline-none" placeholder={t('webimPlaceholder')} value={chat.thread().draft} onInput={e => chat.draft(e.currentTarget.value)} onKeyDown={e => { if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) { e.preventDefault(); if (canSend()) { bottom(); void chat.send(); } } }} />
            <Show when={chat.thread().files.length}><div class="mb-2 flex flex-wrap gap-2"><For each={chat.thread().files}>{file => <span class="flex items-center gap-1 rounded bg-white/5 px-2 py-1 text-xs">{file.split('/').pop()}<button aria-label={t('chatAttachRemove')} onClick={() => chat.files(chat.active(), chat.thread().files.filter(p => p !== file))}><X size={12} /></button></span>}</For></div></Show>
            <div class="flex items-center justify-between">
              <input ref={fileEl} type="file" accept="image/*" class="hidden" onChange={e => void upload(e)} />
              <Button variant="ghost" size="xs" aria-label={t('webimAttach')} disabled={uploading() || chat.thread().files.length >= 4} onClick={() => fileEl?.click()}><Show when={uploading()} fallback={<ImagePlus size={18} />}><LoaderCircle size={18} class="animate-spin" /></Show></Button>
              <Button variant="primary" size="sm" aria-label={t('webimSend')} disabled={!canSend() || (!chat.thread().draft.trim() && !chat.thread().files.length)} onClick={() => { bottom(); void chat.send(); }}><SendHorizontal size={17} />{t('webimSend')}</Button>
            </div>
          </div>
        </div>
      </main>
      <Modal open={!!dialog()} onClose={() => { if (!saving()) setDialog(undefined); }} title={dialog()?.kind === 'rename' ? t('chatRename') : t('chatDelete')} widthClass="w-full max-w-md" actions={<><Button onClick={() => setDialog(undefined)} disabled={saving()}>{t('chatCancel')}</Button><Button variant="primary" disabled={saving() || (dialog()?.kind === 'rename' && !title().trim())} onClick={() => void commit()}>{dialog()?.kind === 'rename' ? t('chatSave') : t('chatDelete')}</Button></>}>
        <div class="px-5 py-3"><Show when={dialog()?.kind === 'rename'} fallback={<p class="text-sm leading-6">{t('chatDeleteHint')}</p>}><input aria-label={t('chatTitle')} class="w-full rounded border border-[var(--color-border-strong)] bg-transparent p-2 text-sm" value={title()} onInput={e => setTitle(e.currentTarget.value)} /></Show><Show when={actionError()}><p role="alert" class="text-xs text-red-300">{actionError()}</p></Show></div>
      </Modal>
    </div>
  );
};
