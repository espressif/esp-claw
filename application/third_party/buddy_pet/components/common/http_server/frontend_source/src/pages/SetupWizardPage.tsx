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
  applySetupWifi,
  cancelWechatLogin,
  pollWechatLoginStatus,
  saveConfigPatch,
  startWechatLogin,
} from '../api/client';
import { LanguageSwitcher } from '../components/layout/LanguageSwitcher';
import { Banner } from '../components/ui/Banner';
import { Button } from '../components/ui/Button';
import { TextInput } from '../components/ui/FormField';
import { LabelLink } from '../components/ui/LabelLink';
import { getProviderLinks } from '../constants/externalLinks';
import { t } from '../i18n';
import { appConfig, ensureConfigGroups, patchConfigLocal } from '../state/config';
import { pushToast } from '../state/toast';

type TabId =
  | 'status'
  | 'basic'
  | 'llm'
  | 'im'
  | 'webreq'
  | 'memory'
  | 'webim'
  | 'capabilities'
  | 'skills'
  | 'files';

type SetupWizardPageProps = {
  onRestartRequest: (targetTab: TabId) => void;
};

type ProviderKey =
  | 'openai'
  | 'bailian'
  | 'deepseek'
  | 'anthropic'
  | 'openai_compatible'
  | 'anthropic_compatible';
type PlatformId = 'wechat' | 'feishu' | 'qq' | 'telegram';

type LlmForm = {
  llm_api_key: string;
  llm_model: string;
  llm_timeout_ms: string;
  llm_max_tokens: string;
  llm_backend_type: string;
  llm_base_url: string;
  llm_auth_type: string;
  llm_default_image_max_bytes: string;
  llm_max_tokens_field: string;
  llm_supports_tools: string;
  llm_supports_vision: string;
  llm_image_remote_url_only: string;
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
  search_http_allowlist: string;
};

type WifiForm = {
  wifi_ssid: string;
  wifi_password: string;
};

type ProviderPreset = {
  llm_backend_type: string;
  llm_base_url: string;
  llm_auth_type: string;
  llm_default_image_max_bytes: string;
  llm_max_tokens_field: string;
  llm_supports_tools: string;
  llm_supports_vision: string;
  llm_image_remote_url_only: string;
  llm_model: string;
};

const PROVIDER_PRESETS: Record<ProviderKey, ProviderPreset> = {
  openai: {
    llm_backend_type: 'openai_compatible',
    llm_base_url: 'https://api.openai.com/v1',
    llm_auth_type: 'bearer',
    llm_default_image_max_bytes: '524288',
    llm_max_tokens_field: 'max_completion_tokens',
    llm_supports_tools: 'true',
    llm_supports_vision: 'false',
    llm_image_remote_url_only: 'false',
    llm_model: 'gpt-5.4',
  },
  bailian: {
    llm_backend_type: 'openai_compatible',
    llm_base_url: 'https://dashscope.aliyuncs.com/compatible-mode/v1',
    llm_auth_type: 'bearer',
    llm_default_image_max_bytes: '524288',
    llm_max_tokens_field: 'max_tokens',
    llm_supports_tools: 'true',
    llm_supports_vision: 'false',
    llm_image_remote_url_only: 'false',
    llm_model: 'qwen3.6-plus',
  },
  deepseek: {
    llm_backend_type: 'openai_compatible',
    llm_base_url: 'https://api.deepseek.com',
    llm_auth_type: 'bearer',
    llm_default_image_max_bytes: '524288',
    llm_max_tokens_field: 'max_completion_tokens',
    llm_supports_tools: 'true',
    llm_supports_vision: 'false',
    llm_image_remote_url_only: 'false',
    llm_model: 'deepseek-v4-pro',
  },
  anthropic: {
    llm_backend_type: 'anthropic_compatible',
    llm_base_url: 'https://api.anthropic.com/v1',
    llm_auth_type: 'none',
    llm_default_image_max_bytes: '524288',
    llm_max_tokens_field: 'max_tokens',
    llm_supports_tools: 'true',
    llm_supports_vision: 'false',
    llm_image_remote_url_only: 'false',
    llm_model: 'claude-sonnet-4-6',
  },
  openai_compatible: {
    llm_backend_type: 'openai_compatible',
    llm_base_url: 'https://api.openai.com/v1',
    llm_auth_type: 'bearer',
    llm_default_image_max_bytes: '524288',
    llm_max_tokens_field: 'max_completion_tokens',
    llm_supports_tools: 'true',
    llm_supports_vision: 'false',
    llm_image_remote_url_only: 'false',
    llm_model: 'gpt-5.4',
  },
  anthropic_compatible: {
    llm_backend_type: 'anthropic_compatible',
    llm_base_url: 'https://api.anthropic.com/v1',
    llm_auth_type: 'none',
    llm_default_image_max_bytes: '524288',
    llm_max_tokens_field: 'max_tokens',
    llm_supports_tools: 'true',
    llm_supports_vision: 'false',
    llm_image_remote_url_only: 'false',
    llm_model: 'claude-sonnet-4-6',
  },
};

const PRESET_BUTTONS: ProviderKey[] = [
  'openai',
  'bailian',
  'deepseek',
  'anthropic',
  'openai_compatible',
  'anthropic_compatible',
];

const PLATFORM_ORDER: PlatformId[] = ['wechat', 'feishu', 'qq', 'telegram'];
const WECHAT_DEFAULT_CDN_BASE_URL = 'https://novac2c.cdn.weixin.qq.com/c2c';

function llmFromConfig(config: Partial<AppConfig>): LlmForm {
  return {
    llm_api_key: config.llm_api_key ?? '',
    llm_model: config.llm_model ?? '',
    llm_timeout_ms: config.llm_timeout_ms ?? '',
    llm_max_tokens: config.llm_max_tokens ?? '',
    llm_backend_type: config.llm_backend_type ?? '',
    llm_base_url: config.llm_base_url ?? '',
    llm_auth_type: config.llm_auth_type ?? '',
    llm_default_image_max_bytes: config.llm_default_image_max_bytes ?? '',
    llm_max_tokens_field: config.llm_max_tokens_field ?? '',
    llm_supports_tools: config.llm_supports_tools ?? 'false',
    llm_supports_vision: config.llm_supports_vision ?? 'false',
    llm_image_remote_url_only: config.llm_image_remote_url_only ?? 'false',
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
    search_http_allowlist: config.search_http_allowlist ?? '',
  };
}

function wifiFromConfig(config: Partial<AppConfig>): WifiForm {
  return {
    wifi_ssid: config.wifi_ssid ?? '',
    wifi_password: config.wifi_password ?? '',
  };
}

function detectProvider(form: LlmForm): ProviderKey {
  for (const key of ['openai', 'bailian', 'deepseek', 'anthropic'] as ProviderKey[]) {
    const preset = PROVIDER_PRESETS[key];
    if (
      preset.llm_backend_type === form.llm_backend_type.trim() &&
      preset.llm_base_url === form.llm_base_url.trim() &&
      preset.llm_auth_type === form.llm_auth_type.trim() &&
      preset.llm_max_tokens_field === form.llm_max_tokens_field.trim()
    ) {
      return key;
    }
  }
  if (
    form.llm_backend_type.trim() === 'anthropic_compatible' &&
    form.llm_base_url.trim() === PROVIDER_PRESETS.anthropic_compatible.llm_base_url &&
    form.llm_auth_type.trim() === PROVIDER_PRESETS.anthropic_compatible.llm_auth_type &&
    form.llm_max_tokens_field.trim() === PROVIDER_PRESETS.anthropic_compatible.llm_max_tokens_field
  ) {
    return 'anthropic_compatible';
  }
  return 'openai_compatible';
}

function isAdvancedProvider(key: ProviderKey): boolean {
  return key === 'openai_compatible' || key === 'anthropic_compatible';
}

function providerLabel(key: ProviderKey): string {
  switch (key) {
    case 'openai':
      return t('llmProviderOpenai') as string;
    case 'bailian':
      return t('setupLlmProviderBailian') as string;
    case 'deepseek':
      return t('llmProviderDeepSeek') as string;
    case 'anthropic':
      return t('llmProviderAnthropic') as string;
    case 'openai_compatible':
      return t('llmProviderOpenaiCompatible') as string;
    case 'anthropic_compatible':
      return t('llmProviderAnthropicCompatible') as string;
  }
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
    !token && !!baseUrl && !!cdnBaseUrl && (!accountId || normalizedAccountId === 'default');

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
  autoStart?: boolean;
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

  onMount(() => {
    if (props.autoStart && !props.configured) {
      void startLogin();
      return;
    }
    void poll();
  });

  onCleanup(stopPolling);

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
    return s ? (t('wechatLoginStatusPrefix') as string) + s : (t('wechatLoginStatus') as string);
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
  const [wifiForm, setWifiForm] = createStore<WifiForm>(wifiFromConfig({}));
  const [llmForm, setLlmForm] = createStore<LlmForm>(llmFromConfig({}));
  const [imForm, setImForm] = createStore<ImForm>(imFromConfig({}));
  const [searchForm, setSearchForm] = createStore<SearchForm>(searchFromConfig({}));
  const [provider, setProvider] = createSignal<ProviderKey>('openai');
  const [selectedPlatforms, setSelectedPlatforms] = createSignal<PlatformId[]>([]);
  const [wechatAdvancedOpen, setWechatAdvancedOpen] = createSignal(false);
  const isWechatConfigured = createMemo(() => isWechatConfiguredLikeIm(imForm));

  onMount(async () => {
    setLoading(true);
    setError(null);

    {
      const offsetMin = new Date().getTimezoneOffset();
      const absH = Math.floor(Math.abs(offsetMin) / 60);
      const absM = Math.abs(offsetMin) % 60;
      const sign = offsetMin >= 0 ? '' : '-';
      const tz = `UTC${sign}${absH}${absM ? ':' + String(absM).padStart(2, '0') : ''}`;
      saveConfigPatch({ time_timezone: tz })
        .then(() => patchConfigLocal({ time_timezone: tz }))
        .catch(() => {
          console.error('Failed to save time_timezone to config');
        });
    }

    try {
      await ensureConfigGroups(['wifi', 'llm', 'im', 'search']);
      batch(() => {
        const config = appConfig();
        setWifiForm(wifiFromConfig(config));
        const nextLlm = llmFromConfig(config);
        const nextIm = imFromConfig(config);
        const hasAnyLlmConfig = [
          nextLlm.llm_api_key,
          nextLlm.llm_model,
          nextLlm.llm_backend_type,
          nextLlm.llm_base_url,
        ].some((value) => value.trim().length > 0);
        setLlmForm(nextLlm);
        setImForm(nextIm);
        setSearchForm(searchFromConfig(config));
        setProvider(hasAnyLlmConfig ? detectProvider(nextLlm) : 'openai');
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
    { title: t('setupStepWifi') as string, description: t('setupStepWifiDesc') as string },
    { title: t('setupStepWechat') as string, description: t('setupStepWechatDesc') as string },
    { title: t('setupStepDone') as string, description: t('setupStepDoneDesc') as string },
  ]);

  const remainingPlatforms = createMemo(() =>
    PLATFORM_ORDER.filter((id) => !selectedPlatforms().includes(id)),
  );
  const providerLinks = createMemo(() => getProviderLinks(provider()));

  const applyPreset = (key: ProviderKey) => {
    const preset = PROVIDER_PRESETS[key];
    setProvider(key);
    setLlmForm('llm_backend_type', preset.llm_backend_type);
    setLlmForm('llm_base_url', preset.llm_base_url);
    setLlmForm('llm_auth_type', preset.llm_auth_type);
    setLlmForm('llm_default_image_max_bytes', preset.llm_default_image_max_bytes);
    setLlmForm('llm_max_tokens_field', preset.llm_max_tokens_field);
    setLlmForm('llm_supports_tools', preset.llm_supports_tools);
    setLlmForm('llm_supports_vision', preset.llm_supports_vision);
    setLlmForm('llm_image_remote_url_only', preset.llm_image_remote_url_only);
    setLlmForm('llm_model', preset.llm_model);
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

  const saveWifi = async () => {
    const ssid = wifiForm.wifi_ssid.trim();
    const password = wifiForm.wifi_password;
    if (!ssid) {
      const message = t('wifiValidationSsidRequired') as string;
      setError(message);
      pushToast(message, 'error', 5000);
      return;
    }
    if (password && password.length < 8) {
      const message = t('wifiValidationPasswordLength') as string;
      setError(message);
      pushToast(message, 'error', 5000);
      return;
    }

    setSaving(true);
    setError(null);
    try {
      await applySetupWifi({ wifi_ssid: ssid, wifi_password: password });
      patchConfigLocal({ wifi_ssid: ssid, wifi_password: password });
      pushToast(t('setupWifiConnecting') as string, 'success');
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

    if (isAdvancedProvider(provider())) {
      requiredFields.push([llmForm.llm_backend_type, t('llmBackend') as string]);
      requiredFields.push([llmForm.llm_max_tokens_field, t('llmMaxTokensField') as string]);
    }

    const missing = requiredFields.filter(([value]) => !value.trim()).map(([, label]) => label);

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
      llm_base_url: llmForm.llm_base_url.trim(),
      llm_auth_type: llmForm.llm_auth_type.trim(),
      llm_default_image_max_bytes: llmForm.llm_default_image_max_bytes.trim(),
      llm_max_tokens_field: llmForm.llm_max_tokens_field.trim(),
      llm_supports_tools: llmForm.llm_supports_tools,
      llm_supports_vision: 'false',
      llm_image_remote_url_only: llmForm.llm_image_remote_url_only,
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
        ? (validatePlatform(t('imTelegramTitle') as string, [
            [imForm.tg_bot_token, t('tgBotToken') as string],
          ]) ??
          (!isTelegramToken(imForm.tg_bot_token.trim())
            ? (t('imValidationInvalidField') as string)
                .replace('{platform}', t('imTelegramTitle') as string)
                .replace('{field}', t('tgBotToken') as string)
            : null))
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
      search_http_allowlist: searchForm.search_http_allowlist.trim(),
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
    const patch = {
      wechat_token: data.token ?? '',
      wechat_base_url: data.base_url ?? '',
      wechat_cdn_base_url: imForm.wechat_cdn_base_url.trim() || WECHAT_DEFAULT_CDN_BASE_URL,
      wechat_account_id: data.account_id ?? '',
    };
    batch(() => {
      setImForm('wechat_token', patch.wechat_token);
      setImForm('wechat_base_url', patch.wechat_base_url);
      setImForm('wechat_account_id', patch.wechat_account_id);
      setImForm('wechat_cdn_base_url', patch.wechat_cdn_base_url);
    });
    setWechatAdvancedOpen(true);
    pushToast(t('imWechatCredsFilled') as string, 'info', 6000);
    void savePatch(patch);
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
            <div class="grid gap-2 sm:grid-cols-3 mt-5">
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
                    <TextInput
                      label={t('wifiSsid')}
                      value={wifiForm.wifi_ssid}
                      onInput={(event) => setWifiForm('wifi_ssid', event.currentTarget.value)}
                    />
                    <TextInput
                      type="password"
                      label={t('wifiPassword')}
                      value={wifiForm.wifi_password}
                      onInput={(event) => setWifiForm('wifi_password', event.currentTarget.value)}
                    />
                    <div class="sm:col-span-2">
                      <Banner kind="info" message={t('setupWifiApplyHint') as string} />
                    </div>
                  </div>
                </Match>

                <Match when={step() === 1}>
                  <div class="rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-white/[0.02] p-5">
                    <Banner kind="info" message={t('wechatLoginNote') as string} />
                    <WechatWizardPanel
                      configured={false}
                      accountId={imForm.wechat_account_id.trim()}
                      autoStart
                      onConfigured={onWechatLoginComplete}
                    />
                  </div>
                </Match>

                <Match when={step() === 2}>
                  <div class="space-y-4">
                    <Banner kind="success" message={t('setupRestartBanner') as string} />
                  </div>
                </Match>
              </Switch>
            </Show>
          </div>

          <div class="px-6 py-5 border-t border-[var(--color-border-subtle)] flex flex-wrap items-center justify-between gap-3">
            <div>
              <Show when={step() > 0 && step() < 2}>
                <Button variant="ghost" onClick={prev} disabled={saving()}>
                  {t('setupBack')}
                </Button>
              </Show>
            </div>
            <div class="flex flex-wrap items-center justify-end gap-2">
              <Show when={step() === 0}>
                <Button variant="primary" onClick={() => void saveWifi()} disabled={saving()}>
                  {t('setupSaveAndNext')}
                </Button>
              </Show>
              <Show when={step() === 1}>
                <span class="text-[0.82rem] text-[var(--color-text-muted)]">
                  {saving() ? (t('setupSaving') as string) : (t('setupWechatAutoSaveHint') as string)}
                </span>
              </Show>
              <Show when={step() === 2}>
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
