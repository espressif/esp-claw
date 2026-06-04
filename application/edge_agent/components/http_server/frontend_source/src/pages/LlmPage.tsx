import { createEffect, createMemo, createSignal, Show, type Component } from 'solid-js';
import { t } from '../i18n';
import type { AppConfig } from '../api/client';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { CollapsibleConfigBlock, StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { TextInput } from '../components/ui/FormField';
import { SavePanel } from '../components/ui/SavePanel';
import { Banner } from '../components/ui/Banner';
import { Switch } from '../components/ui/Switch';
import { Button } from '../components/ui/Button';
import { LabelLink } from '../components/ui/LabelLink';
import { getProviderLinks } from '../constants/externalLinks';
import { pushToast } from '../state/toast';

type PresetKey =
  | 'openai'
  | 'bailian'
  | 'deepseek'
  | 'anthropic'
  | 'anthropic_compatible'
  | 'openai_compatible';

type ProviderPreset = {
  llm_backend_type: string;
  llm_base_url: string;
  llm_auth_type: string;
  llm_max_tokens_field: string;
  llm_default_image_max_bytes: string;
  llm_supports_tools: boolean;
  llm_supports_vision: boolean;
  llm_image_remote_url_only: boolean;
  llm_model: string;
  advanced: boolean;
};

const PROVIDER_PRESETS: Record<PresetKey, ProviderPreset> = {
  openai: {
    llm_backend_type: 'openai_compatible',
    llm_base_url: 'https://api.openai.com/v1',
    llm_auth_type: 'bearer',
    llm_max_tokens_field: 'max_completion_tokens',
    llm_default_image_max_bytes: '524288',
    llm_supports_tools: true,
    llm_supports_vision: true,
    llm_image_remote_url_only: false,
    llm_model: 'gpt-5.4',
    advanced: false,
  },
  bailian: {
    llm_backend_type: 'openai_compatible',
    llm_base_url: 'https://dashscope.aliyuncs.com/compatible-mode/v1',
    llm_auth_type: 'bearer',
    llm_max_tokens_field: 'max_tokens',
    llm_default_image_max_bytes: '524288',
    llm_supports_tools: true,
    llm_supports_vision: true,
    llm_image_remote_url_only: false,
    llm_model: 'qwen3.6-plus',
    advanced: false,
  },
  deepseek: {
    llm_backend_type: 'openai_compatible',
    llm_base_url: 'https://api.deepseek.com',
    llm_auth_type: 'bearer',
    llm_max_tokens_field: 'max_completion_tokens',
    llm_default_image_max_bytes: '524288',
    llm_supports_tools: true,
    llm_supports_vision: false,
    llm_image_remote_url_only: false,
    llm_model: 'deepseek-v4-pro',
    advanced: false,
  },
  anthropic: {
    llm_backend_type: 'anthropic_compatible',
    llm_base_url: 'https://api.anthropic.com/v1',
    llm_auth_type: 'none',
    llm_max_tokens_field: 'max_tokens',
    llm_default_image_max_bytes: '524288',
    llm_supports_tools: true,
    llm_supports_vision: true,
    llm_image_remote_url_only: false,
    llm_model: 'claude-sonnet-4-6',
    advanced: false,
  },
  openai_compatible: {
    llm_backend_type: 'openai_compatible',
    llm_base_url: 'https://api.openai.com/v1',
    llm_auth_type: 'bearer',
    llm_max_tokens_field: 'max_completion_tokens',
    llm_default_image_max_bytes: '524288',
    llm_supports_tools: true,
    llm_supports_vision: true,
    llm_image_remote_url_only: false,
    llm_model: 'gpt-5.4',
    advanced: true,
  },
  anthropic_compatible: {
    llm_backend_type: 'anthropic_compatible',
    llm_base_url: 'https://api.anthropic.com/v1',
    llm_auth_type: 'none',
    llm_max_tokens_field: 'max_tokens',
    llm_default_image_max_bytes: '524288',
    llm_supports_tools: true,
    llm_supports_vision: true,
    llm_image_remote_url_only: false,
    llm_model: 'claude-sonnet-4-6',
    advanced: true,
  },
};

const PRESET_BUTTONS: PresetKey[] = [
  'openai',
  'bailian',
  'deepseek',
  'anthropic',
  'anthropic_compatible',
  'openai_compatible',
];

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
  llm_supports_tools: boolean;
  llm_supports_vision: boolean;
  llm_image_remote_url_only: boolean;
};

function isPositiveInteger(value: string): boolean {
  return /^[1-9]\d*$/.test(value);
}

function parseBool(value: string | undefined): boolean {
  return value === 'true' || value === '1';
}

function presetLabel(key: PresetKey): string {
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
      llm_base_url: config.llm_base_url ?? '',
      llm_auth_type: config.llm_auth_type ?? '',
      llm_default_image_max_bytes: config.llm_default_image_max_bytes ?? '',
      llm_max_tokens_field: config.llm_max_tokens_field ?? '',
      llm_supports_tools: parseBool(config.llm_supports_tools),
      llm_supports_vision: parseBool(config.llm_supports_vision),
      llm_image_remote_url_only: parseBool(config.llm_image_remote_url_only),
    }),
    fromForm: (form) => ({
      llm_api_key: form.llm_api_key.trim(),
      llm_model: form.llm_model.trim(),
      llm_timeout_ms: form.llm_timeout_ms.trim(),
      llm_max_tokens: form.llm_max_tokens.trim(),
      llm_backend_type: form.llm_backend_type.trim(),
      llm_base_url: form.llm_base_url.trim(),
      llm_auth_type: form.llm_auth_type.trim(),
      llm_default_image_max_bytes: form.llm_default_image_max_bytes.trim(),
      llm_max_tokens_field: form.llm_max_tokens_field.trim(),
      llm_supports_tools: String(form.llm_supports_tools),
      llm_supports_vision: String(form.llm_supports_vision),
      llm_image_remote_url_only: String(form.llm_image_remote_url_only),
    }),
  });
  const [validationError, setValidationError] = createSignal<string | null>(null);
  const [advancedOpen, setAdvancedOpen] = createSignal(false);
  const [selectedPreset, setSelectedPreset] = createSignal<PresetKey | null>(null);
  const providerLinks = createMemo(() => {
    const key = selectedPreset();
    return key ? getProviderLinks(key) : undefined;
  });

  createEffect(() => {
    void tab.form.llm_api_key;
    void tab.form.llm_model;
    void tab.form.llm_max_tokens;
    void tab.form.llm_backend_type;
    void tab.form.llm_base_url;
    void tab.form.llm_auth_type;
    void tab.form.llm_default_image_max_bytes;
    void tab.form.llm_max_tokens_field;
    void tab.form.llm_supports_tools;
    void tab.form.llm_supports_vision;
    void tab.form.llm_image_remote_url_only;
    setValidationError(null);
  });

  const applyPreset = (key: PresetKey) => {
    const preset = PROVIDER_PRESETS[key];
    tab.setForm('llm_backend_type', preset.llm_backend_type);
    tab.setForm('llm_base_url', preset.llm_base_url);
    tab.setForm('llm_auth_type', preset.llm_auth_type);
    tab.setForm('llm_max_tokens_field', preset.llm_max_tokens_field);
    tab.setForm('llm_default_image_max_bytes', preset.llm_default_image_max_bytes);
    tab.setForm('llm_supports_tools', preset.llm_supports_tools);
    tab.setForm('llm_supports_vision', preset.llm_supports_vision);
    tab.setForm('llm_image_remote_url_only', preset.llm_image_remote_url_only);
    tab.setForm('llm_model', preset.llm_model);
    setSelectedPreset(key);
    setAdvancedOpen(preset.advanced);
  };

  const handleSave = async () => {
    const requiredFields: Array<[keyof LlmForm, string]> = [
      ['llm_api_key', t('llmApiKey') as string],
      ['llm_model', t('llmModel') as string],
      ['llm_max_tokens', t('llmMaxTokens') as string],
      ['llm_backend_type', t('llmBackend') as string],
      ['llm_base_url', t('llmBaseUrl') as string],
      ['llm_auth_type', t('llmAuthType') as string],
      ['llm_default_image_max_bytes', t('llmDefaultImageMaxBytes') as string],
      ['llm_max_tokens_field', t('llmMaxTokensField') as string],
    ];
    const missing = requiredFields
      .filter(([key]) => typeof tab.form[key] === 'string' && !(tab.form[key] as string).trim())
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

    if (!isPositiveInteger(tab.form.llm_default_image_max_bytes.trim())) {
      const message = t('llmValidationImageMaxBytes') as string;
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
          <div class="flex flex-col gap-3 pt-2">
            <div class="flex flex-col gap-2">
              <div class="text-[0.8rem] text-[var(--color-text-secondary)] font-medium">
                {t('llmFillDefaults') as string}
              </div>
              <div class="flex flex-wrap gap-2">
                {PRESET_BUTTONS.map((key) => (
                  <Button
                    size="sm"
                    variant="secondary"
                    active={selectedPreset() === key}
                    onClick={() => applyPreset(key)}
                  >
                    {presetLabel(key)}
                  </Button>
                ))}
              </div>
            </div>
            <div class="grid gap-3 sm:grid-cols-2">
              <TextInput
                type="password"
                label={
                  <>
                    {t('llmApiKey')}
                    <Show when={providerLinks()}>
                      {(links) => (
                        <LabelLink href={links().consoleUrl}>
                          {t('llmProviderConsole') as string} ↗
                        </LabelLink>
                      )}
                    </Show>
                  </>
                }
                value={tab.form.llm_api_key}
                onInput={(event) => tab.setForm('llm_api_key', event.currentTarget.value)}
              />
              <TextInput
                label={
                  <>
                    {t('llmModel')}
                    <Show when={providerLinks()}>
                      {(links) => (
                        <LabelLink href={links().docsUrl}>
                          {t('llmProviderDocs') as string} ↗
                        </LabelLink>
                      )}
                    </Show>
                  </>
                }
                value={tab.form.llm_model}
                onInput={(event) => tab.setForm('llm_model', event.currentTarget.value)}
              />
              <TextInput
                label={t('llmMaxTokens')}
                placeholder={t('llmMaxTokensPlaceholder') as string}
                value={tab.form.llm_max_tokens}
                onInput={(event) => tab.setForm('llm_max_tokens', event.currentTarget.value)}
              />
            </div>
          </div>
        </StaticConfigBlock>
        <CollapsibleConfigBlock
          title={t('llmAdvanced') as string}
          defaultOpen={false}
          open={advancedOpen()}
          onOpenChange={setAdvancedOpen}
        >
          <div class="grid gap-3 sm:grid-cols-2 pt-2">
            <TextInput
              label={t('llmBackend')}
              placeholder={t('llmBackendPlaceholder') as string}
              value={tab.form.llm_backend_type}
              onInput={(event) => tab.setForm('llm_backend_type', event.currentTarget.value)}
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
            <TextInput
              label={t('llmMaxTokensField')}
              placeholder={t('llmMaxTokensFieldPlaceholder') as string}
              value={tab.form.llm_max_tokens_field}
              onInput={(event) => tab.setForm('llm_max_tokens_field', event.currentTarget.value)}
            />
            <TextInput
              label={t('llmDefaultImageMaxBytes')}
              placeholder={t('llmDefaultImageMaxBytesPlaceholder') as string}
              value={tab.form.llm_default_image_max_bytes}
              onInput={(event) =>
                tab.setForm('llm_default_image_max_bytes', event.currentTarget.value)
              }
            />
            <TextInput
              label={t('llmTimeout')}
              placeholder={t('llmTimeoutPlaceholder') as string}
              value={tab.form.llm_timeout_ms}
              onInput={(event) => tab.setForm('llm_timeout_ms', event.currentTarget.value)}
            />
            <div class="flex items-start">
              <Switch
                checked={tab.form.llm_supports_tools}
                onChange={(checked) => tab.setForm('llm_supports_tools', checked)}
                label={t('llmSupportsTools') as string}
              />
            </div>
            <div class="flex items-start">
              <Switch
                checked={tab.form.llm_supports_vision}
                onChange={(checked) => tab.setForm('llm_supports_vision', checked)}
                label={t('llmSupportsVision') as string}
              />
            </div>
            <div class="flex items-start">
              <Switch
                checked={tab.form.llm_image_remote_url_only}
                onChange={(checked) => tab.setForm('llm_image_remote_url_only', checked)}
                label={t('llmImageRemoteUrlOnly') as string}
              />
            </div>
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
