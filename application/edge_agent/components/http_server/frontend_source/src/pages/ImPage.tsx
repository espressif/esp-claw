import {
  batch,
  createEffect,
  createSignal,
  onCleanup,
  Show,
  type Component,
  type JSX,
  type Setter,
} from 'solid-js';
import { type SetStoreFunction } from 'solid-js/store';
import { generate } from 'lean-qr';
import { t } from '../i18n';
import type { AppConfig, WechatLoginStatus } from '../api/client';
import { cancelWechatLogin, pollWechatLoginStatus, startWechatLogin } from '../api/client';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { TextInput, SelectInput } from '../components/ui/FormField';
import { SavePanel } from '../components/ui/SavePanel';
import { Button } from '../components/ui/Button';
import { Banner } from '../components/ui/Banner';
import { pushToast } from '../state/toast';

type ImForm = {
  wechat_token: string;
  wechat_base_url: string;
  wechat_cdn_base_url: string;
  wechat_account_id: string;
  qq_app_id: string;
  qq_app_secret: string;
  qq_msg_type: string;
  feishu_app_id: string;
  feishu_app_secret: string;
  tg_bot_token: string;
};

/* ── Shared styles ──────────────────────────────────────────────────── */

const ROW_CLASS =
  'flex items-center gap-3 py-3 px-5 hover:bg-white/[0.015] transition-colors select-none';

/* ── Platform row ───────────────────────────────────────────────────── */

type PlatformRowProps = {
  label: string;
  /** Non-empty identifier field means "configured". */
  configured: boolean;
  open: boolean;
  onToggle: () => void;
  onDelete: () => void;
  children: JSX.Element;
};

const ChevronIcon: Component<{ open: boolean }> = (props) => (
  <svg
    class={[
      'w-3.5 h-3.5 transition-transform duration-150 shrink-0',
      props.open ? 'rotate-90' : '',
    ].join(' ')}
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    stroke-width="2.5"
    stroke-linecap="round"
    stroke-linejoin="round"
    aria-hidden="true"
  >
    <path d="M9 6l6 6-6 6" />
  </svg>
);

const PlatformRow: Component<PlatformRowProps> = (props) => (
  <div>
    {/* Header row — always clickable */}
    <div
      class={[ROW_CLASS, 'cursor-pointer'].join(' ')}
      onClick={props.onToggle}
      role="button"
      aria-expanded={props.open}
      tabIndex={0}
      onKeyDown={(e) => {
        if (e.key === 'Enter' || e.key === ' ') {
          e.preventDefault();
          props.onToggle();
        }
      }}
    >
      <span class="flex-1 flex items-center gap-2 min-w-0">
        <span class="text-[0.9rem] font-semibold text-[var(--color-text-primary)] truncate">
          {props.label}
        </span>
        <Show
          when={props.configured}
          fallback={
            <span class="text-[0.75rem] text-[var(--color-text-muted)] italic">
              {t('imNotConfigured')}
            </span>
          }
        >
          <span class="inline-flex items-center gap-1 text-[0.74rem] font-medium text-emerald-400 bg-emerald-500/10 px-1.5 py-0.5 rounded-full">
            <span class="w-1.5 h-1.5 rounded-full bg-emerald-400 inline-block" />
          </span>
        </Show>
      </span>

      {/* Right-side: delete (configured) or just chevron */}
      <div class="flex items-center gap-2 text-[var(--color-text-muted)]">
        <Show when={props.configured}>
          <Button
            size="xs"
            variant="ghost"
            onClick={(e) => {
              e.stopPropagation();
              props.onDelete();
            }}
          >
            {t('imClearConf')}
          </Button>
        </Show>
        <ChevronIcon open={props.open} />
      </div>
    </div>

    <Show when={props.open}>
      <div class="px-5 pb-5 pt-1 bg-white/[0.015] border-t border-[var(--color-border-subtle)]">
        {props.children}
      </div>
    </Show>
  </div>
);

/* ── WeChat config panel ────────────────────────────────────────────── */

type WechatConfigProps = {
  form: ImForm;
  setForm: SetStoreFunction<ImForm>;
  onLoginComplete: (data: WechatLoginStatus) => void;
};

const WechatConfig: Component<WechatConfigProps> = (props) => {
  const [showAdvanced, setShowAdvanced] = createSignal(false);

  const handleLoginComplete = (data: WechatLoginStatus) => {
    /* Expand advanced so user can see filled fields */
    setShowAdvanced(true);
    props.onLoginComplete(data);
  };

  return (
    <div class="flex flex-col gap-3 pt-2">
      <WechatQrPanel onComplete={handleLoginComplete} />

      {/* Advanced settings toggle */}
      <button
        type="button"
        class="flex items-center gap-1.5 text-[0.82rem] text-[var(--color-text-muted)] hover:text-[var(--color-text-primary)] transition-colors w-fit"
        onClick={() => setShowAdvanced((v) => !v)}
        aria-expanded={showAdvanced()}
      >
        <ChevronIcon open={showAdvanced()} />
        <span>{t('imAdvancedSettings')}</span>
      </button>

      <Show when={showAdvanced()}>
        <div class="grid gap-3 sm:grid-cols-2 pt-1">
          <TextInput
            type="password"
            label={t('wechatToken')}
            value={props.form.wechat_token}
            onInput={(e) => props.setForm('wechat_token', e.currentTarget.value)}
          />
          <TextInput
            label={t('wechatAccountId')}
            placeholder={t('wechatAccountIdPlaceholder') as string}
            value={props.form.wechat_account_id}
            onInput={(e) => props.setForm('wechat_account_id', e.currentTarget.value)}
          />
          <TextInput
            label={t('wechatBaseUrl')}
            placeholder={t('wechatBaseUrlPlaceholder') as string}
            value={props.form.wechat_base_url}
            onInput={(e) => props.setForm('wechat_base_url', e.currentTarget.value)}
          />
          <TextInput
            label={t('wechatCdnBaseUrl')}
            placeholder={t('wechatCdnBaseUrlPlaceholder') as string}
            value={props.form.wechat_cdn_base_url}
            onInput={(e) => props.setForm('wechat_cdn_base_url', e.currentTarget.value)}
          />
        </div>
      </Show>
    </div>
  );
};

/* ── WeChat QR login panel ──────────────────────────────────────────── */

const WechatQrPanel: Component<{ onComplete: (data: WechatLoginStatus) => void }> = (props) => {
  const [status, setStatus] = createSignal<WechatLoginStatus | null>(null);
  const [busy, setBusy] = createSignal(false);
  const [error, setError] = createSignal<string | null>(null);
  let canvasRef: HTMLCanvasElement | undefined;
  let pollTimer: ReturnType<typeof setTimeout> | null = null;

  const stopPolling = () => {
    if (pollTimer) {
      clearTimeout(pollTimer);
      pollTimer = null;
    }
  };

  const renderQr = (data?: string) => {
    if (!canvasRef) return;
    if (!data) {
      canvasRef.getContext('2d')?.clearRect(0, 0, canvasRef.width, canvasRef.height);
      return;
    }
    try {
      generate(data).toCanvas(canvasRef);
    } catch {
      /* ignore render errors */
    }
  };

  const applyStatus = (data: WechatLoginStatus) => {
    setStatus(data);
    setError(null);
    renderQr(data.qr_data_url);
    if (data.completed && data.token) {
      stopPolling();
      props.onComplete(data);
    }
  };

  const poll = async () => {
    try {
      const data = await pollWechatLoginStatus();
      applyStatus(data);
      if (data.active && !data.completed) {
        pollTimer = setTimeout(poll, 1500);
      }
    } catch (err) {
      setError((err as Error).message);
      stopPolling();
    }
  };

  /* Poll once on mount to restore any in-progress session. */
  poll();

  onCleanup(stopPolling);

  const startLogin = async () => {
    setBusy(true);
    setError(null);
    stopPolling();
    try {
      const data = await startWechatLogin('', true);
      applyStatus(data);
      pollTimer = setTimeout(poll, 1000);
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setBusy(false);
    }
  };

  const cancelLogin = async () => {
    setBusy(true);
    setError(null);
    stopPolling();
    try {
      await cancelWechatLogin();
      setStatus({ status: 'cancelled', message: t('wechatLoginCancelledMsg') as string });
      renderQr('');
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setBusy(false);
    }
  };

  const statusText = () => {
    const s = status()?.status;
    return s
      ? ((t('wechatLoginStatusPrefix') as string) + s)
      : (t('wechatLoginStatus') as string);
  };

  return (
    <div class="flex flex-col gap-3">
      {/* Controls */}
      <div class="flex items-center justify-between gap-3 flex-wrap">
        <p class="text-[0.82rem] text-[var(--color-text-muted)] m-0">{statusText()}</p>
        <div class="flex gap-2">
          <Button size="sm" variant="secondary" onClick={startLogin} disabled={busy()}>
            {t('wechatLoginGenerate')}
          </Button>
          <Button
            size="sm"
            variant="secondary"
            onClick={cancelLogin}
            disabled={busy() || !status()?.active}
          >
            {t('wechatLoginCancel')}
          </Button>
        </div>
      </div>

      <Banner kind="error" message={error() ?? undefined} />
      <Banner kind="info" message={status()?.message ?? undefined} />

      <Show when={status()?.qr_data_url}>
        <div class="flex flex-col items-center gap-2 py-2">
          <canvas
            ref={canvasRef}
            class="block w-full max-w-[200px] rounded-xl bg-white p-2.5"
            style={{ 'image-rendering': 'pixelated' }}
          />
          <a
            href={status()!.qr_data_url!}
            target="_blank"
            rel="noopener noreferrer"
            class="text-[0.82rem] text-[var(--color-accent-soft)] hover:underline"
          >
            {t('wechatLoginOpenLink')}
          </a>
        </div>
      </Show>

      <p class="text-[0.76rem] text-[var(--color-text-muted)] leading-relaxed m-0">
        {t('wechatLoginNote')}
      </p>
    </div>
  );
};

/* ── Main page ──────────────────────────────────────────────────────── */

export const ImPage: Component = () => {
  const tab = createConfigTab<ImForm>({
    tab: 'im',
    groups: ['im'],
    toForm: (config: Partial<AppConfig>) => ({
      wechat_token: config.wechat_token ?? '',
      wechat_base_url: config.wechat_base_url ?? '',
      wechat_cdn_base_url: config.wechat_cdn_base_url ?? '',
      wechat_account_id: config.wechat_account_id ?? '',
      qq_app_id: config.qq_app_id ?? '',
      qq_app_secret: config.qq_app_secret ?? '',
      qq_msg_type: config.qq_msg_type ?? '0',
      feishu_app_id: config.feishu_app_id ?? '',
      feishu_app_secret: config.feishu_app_secret ?? '',
      tg_bot_token: config.tg_bot_token ?? '',
    }),
    fromForm: (form) => ({
      wechat_token: form.wechat_token,
      wechat_base_url: form.wechat_base_url.trim(),
      wechat_cdn_base_url: form.wechat_cdn_base_url.trim(),
      wechat_account_id: form.wechat_account_id.trim(),
      qq_app_id: form.qq_app_id.trim(),
      qq_app_secret: form.qq_app_secret,
      qq_msg_type: form.qq_msg_type,
      feishu_app_id: form.feishu_app_id.trim(),
      feishu_app_secret: form.feishu_app_secret,
      tg_bot_token: form.tg_bot_token.trim(),
    }),
  });
  const [validationError, setValidationError] = createSignal<string | null>(null);

  createEffect(() => {
    void tab.form.wechat_token;
    void tab.form.wechat_base_url;
    void tab.form.wechat_cdn_base_url;
    void tab.form.wechat_account_id;
    void tab.form.qq_app_id;
    void tab.form.qq_app_secret;
    void tab.form.qq_msg_type;
    void tab.form.feishu_app_id;
    void tab.form.feishu_app_secret;
    void tab.form.tg_bot_token;
    setValidationError(null);
  });

  /* Per-platform open state */
  const [openSet, setOpenSet] = createSignal(new Set<string>());
  const isOpen = (id: string) => openSet().has(id);
  const toggle = (id: string) =>
    setOpenSet((prev) => {
      const next = new Set(prev);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });
  const close = (id: string) =>
    setOpenSet((prev) => {
      const next = new Set(prev);
      next.delete(id);
      return next;
    });

  /* Derived "configured" states */
  const isWechatConfigured = () => !!tab.form.wechat_token.trim();
  const isQqConfigured = () => !!tab.form.qq_app_id.trim();
  const isFeishuConfigured = () => !!tab.form.feishu_app_id.trim();
  const isTelegramConfigured = () => !!tab.form.tg_bot_token.trim();

  const clearPlatform = (
    id: string,
    fields: (keyof ImForm)[],
    setFn: (key: keyof ImForm, val: string) => void,
  ) => {
    fields.forEach((f) => setFn(f, ''));
    close(id);
  };

  const setField = (key: keyof ImForm, value: string) => tab.setForm(key as any, value);

  /* WeChat QR login completed → populate form fields, panel is already open */
  const onWechatLoginComplete = (data: WechatLoginStatus) => {
    batch(() => {
      if (data.token) tab.setForm('wechat_token', data.token);
      if (data.base_url) tab.setForm('wechat_base_url', data.base_url);
      if (data.account_id) tab.setForm('wechat_account_id', data.account_id);
    });
    pushToast(t('imWechatCredsFilled') as string, 'info', 6000);
  };

  const validatePlatform = (label: string, fields: Array<[string, string]>) => {
    const hasAny = fields.some(([value]) => value.trim().length > 0);
    if (!hasAny) return null;
    const missing = fields.filter(([value]) => value.trim().length === 0).map(([, name]) => name);
    if (missing.length === 0) return null;
    return (t('imValidationIncompletePlatform') as string)
      .replace('{platform}', label)
      .replace('{fields}', missing.join(' / '));
  };

  const validateWechat = () => {
    const token = tab.form.wechat_token.trim();
    const baseUrl = tab.form.wechat_base_url.trim();
    const cdnBaseUrl = tab.form.wechat_cdn_base_url.trim();
    const accountId = tab.form.wechat_account_id.trim();
    const normalizedAccountId = accountId.toLowerCase();

    const onlyDefaultPrefill =
      !token &&
      !!baseUrl &&
      !!cdnBaseUrl &&
      (!accountId || normalizedAccountId === 'default');

    if (onlyDefaultPrefill) return null;

    const startedConfig = !!token || !!accountId;
    if (!startedConfig) return null;

    const missing = [
      !token ? (t('wechatToken') as string) : null,
      !baseUrl ? (t('wechatBaseUrl') as string) : null,
      !cdnBaseUrl ? (t('wechatCdnBaseUrl') as string) : null,
      !accountId ? (t('wechatAccountId') as string) : null,
    ].filter((value): value is string => !!value);

    if (missing.length === 0) return null;

    return (t('imValidationIncompletePlatform') as string)
      .replace('{platform}', t('imWechatTitle') as string)
      .replace('{fields}', missing.join(' / '));
  };

  const handleSave = async () => {
    const message =
      validateWechat() ??
      validatePlatform(t('imQqTitle') as string, [
        [tab.form.qq_app_id, t('qqAppId') as string],
        [tab.form.qq_app_secret, t('qqAppSecret') as string],
      ]) ??
      validatePlatform(t('imFeishuTitle') as string, [
        [tab.form.feishu_app_id, t('feishuAppId') as string],
        [tab.form.feishu_app_secret, t('feishuAppSecret') as string],
      ]) ??
      validatePlatform(t('imTelegramTitle') as string, [
        [tab.form.tg_bot_token, t('tgBotToken') as string],
      ]);

    if (message) {
      setValidationError(message);
      pushToast(message, 'error', 5000);
      return;
    }

    await tab.save();
  };

  return (
    <TabShell>
      <PageHeader title={t('navIm') as string} description={t('sectionIm') as string} />

      <Show when={validationError() ?? tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={validationError() ?? tab.error() ?? undefined} />
        </div>
      </Show>

      {/* Platform list — flat rows with dividers, no card borders */}
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        {/* WeChat */}
        <PlatformRow
          label={t('imWechatTitle') as string}
          configured={isWechatConfigured()}
          open={isOpen('wechat')}
          onToggle={() => toggle('wechat')}
          onDelete={() =>
            clearPlatform(
              'wechat',
              ['wechat_token', 'wechat_base_url', 'wechat_cdn_base_url', 'wechat_account_id'],
              setField,
            )
          }
        >
          <WechatConfig
            form={tab.form}
            setForm={tab.setForm}
            onLoginComplete={onWechatLoginComplete}
          />
        </PlatformRow>

        {/* QQ */}
        <PlatformRow
          label={t('imQqTitle') as string}
          configured={isQqConfigured()}
          open={isOpen('qq')}
          onToggle={() => toggle('qq')}
          onDelete={() => clearPlatform('qq', ['qq_app_id', 'qq_app_secret'], setField)}
        >
          <div class="grid gap-3 sm:grid-cols-2 pt-2">
            <TextInput
              label={t('qqAppId')}
              value={tab.form.qq_app_id}
              onInput={(e) => tab.setForm('qq_app_id', e.currentTarget.value)}
            />
            <TextInput
              type="password"
              label={t('qqAppSecret')}
              value={tab.form.qq_app_secret}
              onInput={(e) => tab.setForm('qq_app_secret', e.currentTarget.value)}
            />
            <SelectInput
              label={t('qqMsgType')}
              value={tab.form.qq_msg_type}
              onChange={(e) => tab.setForm('qq_msg_type', e.currentTarget.value)}
            >
              <option value="0">{t('qqMsgTypePlain')}</option>
              <option value="2">{t('qqMsgTypeMarkdown')}</option>
            </SelectInput>
          </div>
        </PlatformRow>

        {/* Feishu */}
        <PlatformRow
          label={t('imFeishuTitle') as string}
          configured={isFeishuConfigured()}
          open={isOpen('feishu')}
          onToggle={() => toggle('feishu')}
          onDelete={() =>
            clearPlatform('feishu', ['feishu_app_id', 'feishu_app_secret'], setField)
          }
        >
          <div class="grid gap-3 sm:grid-cols-2 pt-2">
            <TextInput
              label={t('feishuAppId')}
              value={tab.form.feishu_app_id}
              onInput={(e) => tab.setForm('feishu_app_id', e.currentTarget.value)}
            />
            <TextInput
              type="password"
              label={t('feishuAppSecret')}
              value={tab.form.feishu_app_secret}
              onInput={(e) => tab.setForm('feishu_app_secret', e.currentTarget.value)}
            />
          </div>
        </PlatformRow>

        {/* Telegram */}
        <PlatformRow
          label={t('imTelegramTitle') as string}
          configured={isTelegramConfigured()}
          open={isOpen('telegram')}
          onToggle={() => toggle('telegram')}
          onDelete={() => clearPlatform('telegram', ['tg_bot_token'], setField)}
        >
          <div class="pt-2">
            <TextInput
              full
              type="password"
              label={t('tgBotToken')}
              value={tab.form.tg_bot_token}
              onInput={(e) => tab.setForm('tg_bot_token', e.currentTarget.value)}
            />
          </div>
        </PlatformRow>
      </div>

      <SavePanel
        dirty={tab.dirty()}
        saving={tab.saving()}
        onSave={() => handleSave().catch(() => undefined)}
        onDiscard={tab.discard}
        note={t('restartHint') as string}
      />
    </TabShell>
  );
};
