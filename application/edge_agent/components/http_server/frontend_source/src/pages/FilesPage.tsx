import { HardDriveDownload, Trash2, X } from 'lucide-solid';
import { createEffect, createSignal, For, onCleanup, Show, type Component } from 'solid-js';
import { Portal } from 'solid-js/web';
import { t, tf } from '../i18n';
import {
  createFolder,
  deletePath,
  downloadFolderTar,
  fetchFileContent,
  fetchFileList,
  saveFileContent,
  uploadFile,
  type FileEntry,
} from '../api/client';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { Button } from '../components/ui/Button';
import { Banner } from '../components/ui/Banner';
import { Modal } from '../components/ui/Modal';
import { markDirty } from '../state/dirty';
import { pushToast } from '../state/toast';

const EDITABLE_EXT = ['.md', '.json', '.txt', '.jsonl', '.jsonc', '.lua', '.log'];

function parentOf(path: string): string {
  if (path === '/') return '/';
  const parts = path.split('/').filter(Boolean);
  parts.pop();
  return parts.length ? '/' + parts.join('/') : '/';
}

function joinPath(base: string, name: string): string {
  return base === '/' ? '/' + name : base + '/' + name;
}

function humanSize(bytes: number): string {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
  return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
}

function isEditable(path: string): boolean {
  const lower = path.toLowerCase();
  return EDITABLE_EXT.some((ext) => lower.endsWith(ext));
}

function downloadHref(path: string): string {
  return '/files' + path;
}

type EditorState = {
  open: boolean;
  path: string;
  content: string;
  baseline: string;
  loading: boolean;
  saving: boolean;
  error: string | null;
  readOnly: boolean;
};

type FolderDownloadState = {
  path: string;
  name: string;
  progress: number;
  controller: AbortController;
};

const initialEditor: EditorState = {
  open: false,
  path: '',
  content: '',
  baseline: '',
  loading: false,
  saving: false,
  error: null,
  readOnly: true,
};

export const FilesPage: Component = () => {
  const [currentPath, setCurrentPath] = createSignal('/');
  const [entries, setEntries] = createSignal<FileEntry[]>([]);
  const [error, setError] = createSignal<string | null>(null);
  const [loading, setLoading] = createSignal(false);
  const [devMode, setDevMode] = createSignal(false);
  const [uploadPath, setUploadPath] = createSignal('');
  const [fileChosenName, setFileChosenName] = createSignal<string | null>(null);
  const [newFolderName, setNewFolderName] = createSignal('');
  let fileInputRef: HTMLInputElement | undefined;
  const [chosenFile, setChosenFile] = createSignal<File | null>(null);

  const [editor, setEditor] = createSignal<EditorState>(initialEditor);
  const [folderDownload, setFolderDownload] = createSignal<FolderDownloadState | null>(null);

  const dirtyEditor = () => editor().open && editor().content !== editor().baseline;

  createEffect(() => {
    markDirty('files', dirtyEditor());
  });
  onCleanup(() => markDirty('files', false));
  onCleanup(() => folderDownload()?.controller.abort());

  const loadList = async () => {
    setError(null);
    setLoading(true);
    try {
      const data = await fetchFileList(currentPath());
      setCurrentPath(data.path || '/');
      setEntries(
        (data.entries ?? [])
          .slice()
          .sort((a, b) => Number(b.is_dir) - Number(a.is_dir) || a.name.localeCompare(b.name)),
      );
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setLoading(false);
    }
  };

  createEffect(() => {
    void currentPath();
    loadList();
  });

  const toggleDevMode = () => {
    if (!devMode()) {
      if (!window.confirm(t('fileDevModeConfirm') as string)) return;
    }
    setDevMode(!devMode());
  };

  const handleUpload = async () => {
    if (!devMode()) {
      pushToast(t('fileDevModeRequired') as string, 'error');
      return;
    }
    const file = chosenFile();
    const target = uploadPath().trim() || (file ? joinPath(currentPath(), file.name) : '');
    if (!file || !target.startsWith('/')) {
      pushToast(t('fileSelectAndPath') as string, 'error');
      return;
    }
    try {
      await uploadFile(target, file);
      setUploadPath('');
      setChosenFile(null);
      setFileChosenName(null);
      if (fileInputRef) fileInputRef.value = '';
      pushToast(t('fileUploadComplete') as string, 'success');
      await loadList();
    } catch (err) {
      pushToast((err as Error).message, 'error');
    }
  };

  const handleCreateFolder = async () => {
    if (!devMode()) {
      pushToast(t('fileDevModeRequired') as string, 'error');
      return;
    }
    const name = newFolderName().trim();
    if (!name) {
      pushToast(t('fileFolderNameRequired') as string, 'error');
      return;
    }
    try {
      await createFolder(joinPath(currentPath(), name));
      setNewFolderName('');
      pushToast(t('fileFolderCreated') as string, 'success');
      await loadList();
    } catch (err) {
      pushToast((err as Error).message, 'error');
    }
  };

  const handleDelete = async (entry: FileEntry) => {
    if (!devMode()) {
      pushToast(t('fileDevModeRequired') as string, 'error');
      return;
    }
    if (!window.confirm(tf('fileDeleteConfirm', { path: entry.path }))) return;
    try {
      await deletePath(entry.path);
      pushToast(t('fileDeleteComplete') as string, 'success');
      await loadList();
    } catch (err) {
      const msg = (err as Error).message;
      if (entry.is_dir && msg.includes('directory_not_empty')) {
        if (window.confirm(tf('fileDeleteDirNotEmpty', { path: entry.path }))) {
          try {
            await deletePath(entry.path, { recursive: true });
            pushToast(t('fileDeleteComplete') as string, 'success');
            await loadList();
          } catch (err2) {
            pushToast((err2 as Error).message, 'error');
          }
        }
      } else {
        pushToast(msg, 'error');
      }
    }
  };

  const openEditor = async (entry: FileEntry) => {
    setEditor({
      ...initialEditor,
      open: true,
      path: entry.path,
      loading: true,
      readOnly: !devMode(),
    });
    try {
      const { content } = await fetchFileContent(entry.path);
      setEditor((prev) => ({
        ...prev,
        content,
        baseline: content,
        loading: false,
      }));
    } catch (err) {
      setEditor((prev) => ({
        ...prev,
        error: (err as Error).message,
        loading: false,
      }));
    }
  };

  const handleEntryActivate = async (entry: FileEntry) => {
    if (entry.is_dir) {
      setCurrentPath(entry.path);
      return;
    }
    if (!isEditable(entry.path)) {
      pushToast(
        tf('fileUnsupportedAction', {
          action: devMode() ? (t('fileEdit') as string) : (t('filePreview') as string),
        }),
        'error',
      );
      return;
    }
    await openEditor(entry);
  };

  const handleDownloadFolder = async (entry: FileEntry) => {
    if (folderDownload()) return;
    const controller = new AbortController();
    setFolderDownload({
      path: entry.path,
      name: entry.name,
      progress: 0,
      controller,
    });
    try {
      const blob = await downloadFolderTar(
        entry.path,
        (done, total) => {
          setFolderDownload((prev) =>
            prev?.path === entry.path
              ? { ...prev, progress: Math.round((done / total) * 100) }
              : prev,
          );
        },
        controller.signal,
      );
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = entry.name + '.tar';
      a.click();
      URL.revokeObjectURL(url);
      pushToast(t('fileDownloadFolderDone') as string, 'success');
    } catch (err) {
      if ((err as Error).name === 'AbortError') {
        pushToast(t('fileDownloadFolderCancelled') as string, 'info');
      } else {
        pushToast((err as Error).message || (t('fileDownloadFolderFailed') as string), 'error');
      }
    } finally {
      setFolderDownload((prev) => (prev?.path === entry.path ? null : prev));
    }
  };

  const cancelFolderDownload = () => {
    folderDownload()?.controller.abort();
  };

  const reloadEditor = async () => {
    const state = editor();
    if (!state.path) return;
    setEditor({ ...state, loading: true, error: null });
    try {
      const { content } = await fetchFileContent(state.path);
      setEditor((prev) => ({
        ...prev,
        content,
        baseline: content,
        loading: false,
      }));
    } catch (err) {
      setEditor((prev) => ({ ...prev, error: (err as Error).message, loading: false }));
    }
  };

  const saveEditor = async () => {
    const state = editor();
    if (!state.path) return;
    if (state.readOnly) {
      pushToast(t('fileDevModeRequired') as string, 'error');
      return;
    }
    setEditor({ ...state, saving: true, error: null });
    try {
      await saveFileContent(state.path, state.content);
      setEditor((prev) => ({ ...prev, baseline: prev.content, saving: false }));
      pushToast(t('fileEditorSaved') as string, 'success');
      await loadList();
    } catch (err) {
      setEditor((prev) => ({ ...prev, saving: false, error: (err as Error).message }));
    }
  };

  const closeEditor = () => {
    if (dirtyEditor() && !window.confirm(t('unsavedConfirmLeave') as string)) {
      return;
    }
    setEditor(initialEditor);
  };

  const goUp = () => setCurrentPath(parentOf(currentPath()));

  return (
    <TabShell>
      <PageHeader
        title={t('navFiles') as string}
        actions={
          <div class="flex items-center gap-2 flex-wrap">
            <Button size="sm" variant="secondary" active={devMode()} onClick={toggleDevMode}>
              {devMode() ? t('fileDevModeOn') : t('fileDevMode')}
            </Button>
            <Button size="sm" variant="secondary" onClick={loadList} disabled={loading()}>
              {t('fileRefresh')}
            </Button>
          </div>
        }
      />
      <Show when={error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={error() ?? undefined} />
        </div>
      </Show>
      <div class="px-5 pt-4 flex flex-wrap items-center gap-2">
        <Button size="sm" variant="secondary" onClick={goUp} disabled={currentPath() === '/'}>
          {t('fileUpDir')}
        </Button>
        <code class="inline-flex items-center h-9 px-3 rounded-[var(--radius-sm)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-surface)] text-[0.82rem] font-mono text-[var(--color-text-secondary)]">
          {currentPath()}
        </code>
      </div>

      <Show when={devMode()}>
        <div class="px-5 pt-3 flex flex-wrap gap-2 items-center">
          <input
            type="text"
            placeholder={t('fileNewFolder') as string}
            class="flex-1 min-w-[180px] max-w-sm text-[0.82rem] h-9"
            value={newFolderName()}
            onInput={(event) => setNewFolderName(event.currentTarget.value)}
          />
          <Button size="sm" variant="secondary" onClick={handleCreateFolder}>
            {t('fileCreateFolder')}
          </Button>
        </div>
        <div class="px-5 pt-3 flex flex-wrap gap-2 items-center">
          <input
            type="text"
            placeholder={t('fileUploadPath') as string}
            class="flex-1 min-w-[180px] max-w-sm text-[0.82rem] h-9"
            value={uploadPath()}
            onInput={(event) => setUploadPath(event.currentTarget.value)}
          />
          <div class="flex items-center gap-2 flex-1 min-w-[180px] h-9 px-2 rounded-[var(--radius-sm)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-input)]">
            <input
              ref={fileInputRef}
              type="file"
              class="hidden"
              onChange={(event) => {
                const file = event.currentTarget.files?.[0] ?? null;
                setChosenFile(file);
                setFileChosenName(file?.name ?? null);
                if (file) {
                  setUploadPath(joinPath(currentPath(), file.name));
                }
              }}
            />
            <Button variant="ghost" size="xs" onClick={() => fileInputRef?.click()}>
              {t('fileChoose')}
            </Button>
            <span class="text-[0.82rem] text-[var(--color-text-muted)] flex-1 truncate">
              {fileChosenName() ?? t('fileNoFileSelected')}
            </span>
          </div>
          <Button size="sm" variant="primary" onClick={handleUpload}>
            {t('fileUpload')}
          </Button>
        </div>
      </Show>

      <div class="p-5">
        <div class="rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-surface)] overflow-hidden">
          <table class="w-full">
            <thead>
              <tr class="bg-[var(--color-bg-card)]">
                <th class="text-left px-4 py-2.5 text-[0.72rem] font-bold uppercase tracking-wider text-[var(--color-text-muted)]">
                  {t('fileColName')}
                </th>
                <th class="text-left px-4 py-2.5 text-[0.72rem] font-bold uppercase tracking-wider text-[var(--color-text-muted)]">
                  {t('fileColType')}
                </th>
                <th class="text-left px-4 py-2.5 text-[0.72rem] font-bold uppercase tracking-wider text-[var(--color-text-muted)]">
                  {t('fileColSize')}
                </th>
                <th class="text-left px-4 py-2.5 text-[0.72rem] font-bold uppercase tracking-wider text-[var(--color-text-muted)]">
                  {t('fileColActions')}
                </th>
              </tr>
            </thead>
            <tbody>
              <Show
                when={entries().length > 0}
                fallback={
                  <tr>
                    <td colspan={4} class="px-4 py-6 text-center text-[var(--color-text-muted)]">
                      {t('fileEmpty')}
                    </td>
                  </tr>
                }
              >
                <For each={entries()}>
                  {(entry) => (
                    <tr class="border-t border-[var(--color-border-subtle)] hover:bg-white/[0.02]">
                      <td
                        class="px-4 py-2.5 text-[0.88rem] text-[var(--color-text-primary)] break-all cursor-pointer"
                        onClick={() => void handleEntryActivate(entry)}
                      >
                        {entry.name}
                      </td>
                      <td
                        class="px-4 py-2.5 text-[0.82rem] text-[var(--color-text-secondary)] cursor-pointer"
                        onClick={() => void handleEntryActivate(entry)}
                      >
                        {entry.is_dir ? t('fileTypeFolder') : t('fileTypeFile')}
                      </td>
                      <td
                        class="px-4 py-2.5 text-[0.82rem] text-[var(--color-text-secondary)] cursor-pointer"
                        onClick={() => void handleEntryActivate(entry)}
                      >
                        {entry.is_dir ? '—' : humanSize(entry.size ?? 0)}
                      </td>
                      <td class="px-4 py-2.5">
                        <div
                          class="flex flex-wrap items-center gap-1.5"
                          onClick={(event) => event.stopPropagation()}
                        >
                          <Show
                            when={entry.is_dir}
                            fallback={
                              <a
                                href={downloadHref(entry.path)}
                                target="_blank"
                                rel="noopener"
                                class="inline-flex h-8 w-8 items-center justify-center rounded-[var(--radius-sm)] text-white hover:bg-white/[0.05]"
                                title={t('fileDownload') as string}
                                aria-label={t('fileDownload') as string}
                              >
                                <HardDriveDownload class="h-4 w-4 shrink-0" />
                              </a>
                            }
                          >
                            <button
                              type="button"
                              class="inline-flex h-8 w-8 items-center justify-center rounded-[var(--radius-sm)] text-[var(--color-text-primary)] hover:bg-white/[0.05] disabled:opacity-50 disabled:cursor-not-allowed"
                              disabled={folderDownload() !== null}
                              onClick={() => void handleDownloadFolder(entry)}
                              title={t('fileDownloadFolder') as string}
                              aria-label={t('fileDownloadFolder') as string}
                            >
                              <HardDriveDownload class="h-4 w-4 shrink-0" />
                            </button>
                          </Show>
                          <button
                            type="button"
                            class="inline-flex h-8 w-8 items-center justify-center rounded-[var(--radius-sm)] text-[var(--color-danger)] hover:bg-[rgba(248,113,113,0.08)] disabled:cursor-not-allowed disabled:opacity-50"
                            disabled={!devMode()}
                            onClick={() => void handleDelete(entry)}
                            title={t('fileDelete') as string}
                            aria-label={t('fileDelete') as string}
                          >
                            <Trash2 class="h-4 w-4 shrink-0" />
                          </button>
                        </div>
                      </td>
                    </tr>
                  )}
                </For>
              </Show>
            </tbody>
          </table>
        </div>
      </div>

      <FileEditorModal
        state={editor}
        onClose={closeEditor}
        onContentChange={(value) => setEditor((prev) => ({ ...prev, content: value }))}
        onReload={reloadEditor}
        onSave={saveEditor}
      />
      <FolderDownloadPopup state={folderDownload} onCancel={cancelFolderDownload} />
    </TabShell>
  );
};

const FolderDownloadPopup: Component<{
  state: () => FolderDownloadState | null;
  onCancel: () => void;
}> = (props) => {
  const progressWidth = () => `${Math.max(0, Math.min(100, props.state()?.progress ?? 0))}%`;

  return (
    <Show when={props.state()}>
      {(download) => (
        <Portal>
          <div
            class="fixed bottom-5 right-5 z-[1100] w-[min(22rem,calc(100vw-2.5rem))] rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-card)] shadow-[0_18px_48px_rgba(0,0,0,0.38)]"
            role="status"
            aria-live="polite"
          >
            <div class="flex items-start justify-between gap-3 p-4 pb-3">
              <div class="min-w-0">
                <div class="text-[0.86rem] font-semibold text-[var(--color-text-primary)]">
                  {t('fileDownloadingFolder')}
                </div>
                <div class="mt-1 truncate text-[0.76rem] text-[var(--color-text-muted)]">
                  {download().name}
                </div>
              </div>
              <button
                type="button"
                class="inline-flex h-7 w-7 shrink-0 items-center justify-center rounded-[var(--radius-sm)] text-[var(--color-text-secondary)] hover:bg-white/[0.05] hover:text-[var(--color-text-primary)]"
                onClick={props.onCancel}
                title={t('fileDownloadFolderCancel') as string}
                aria-label={t('fileDownloadFolderCancel') as string}
              >
                <X class="h-4 w-4" />
              </button>
            </div>
            <div class="px-4 pb-4">
              <div class="mb-2 flex items-center justify-between gap-3 text-[0.76rem] text-[var(--color-text-muted)]">
                <span class="min-w-0 truncate">{download().path}</span>
                <span>{download().progress}%</span>
              </div>
              <div class="h-2 overflow-hidden rounded-full bg-white/[0.08]">
                <div
                  class="h-full rounded-full bg-[var(--color-accent)] transition-[width] duration-200"
                  style={{ width: progressWidth() }}
                />
              </div>
              <Button class="mt-3 w-full" size="sm" variant="danger-ghost" onClick={props.onCancel}>
                {t('fileDownloadFolderCancel')}
              </Button>
            </div>
          </div>
        </Portal>
      )}
    </Show>
  );
};

const FileEditorModal: Component<{
  state: () => EditorState;
  onClose: () => void;
  onContentChange: (value: string) => void;
  onReload: () => void;
  onSave: () => void;
}> = (props) => {
  return (
    <Modal
      open={props.state().open}
      onClose={props.onClose}
      title={t('fileEditorTitle') as string}
      subtitle={
        <code class="font-mono text-[0.8rem] text-[var(--color-text-muted)]">
          {props.state().path}
        </code>
      }
      actions={
        <>
          <Button
            size="sm"
            variant="secondary"
            onClick={props.onReload}
            disabled={props.state().loading}
          >
            {t('fileEditorRefresh')}
          </Button>
          <Show when={!props.state().readOnly}>
            <Button
              size="sm"
              variant="primary"
              onClick={props.onSave}
              disabled={props.state().saving}
            >
              {props.state().saving ? '…' : t('fileEditorSave')}
            </Button>
          </Show>
        </>
      }
    >
      <div class="p-5 flex flex-col gap-3">
        <Show when={props.state().error}>
          <Banner kind="error" message={props.state().error ?? undefined} />
        </Show>
        <textarea
          spellcheck={false}
          readonly={props.state().readOnly}
          class="w-full min-h-[420px] rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-input)] p-4 text-[0.88rem] leading-6 font-mono text-[var(--color-text-primary)] focus:outline-none focus:border-[rgba(232,54,45,0.4)]"
          value={props.state().content}
          onInput={(event) => props.onContentChange(event.currentTarget.value)}
        />
      </div>
    </Modal>
  );
};
