import {
  For,
  Show,
  batch,
  createMemo,
  createSignal,
  Match,
  onCleanup,
  onMount,
  Switch,
  type Component,
} from 'solid-js';
import { createStore } from 'solid-js/store';
import { generate } from 'lean-qr';
import type { AppConfig, WechatLoginStatus } from '../api/client';
import {
  cancelWechatLogin,
  pollWechatLoginStatus,
  saveConfigPatch,
  startWechatLogin,
} from '../api/client';
import { LanguageSwitcher } from '../components/layout/LanguageSwitcher';
import { Banner } from '../components/ui/Banner';
import { Button } from '../components/ui/Button';
import { SelectInput, TextInput } from '../components/ui/FormField';
import { t } from '../i18n';
import { appConfig, ensureConfigGroups, patchConfigLocal } from '../state/config';
import { pushToast } from '../state/toast';

type TabId =
  | 'status'
  | 'basic'
  | 'llm'
  | 'im'
  | 'search'
  | 'memory'
  | 'webim'
  | 'capabilities'
  | 'skills'
  | 'files';

type SetupWizardPageProps = {
  onRestartRequest: (targetTab: TabId) => void;
};

type ProviderKey = 'openai' | 'bailian' | 'deepseek' | 'anthropic' | 'compatible';
type CompatibleKind = 'openai' | 'claude';
type PlatformId = 'wechat' | 'feishu' | 'qq' | 'telegram';

type LlmForm = {
  llm_api_key: string;
  llm_model: string;
  llm_timeout_ms: string;
  llm_max_tokens: string;
  llm_backend_type: string;
  llm_profile: string;
  llm_base_url: string;
  llm_auth_type: string;
};

type ImForm = {
  wechat_token: string;
  wechat_base_url: string;
  wechat_cdn_base_url: string;
  wechat_account_id: string;
  qq_app_id: string;
  qq_app_secret: string;
  feishu_app_id: string;
  feishu_app_secret: string;
  tg_bot_token: string;
};

type SearchForm = {
  search_brave_key: string;
  search_tavily_key: string;
};

type ProviderPreset = {
  llm_backend_type: string;
  llm_profile: string;
  llm_base_url: string;
  llm_auth_type: string;
  llm_model: string;
};

const PROVIDER_PRESETS: Record<Exclude<ProviderKey, 'compatible'>, ProviderPreset> = {
  openai: {
    llm_backend_type: 'openai_compatible',
    llm_profile: 'openai',
    llm_base_url: 'https://api.openai.com/v1',
    llm_auth_type: 'bearer',
    llm_model: 'gpt-5.4',
  },
  bailian: {
    llm_backend_type: 'openai_compatible',
    llm_profile: 'qwen_compatible',
    llm_base_url: 'https://dashscope.aliyuncs.com/compatible-mode/v1',
    llm_auth_type: 'bearer',
    llm_model: 'qwen3.6-plus',
  },
  deepseek: {
    llm_backend_type: 'openai_compatible',
    llm_profile: 'custom_openai_compatible',
    llm_base_url: 'https://api.deepseek.com',
    llm_auth_type: 'bearer',
    llm_model: 'deepseek-v4-pro',
  },
  anthropic: {
    llm_backend_type: 'anthropic',
    llm_profile: 'anthropic',
    llm_base_url: 'https://api.anthropic.com/v1',
    llm_auth_type: 'none',
    llm_model: 'claude-sonnet-4-6',
  },
};

const PLATFORM_ORDER: PlatformId[] = ['wechat', 'feishu', 'qq', 'telegram'];
const WECHAT_DEFAULT_CDN_BASE_URL = 'https://novac2c.cdn.weixin.qq.com/c2c';

function llmFromConfig(config: Partial<AppConfig>): LlmForm {
  return {
    llm_api_key: config.llm_api_key ?? '',
    llm_model: config.llm_model ?? '',
    llm_timeout_ms: config.llm_timeout_ms ?? '',
    llm_max_tokens: config.llm_max_tokens ?? '',
    llm_backend_type: config.llm_backend_type ?? '',
    llm_profile: config.llm_profile ?? '',
    llm_base_url: config.llm_base_url ?? '',
    llm_auth_type: config.llm_auth_type ?? '',
  };
}

function imFromConfig(config: Partial<AppConfig>): ImForm {
  return {
    wechat_token: config.wechat_token ?? '',
    wechat_base_url: config.wechat_base_url ?? '',
    wechat_cdn_base_url: config.wechat_cdn_base_url ?? '',
    wechat_account_id: config.wechat_account_id ?? '',
    qq_app_id: config.qq_app_id ?? '',
    qq_app_secret: config.qq_app_secret ?? '',
    feishu_app_id: config.feishu_app_id ?? '',
    feishu_app_secret: config.feishu_app_secret ?? '',
    tg_bot_token: config.tg_bot_token ?? '',
  };
}

function searchFromConfig(config: Partial<AppConfig>): SearchForm {
  return {
    search_brave_key: config.search_brave_key ?? '',
    search_tavily_key: config.search_tavily_key ?? '',
  };
}

function detectProvider(form: LlmForm): ProviderKey {
  for (const key of Object.keys(PROVIDER_PRESETS) as Exclude<ProviderKey, 'compatible'>[]) {
    const preset = PROVIDER_PRESETS[key];
    if (
      preset.llm_backend_type === form.llm_backend_type.trim() &&
      preset.llm_profile === form.llm_profile.trim() &&
      preset.llm_base_url === form.llm_base_url.trim() &&
      preset.llm_auth_type === form.llm_auth_type.trim()
    ) {
      return key;
    }
  }
  return 'compatible';
}

function detectCompatibleKind(form: LlmForm): CompatibleKind {
  return form.llm_backend_type.trim() === 'anthropic' ? 'claude' : 'openai';
}

function selectedPlatformsFromForm(form: ImForm): PlatformId[] {
  const out: PlatformId[] = [];
  if (isWechatConfiguredLikeIm(form)) {
    out.push('wechat');
  }
  if (form.feishu_app_id.trim() || form.feishu_app_secret.trim()) out.push('feishu');
  if (form.qq_app_id.trim() || form.qq_app_secret.trim()) out.push('qq');
  if (form.tg_bot_token.trim()) out.push('telegram');
  return out;
}

function platformLabel(id: PlatformId): string {
  switch (id) {
    case 'wechat':
      return t('imWechatTitle') as string;
    case 'feishu':
      return t('imFeishuTitle') as string;
    case 'qq':
      return t('imQqTitle') as string;
    case 'telegram':
      return t('imTelegramTitle') as string;
  }
}

function isWechatConfiguredLikeIm(form: ImForm): boolean {
  const token = form.wechat_token.trim();
  const baseUrl = form.wechat_base_url.trim();
  const cdnBaseUrl = form.wechat_cdn_base_url.trim();
  const accountId = form.wechat_account_id.trim();
  const normalizedAccountId = accountId.toLowerCase();

  const onlyDefaultPrefill =
    !token &&
    !!baseUrl &&
    !!cdnBaseUrl &&
    (!accountId || normalizedAccountId === 'default');

  if (onlyDefaultPrefill) return false;
  return !!token || !!accountId;
}

function isHttpUrl(value: string): boolean {
  try {
    const url = new URL(value);
    return url.protocol === 'http:' || url.protocol === 'https:';
  } catch {
    return false;
  }
}

function isTelegramToken(value: string): boolean {
  return /^\d+:[A-Za-z0-9_-]{20,}$/.test(value);
}

const WechatWizardPanel: Component<{
  configured: boolean;
  accountId: string;
  onConfigured: (data: WechatLoginStatus) => void;
}> = (props) => {
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
      props.onConfigured(data);
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

  onMount(() => {
    void poll();
  });

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
    <div class="flex flex-col gap-4 mt-4">
      <Show
        when={!props.configured}
        fallback={
          <Banner kind="success">
            <div class="flex flex-wrap items-center justify-between gap-2">
              <span>{t('setupWechatConfigured')}</span>
              <span class="font-mono text-[0.8rem] text-[var(--color-text-primary)]">
                AccountID: {props.accountId || (t('sysInfoNone') as string)}
              </span>
            </div>
          </Banner>
        }
      >
        <>
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
        </>
      </Show>
    </div>
  );
};

export const SetupWizardPage: Component<SetupWizardPageProps> = (props) => {
  const [step, setStep] = createSignal(0);
  const [loading, setLoading] = createSignal(true);
  const [saving, setSaving] = createSignal(false);
  const [error, setError] = createSignal<string | null>(null);
  const [llmForm, setLlmForm] = createStore<LlmForm>(llmFromConfig({}));
  const [imForm, setImForm] = createStore<ImForm>(imFromConfig({}));
  const [searchForm, setSearchForm] = createStore<SearchForm>(searchFromConfig({}));
  const [provider, setProvider] = createSignal<ProviderKey>('openai');
  const [compatibleKind, setCompatibleKind] = createSignal<CompatibleKind>('openai');
  const [selectedPlatforms, setSelectedPlatforms] = createSignal<PlatformId[]>([]);
  const [wechatAdvancedOpen, setWechatAdvancedOpen] = createSignal(false);
  const isWechatConfigured = createMemo(() => isWechatConfiguredLikeIm(imForm));

  onMount(async () => {
    setLoading(true);
    setError(null);
    try {
      await ensureConfigGroups(['llm', 'im', 'search']);
      batch(() => {
        const config = appConfig();
        const nextLlm = llmFromConfig(config);
        const nextIm = imFromConfig(config);
        const hasAnyLlmConfig = [
          nextLlm.llm_api_key,
          nextLlm.llm_model,
          nextLlm.llm_backend_type,
          nextLlm.llm_profile,
          nextLlm.llm_base_url,
        ].some((value) => value.trim().length > 0);
        setLlmForm(nextLlm);
        setImForm(nextIm);
        setSearchForm(searchFromConfig(config));
        setProvider(hasAnyLlmConfig ? detectProvider(nextLlm) : 'openai');
        setCompatibleKind(detectCompatibleKind(nextLlm));
        setSelectedPlatforms(selectedPlatformsFromForm(nextIm));
        setWechatAdvancedOpen(
          !!(nextIm.wechat_token || nextIm.wechat_base_url || nextIm.wechat_account_id),
        );
        if (!hasAnyLlmConfig) {
          applyPreset('openai');
        }
      });
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setLoading(false);
    }
  });

  const steps = createMemo(() => [
    { title: t('setupStepLlm') as string, description: t('setupStepLlmDesc') as string },
    { title: t('setupStepIm') as string, description: t('setupStepImDesc') as string },
    { title: t('setupStepSearch') as string, description: t('setupStepSearchDesc') as string },
    { title: t('setupStepDone') as string, description: t('setupStepDoneDesc') as string },
  ]);

  const remainingPlatforms = createMemo(() =>
    PLATFORM_ORDER.filter((id) => !selectedPlatforms().includes(id)),
  );

  const applyPreset = (key: Exclude<ProviderKey, 'compatible'>) => {
    const preset = PROVIDER_PRESETS[key];
    setLlmForm('llm_backend_type', preset.llm_backend_type);
    setLlmForm('llm_profile', preset.llm_profile);
    setLlmForm('llm_base_url', preset.llm_base_url);
    setLlmForm('llm_auth_type', preset.llm_auth_type);
    setLlmForm('llm_model', preset.llm_model);
  };

  const applyCompatibleKind = (kind: CompatibleKind) => {
    setCompatibleKind(kind);
    if (kind === 'claude') {
      setLlmForm('llm_backend_type', 'anthropic');
      setLlmForm('llm_profile', 'anthropic');
      setLlmForm('llm_auth_type', 'none');
    } else {
      setLlmForm('llm_backend_type', 'openai_compatible');
      setLlmForm('llm_profile', 'custom_openai_compatible');
      setLlmForm('llm_auth_type', 'bearer');
    }
  };

  const handleProviderChange = (key: ProviderKey) => {
    setProvider(key);
    if (key === 'compatible') {
      applyCompatibleKind(detectCompatibleKind(llmForm));
      return;
    }
    applyPreset(key);
  };

  const next = () => setStep((value) => Math.min(value + 1, steps().length - 1));
  const prev = () => setStep((value) => Math.max(value - 1, 0));

  const savePatch = async (patch: Partial<AppConfig>) => {
    setSaving(true);
    setError(null);
    try {
      await saveConfigPatch(patch);
      patchConfigLocal(patch);
      pushToast(t('setupSavedStep') as string, 'success');
      next();
    } catch (err) {
      const message = (err as Error).message;
      setError(message);
      pushToast(message, 'error', 5000);
    } finally {
      setSaving(false);
    }
  };

  const saveLlm = async () => {
    const requiredFields: Array<[string, string]> = [
      [llmForm.llm_api_key, t('llmApiKey') as string],
      [llmForm.llm_model, t('llmModel') as string],
      [llmForm.llm_base_url, t('llmBaseUrl') as string],
    ];

    if (provider() === 'compatible') {
      requiredFields.push([llmForm.llm_backend_type, t('llmBackend') as string]);
      requiredFields.push([llmForm.llm_profile, t('llmProfile') as string]);
    }

    const missing = requiredFields
      .filter(([value]) => !value.trim())
      .map(([, label]) => label);

    if (missing.length > 0) {
      const message = (t('llmValidationRequiredFields') as string).replace(
        '{fields}',
        missing.join(' / '),
      );
      setError(message);
      pushToast(message, 'error', 5000);
      return;
    }

    await savePatch({
      llm_api_key: llmForm.llm_api_key.trim(),
      llm_model: llmForm.llm_model.trim(),
      llm_timeout_ms: llmForm.llm_timeout_ms.trim(),
      llm_max_tokens: llmForm.llm_max_tokens.trim(),
      llm_backend_type: llmForm.llm_backend_type.trim(),
      llm_profile: llmForm.llm_profile.trim(),
      llm_base_url: llmForm.llm_base_url.trim(),
      llm_auth_type: llmForm.llm_auth_type.trim(),
    });
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

  const saveIm = async () => {
    const selected = new Set(selectedPlatforms());
    const message =
      (selected.has('wechat')
        ? (() => {
            if (!isWechatConfiguredLikeIm(imForm)) {
              return t('setupWechatLoginRequired') as string;
            }

            const token = imForm.wechat_token.trim();
            const baseUrl = imForm.wechat_base_url.trim();
            const cdnBaseUrl = imForm.wechat_cdn_base_url.trim();
            const accountId = imForm.wechat_account_id.trim();
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

            if (missing.length > 0) {
              return (t('imValidationIncompletePlatform') as string)
                .replace('{platform}', t('imWechatTitle') as string)
                .replace('{fields}', missing.join(' / '));
            }

            if (!isHttpUrl(baseUrl)) {
              return (t('imValidationInvalidField') as string)
                .replace('{platform}', t('imWechatTitle') as string)
                .replace('{field}', t('wechatBaseUrl') as string);
            }

            if (!isHttpUrl(cdnBaseUrl)) {
              return (t('imValidationInvalidField') as string)
                .replace('{platform}', t('imWechatTitle') as string)
                .replace('{field}', t('wechatCdnBaseUrl') as string);
            }

            return null;
          })()
        : null) ??
      (selected.has('qq')
        ? validatePlatform(t('imQqTitle') as string, [
            [imForm.qq_app_id, t('qqAppId') as string],
            [imForm.qq_app_secret, t('qqAppSecret') as string],
          ])
        : null) ??
      (selected.has('feishu')
        ? validatePlatform(t('imFeishuTitle') as string, [
            [imForm.feishu_app_id, t('feishuAppId') as string],
            [imForm.feishu_app_secret, t('feishuAppSecret') as string],
          ])
        : null) ??
      (selected.has('telegram')
        ? validatePlatform(t('imTelegramTitle') as string, [
            [imForm.tg_bot_token, t('tgBotToken') as string],
          ]) ??
          (!isTelegramToken(imForm.tg_bot_token.trim())
            ? (t('imValidationInvalidField') as string)
                .replace('{platform}', t('imTelegramTitle') as string)
                .replace('{field}', t('tgBotToken') as string)
            : null)
        : null);

    if (message) {
      setError(message);
      pushToast(message, 'error', 5000);
      return;
    }

    await savePatch({
      wechat_token: imForm.wechat_token,
      wechat_base_url: imForm.wechat_base_url.trim(),
      wechat_cdn_base_url: imForm.wechat_cdn_base_url.trim(),
      wechat_account_id: imForm.wechat_account_id.trim(),
      qq_app_id: imForm.qq_app_id.trim(),
      qq_app_secret: imForm.qq_app_secret,
      feishu_app_id: imForm.feishu_app_id.trim(),
      feishu_app_secret: imForm.feishu_app_secret,
      tg_bot_token: imForm.tg_bot_token.trim(),
    });
  };

  const saveSearch = async () => {
    await savePatch({
      search_tavily_key: searchForm.search_tavily_key.trim(),
    });
  };

  const addPlatform = (id: PlatformId) => {
    setSelectedPlatforms((prev) => [...prev, id]);
  };

  const clearPlatform = (id: PlatformId) => {
    setSelectedPlatforms((prev) => prev.filter((item) => item !== id));
    if (id === 'wechat') {
      setImForm('wechat_token', '');
      setImForm('wechat_base_url', '');
      setImForm('wechat_cdn_base_url', '');
      setImForm('wechat_account_id', '');
    } else if (id === 'feishu') {
      setImForm('feishu_app_id', '');
      setImForm('feishu_app_secret', '');
    } else if (id === 'qq') {
      setImForm('qq_app_id', '');
      setImForm('qq_app_secret', '');
    } else {
      setImForm('tg_bot_token', '');
    }
  };

  const onWechatLoginComplete = (data: WechatLoginStatus) => {
    batch(() => {
      if (data.token) setImForm('wechat_token', data.token);
      if (data.base_url) setImForm('wechat_base_url', data.base_url);
      if (data.account_id) setImForm('wechat_account_id', data.account_id);
      if (!imForm.wechat_cdn_base_url.trim()) {
        setImForm('wechat_cdn_base_url', WECHAT_DEFAULT_CDN_BASE_URL);
      }
    });
    setWechatAdvancedOpen(true);
    pushToast(t('imWechatCredsFilled') as string, 'info', 6000);
  };

  const renderPlatformCard = (id: PlatformId) => (
    <div class="rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-white/[0.02] overflow-hidden">
      {/* Card header */}
      <div class="flex items-center justify-between gap-3 px-5 py-3.5 border-b border-[var(--color-border-subtle)] bg-white/[0.015]">
        <h3 class="m-0 text-[0.9rem] font-semibold text-[var(--color-text-primary)]">
          {platformLabel(id)}
        </h3>
        <Button variant="danger-ghost" size="sm" onClick={() => clearPlatform(id)}>
          {t('setupImRemove')}
        </Button>
      </div>

      {/* Card body */}
      <div class="p-5">
        <Show when={id === 'wechat'}>
          <Banner kind="info" message={t('wechatLoginNote') as string} />
          <WechatWizardPanel
            configured={isWechatConfigured()}
            accountId={imForm.wechat_account_id.trim()}
            onConfigured={onWechatLoginComplete}
          />
          {/* Collapsible advanced credentials */}
          <div class="mt-4 rounded-[var(--radius-sm)] border border-[var(--color-border-subtle)] overflow-hidden">
            <button
              type="button"
              class="w-full flex items-center justify-between px-4 py-2.5 text-[0.8rem] font-medium text-[var(--color-text-secondary)] hover:bg-white/[0.04] transition text-left"
              onClick={() => setWechatAdvancedOpen((v) => !v)}
            >
              <span>{t('imAdvancedSettings')}</span>
              <svg
                xmlns="http://www.w3.org/2000/svg"
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                stroke-width="2"
                stroke-linecap="round"
                stroke-linejoin="round"
                class={`h-3.5 w-3.5 text-[var(--color-text-muted)] transition-transform duration-150 ${wechatAdvancedOpen() ? 'rotate-180' : ''}`}
                aria-hidden="true"
              >
                <polyline points="6 9 12 15 18 9" />
              </svg>
            </button>
            <Show when={wechatAdvancedOpen()}>
              <div class="border-t border-[var(--color-border-subtle)] px-4 pt-4 pb-5 grid gap-4 sm:grid-cols-2">
                <TextInput
                  type="password"
                  label={t('wechatToken')}
                  value={imForm.wechat_token}
                  onInput={(e) => setImForm('wechat_token', e.currentTarget.value)}
                />
                <TextInput
                  label={t('wechatAccountId')}
                  placeholder={t('wechatAccountIdPlaceholder') as string}
                  value={imForm.wechat_account_id}
                  onInput={(e) => setImForm('wechat_account_id', e.currentTarget.value)}
                />
                <TextInput
                  type="url"
                  label={t('wechatBaseUrl')}
                  placeholder={t('wechatBaseUrlPlaceholder') as string}
                  value={imForm.wechat_base_url}
                  onInput={(e) => setImForm('wechat_base_url', e.currentTarget.value)}
                />
                <TextInput
                  type="url"
                  label={t('wechatCdnBaseUrl')}
                  placeholder={t('wechatCdnBaseUrlPlaceholder') as string}
                  value={imForm.wechat_cdn_base_url}
                  onInput={(e) => setImForm('wechat_cdn_base_url', e.currentTarget.value)}
                />
              </div>
            </Show>
          </div>
        </Show>

        <Show when={id === 'feishu'}>
          <div class="grid gap-4 sm:grid-cols-2">
            <TextInput
              label={t('feishuAppId')}
              value={imForm.feishu_app_id}
              onInput={(event) => setImForm('feishu_app_id', event.currentTarget.value)}
            />
            <TextInput
              type="password"
              label={t('feishuAppSecret')}
              value={imForm.feishu_app_secret}
              onInput={(event) => setImForm('feishu_app_secret', event.currentTarget.value)}
            />
          </div>
        </Show>

        <Show when={id === 'qq'}>
          <div class="grid gap-4 sm:grid-cols-2">
            <TextInput
              label={t('qqAppId')}
              value={imForm.qq_app_id}
              onInput={(event) => setImForm('qq_app_id', event.currentTarget.value)}
            />
            <TextInput
              type="password"
              label={t('qqAppSecret')}
              value={imForm.qq_app_secret}
              onInput={(event) => setImForm('qq_app_secret', event.currentTarget.value)}
            />
          </div>
        </Show>

        <Show when={id === 'telegram'}>
          <div class="grid gap-4 sm:grid-cols-2">
            <TextInput
              type="password"
              label={t('tgBotToken')}
              value={imForm.tg_bot_token}
              onInput={(event) => setImForm('tg_bot_token', event.currentTarget.value)}
            />
          </div>
        </Show>
      </div>
    </div>
  );

  return (
    <div class="min-h-screen bg-[radial-gradient(circle_at_top,rgba(232,54,45,0.12),transparent_36%),linear-gradient(180deg,#16161b_0%,#0e1014_100%)]">
      <div class="max-w-4xl mx-auto px-4 py-8 sm:px-6 sm:py-10">
        <div class="rounded-[var(--radius-lg)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-card)] overflow-hidden shadow-[0_24px_80px_rgba(0,0,0,0.28)]">
          <div class="px-6 py-6 border-b border-[var(--color-border-subtle)]">
            <div class="flex items-start justify-between gap-4">
              <div>
                <p class="m-0 text-[0.78rem] uppercase tracking-[0.18em] text-[var(--color-accent-soft)]">
                  {t('setupEyebrow')}
                </p>
                <h1 class="m-0 mt-2 text-2xl font-semibold text-[var(--color-text-primary)]">
                  {t('setupTitle')}
                </h1>
                <p class="m-0 mt-2 text-[0.92rem] text-[var(--color-text-muted)] max-w-2xl">
                  {t('setupIntro')}
                </p>
              </div>
              <div class="shrink-0">
                <LanguageSwitcher />
              </div>
            </div>
            <div class="grid gap-2 sm:grid-cols-4 mt-5">
              <For each={steps()}>
                {(item, index) => (
                  <div
                    class={[
                      'rounded-[var(--radius-sm)] border px-3 py-3',
                      index() === step()
                        ? 'border-[var(--color-border-accent)] bg-[var(--color-accent)]/10'
                        : index() < step()
                          ? 'border-[rgba(104,211,145,0.22)] bg-[rgba(104,211,145,0.08)]'
                          : 'border-[var(--color-border-subtle)] bg-white/[0.02]',
                    ].join(' ')}
                  >
                    <div class="text-[0.74rem] text-[var(--color-text-muted)]">
                      {(t('setupStepLabel') as string).replace('{index}', String(index() + 1))}
                    </div>
                    <div class="mt-1 text-[0.88rem] font-medium text-[var(--color-text-primary)]">
                      {item.title}
                    </div>
                  </div>
                )}
              </For>
            </div>
          </div>

          <div class="p-6 sm:p-8">
            <Show when={loading()}>
              <Banner kind="info" message={t('statusLoading') as string} />
            </Show>
            <Show when={!loading() && error()}>
              <div class="mb-4">
                <Banner kind="error" message={error() ?? undefined} />
              </div>
            </Show>
            <Show when={!loading()}>
              <div class="mb-6">
                <h2 class="m-0 text-xl font-semibold text-[var(--color-text-primary)]">
                  {steps()[step()]?.title}
                </h2>
                <p class="m-0 mt-2 text-[0.9rem] text-[var(--color-text-muted)]">
                  {steps()[step()]?.description}
                </p>
              </div>

              <Switch>
                <Match when={step() === 0}>
                  <div class="grid gap-4 sm:grid-cols-2">
                    <SelectInput
                      label={t('llmProvider')}
                      value={provider()}
                      onChange={(event) => handleProviderChange(event.currentTarget.value as ProviderKey)}
                    >
                      <option value="openai">{t('llmProviderOpenai') as string}</option>
                      <option value="bailian">{t('setupLlmProviderBailian') as string}</option>
                      <option value="deepseek">{t('llmProviderDeepSeek') as string}</option>
                      <option value="anthropic">{t('setupLlmProviderClaude') as string}</option>
                      <option value="compatible">{t('setupLlmProviderCompatible') as string}</option>
                    </SelectInput>
                    <TextInput
                      label={t('llmModel')}
                      value={llmForm.llm_model}
                      onInput={(event) => setLlmForm('llm_model', event.currentTarget.value)}
                    />
                    <TextInput
                      type="password"
                      label={t('llmApiKey')}
                      value={llmForm.llm_api_key}
                      onInput={(event) => setLlmForm('llm_api_key', event.currentTarget.value)}
                    />

                    <Show when={provider() === 'compatible'}>
                      <>
                        <SelectInput
                          label={t('setupLlmCompatibleMode')}
                          value={compatibleKind()}
                          onChange={(event) =>
                            applyCompatibleKind(event.currentTarget.value as CompatibleKind)
                          }
                        >
                          <option value="openai">{t('setupLlmCompatibleOpenai') as string}</option>
                          <option value="claude">{t('setupLlmCompatibleClaude') as string}</option>
                        </SelectInput>
                        <TextInput
                          type="url"
                          label={t('llmBaseUrl')}
                          placeholder={t('llmBaseUrlPlaceholder') as string}
                          value={llmForm.llm_base_url}
                          onInput={(event) => setLlmForm('llm_base_url', event.currentTarget.value)}
                        />
                      </>
                    </Show>
                  </div>
                </Match>

                <Match when={step() === 1}>
                  <div class="flex flex-col gap-5">
                    {/* Platform picker — always visible */}
                    <div class="rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-white/[0.02] px-4 py-3">
                      <div class="flex flex-wrap items-center gap-3 min-h-[2rem]">
                        <p class="m-0 text-[0.82rem] text-[var(--color-text-muted)] flex-1 min-w-[12rem]">
                          {t('setupImPickerHint')}
                        </p>
                        <Show
                          when={remainingPlatforms().length > 0}
                          fallback={
                            <span class="text-[0.8rem] text-[var(--color-text-muted)] italic">
                              {t('setupImAdded')}
                            </span>
                          }
                        >
                          <div class="flex flex-wrap gap-2">
                            <For each={remainingPlatforms()}>
                              {(id) => (
                                <Button size="sm" variant="secondary" onClick={() => addPlatform(id)}>
                                  + {platformLabel(id)}
                                </Button>
                              )}
                            </For>
                          </div>
                        </Show>
                      </div>
                    </div>

                    {/* Empty state */}
                    <Show when={selectedPlatforms().length === 0}>
                      <div class="rounded-[var(--radius-lg)] border border-dashed border-[var(--color-border-subtle)] bg-white/[0.02] px-6 py-10 flex flex-col items-center justify-center text-center">
                        <p class="m-0 text-[var(--color-text-primary)] font-medium">
                          {t('setupImEmpty')}
                        </p>
                        <p class="m-0 mt-2 text-[0.86rem] text-[var(--color-text-muted)]">
                          {t('setupImPickerHint')}
                        </p>
                      </div>
                    </Show>

                    {/* Platform cards */}
                    <For each={selectedPlatforms()}>{(id) => renderPlatformCard(id)}</For>
                  </div>
                </Match>

                <Match when={step() === 2}>
                  <div class="grid gap-4 sm:grid-cols-2">
                    <TextInput
                      type="password"
                      label={t('searchTavilyKey')}
                      value={searchForm.search_tavily_key}
                      onInput={(event) => setSearchForm('search_tavily_key', event.currentTarget.value)}
                    />
                  </div>
                </Match>

                <Match when={step() === 3}>
                  <div class="space-y-4">
                    <Banner kind="success" message={t('setupRestartBanner') as string} />
                  </div>
                </Match>
              </Switch>
            </Show>
          </div>

          <div class="px-6 py-5 border-t border-[var(--color-border-subtle)] flex flex-wrap items-center justify-between gap-3">
            <div>
              <Show when={step() > 0 && step() < 3}>
                <Button variant="ghost" onClick={prev} disabled={saving()}>
                  {t('setupBack')}
                </Button>
              </Show>
            </div>
            <div class="flex flex-wrap items-center justify-end gap-2">
              <Show when={step() < 3}>
                <span class="text-[0.82rem] text-[var(--color-text-muted)]">{t('setupSkipHint')}</span>
              </Show>
              <Show when={step() < 3}>
                <Button variant="secondary" onClick={next} disabled={saving()}>
                  {t('setupSkip')}
                </Button>
              </Show>
              <Show when={step() === 0}>
                <Button variant="primary" onClick={() => void saveLlm()} disabled={saving()}>
                  {t('setupSaveAndNext')}
                </Button>
              </Show>
              <Show when={step() === 1}>
                <Button variant="primary" onClick={() => void saveIm()} disabled={saving()}>
                  {t('setupSaveAndNext')}
                </Button>
              </Show>
              <Show when={step() === 2}>
                <Button variant="primary" onClick={() => void saveSearch()} disabled={saving()}>
                  {t('setupFinish')}
                </Button>
              </Show>
              <Show when={step() === 3}>
                <Button variant="primary" onClick={() => props.onRestartRequest('webim')}>
                  {t('setupRestartNow')}
                </Button>
              </Show>
            </div>
          </div>
        </div>
      </div>
    </div>
  );
};
