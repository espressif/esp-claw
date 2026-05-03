import { createEffect, createMemo, createSignal, Show, type Component } from 'solid-js';
import { t } from '../i18n';
import type { AppConfig } from '../api/client';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { CollapsibleConfigBlock, StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { SelectInput, TextInput } from '../components/ui/FormField';
import { SavePanel } from '../components/ui/SavePanel';
import { Banner } from '../components/ui/Banner';
import { pushToast } from '../state/toast';

type ProviderKey = 'openai' | 'qwen' | 'deepseek' | 'anthropic' | 'custom';

type ProviderPreset = {
  llm_backend_type: string;
  llm_profile: string;
  llm_base_url: string;
  llm_auth_type: string;
};

const PROVIDER_PRESETS: Record<Exclude<ProviderKey, 'custom'>, ProviderPreset> = {
  openai: {
    llm_backend_type: 'openai_compatible',
    llm_profile: 'openai',
    llm_base_url: 'https://api.openai.com/v1',
    llm_auth_type: 'bearer',
  },
  qwen: {
    llm_backend_type: 'openai_compatible',
    llm_profile: 'qwen_compatible',
    llm_base_url: 'https://dashscope.aliyuncs.com/compatible-mode/v1',
    llm_auth_type: 'bearer',
  },
  deepseek: {
    llm_backend_type: 'openai_compatible',
    llm_profile: 'custom_openai_compatible',
    llm_base_url: 'https://api.deepseek.com',
    llm_auth_type: 'bearer',
  },
  anthropic: {
    llm_backend_type: 'anthropic',
    llm_profile: 'anthropic',
    llm_base_url: 'https://api.anthropic.com/v1',
    llm_auth_type: 'none',
  },
};

const DEFAULT_MODELS: Record<Exclude<ProviderKey, 'custom'>, string> = {
  openai: 'gpt-5.4',
  qwen: 'qwen3.6-plus',
  deepseek: 'deepseek-v4-pro',
  anthropic: 'claude-sonnet-4-6',
};

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

function isPositiveInteger(value: string): boolean {
  return /^[1-9]\d*$/.test(value);
}

function detectPreset(form: LlmForm): ProviderKey {
  for (const key of Object.keys(PROVIDER_PRESETS) as Exclude<ProviderKey, 'custom'>[]) {
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
  return 'custom';
}

export const LlmPage: Component = () => {
  const tab = createConfigTab<LlmForm>({
    tab: 'llm',
    groups: ['llm'],
    toForm: (config: Partial<AppConfig>) => ({
      llm_api_key: config.llm_api_key ?? '',
      llm_model: config.llm_model ?? '',
      llm_timeout_ms: config.llm_timeout_ms ?? '',
      llm_max_tokens: config.llm_max_tokens ?? '',
      llm_backend_type: config.llm_backend_type ?? '',
      llm_profile: config.llm_profile ?? '',
      llm_base_url: config.llm_base_url ?? '',
      llm_auth_type: config.llm_auth_type ?? '',
    }),
    fromForm: (form) => ({
      llm_api_key: form.llm_api_key.trim(),
      llm_model: form.llm_model.trim(),
      llm_timeout_ms: form.llm_timeout_ms.trim(),
      llm_max_tokens: form.llm_max_tokens.trim(),
      llm_backend_type: form.llm_backend_type.trim(),
      llm_profile: form.llm_profile.trim(),
      llm_base_url: form.llm_base_url.trim(),
      llm_auth_type: form.llm_auth_type.trim(),
    }),
  });
  const [validationError, setValidationError] = createSignal<string | null>(null);

  const preset = createMemo(() => detectPreset(tab.form));

  createEffect(() => {
    void tab.form.llm_api_key;
    void tab.form.llm_model;
    void tab.form.llm_max_tokens;
    void tab.form.llm_backend_type;
    void tab.form.llm_profile;
    void tab.form.llm_base_url;
    void tab.form.llm_auth_type;
    setValidationError(null);
  });

  const applyPreset = (key: ProviderKey) => {
    if (key === 'custom') return;
    const preset = PROVIDER_PRESETS[key];
    tab.setForm('llm_backend_type', preset.llm_backend_type);
    tab.setForm('llm_profile', preset.llm_profile);
    tab.setForm('llm_base_url', preset.llm_base_url);
    tab.setForm('llm_auth_type', preset.llm_auth_type);
    tab.setForm('llm_model', DEFAULT_MODELS[key]);
  };

  const handleSave = async () => {
    const requiredFields: Array<[keyof LlmForm, string]> = [
      ['llm_api_key', t('llmApiKey') as string],
      ['llm_model', t('llmModel') as string],
      ['llm_max_tokens', t('llmMaxTokens') as string],
      ['llm_backend_type', t('llmBackend') as string],
      ['llm_profile', t('llmProfile') as string],
      ['llm_base_url', t('llmBaseUrl') as string],
      ['llm_auth_type', t('llmAuthType') as string],
    ];
    const missing = requiredFields
      .filter(([key]) => !tab.form[key].trim())
      .map(([, label]) => label);

    if (missing.length > 0) {
      const message = (t('llmValidationRequiredFields') as string).replace(
        '{fields}',
        missing.join(' / '),
      );
      setValidationError(message);
      pushToast(message, 'error', 5000);
      return;
    }

    if (!isPositiveInteger(tab.form.llm_max_tokens.trim())) {
      const message = t('llmValidationMaxTokens') as string;
      setValidationError(message);
      pushToast(message, 'error', 5000);
      return;
    }

    await tab.save();
  };

  return (
    <TabShell>
      <PageHeader title={t('navLlm') as string} description={t('sectionLlm') as string} />
      <Show when={validationError() ?? tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={validationError() ?? tab.error() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('sectionLlm') as string}>
          <div class="grid gap-3 sm:grid-cols-2 pt-2">
            <SelectInput
              label={t('llmProvider')}
              value={preset()}
              onChange={(event) => applyPreset(event.currentTarget.value as ProviderKey)}
            >
              <option value="openai">{t('llmProviderOpenai') as string}</option>
              <option value="qwen">{t('llmProviderQwen') as string}</option>
              <option value="deepseek">{t('llmProviderDeepSeek') as string}</option>
              <option value="anthropic">{t('llmProviderAnthropic') as string}</option>
              <option value="custom">{t('llmProviderCustom') as string}</option>
            </SelectInput>
            <TextInput
              type="password"
              label={t('llmApiKey')}
              value={tab.form.llm_api_key}
              onInput={(event) => tab.setForm('llm_api_key', event.currentTarget.value)}
            />
            <TextInput
              label={t('llmModel')}
              value={tab.form.llm_model}
              onInput={(event) => tab.setForm('llm_model', event.currentTarget.value)}
            />
            <TextInput
              label={t('llmTimeout')}
              placeholder={t('llmTimeoutPlaceholder') as string}
              value={tab.form.llm_timeout_ms}
              onInput={(event) => tab.setForm('llm_timeout_ms', event.currentTarget.value)}
            />
            <TextInput
              label={t('llmMaxTokens')}
              placeholder={t('llmMaxTokensPlaceholder') as string}
              value={tab.form.llm_max_tokens}
              onInput={(event) => tab.setForm('llm_max_tokens', event.currentTarget.value)}
            />
          </div>
        </StaticConfigBlock>
        <CollapsibleConfigBlock title={t('llmAdvanced') as string} defaultOpen={false}>
          <div class="grid gap-3 sm:grid-cols-2 pt-2">
            <TextInput
              label={t('llmBackend')}
              placeholder={t('llmBackendPlaceholder') as string}
              value={tab.form.llm_backend_type}
              onInput={(event) => tab.setForm('llm_backend_type', event.currentTarget.value)}
            />
            <TextInput
              label={t('llmProfile')}
              placeholder={t('llmProfilePlaceholder') as string}
              value={tab.form.llm_profile}
              onInput={(event) => tab.setForm('llm_profile', event.currentTarget.value)}
            />
            <TextInput
              type="url"
              label={t('llmBaseUrl')}
              placeholder={t('llmBaseUrlPlaceholder') as string}
              value={tab.form.llm_base_url}
              onInput={(event) => tab.setForm('llm_base_url', event.currentTarget.value)}
            />
            <TextInput
              label={t('llmAuthType')}
              placeholder={t('llmAuthTypePlaceholder') as string}
              value={tab.form.llm_auth_type}
              onInput={(event) => tab.setForm('llm_auth_type', event.currentTarget.value)}
            />
          </div>
        </CollapsibleConfigBlock>
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
