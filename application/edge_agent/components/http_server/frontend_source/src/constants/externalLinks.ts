export type ExternalLinkPair = {
  docsUrl: string;
  consoleUrl: string;
};

const PROVIDER_LINKS: Record<string, ExternalLinkPair> = {
  openai: {
    docsUrl: 'https://developers.openai.com/api/docs',
    consoleUrl: 'https://platform.openai.com/api-keys',
  },
  bailian: {
    docsUrl: 'https://help.aliyun.com/zh/model-studio/what-is-model-studio',
    consoleUrl: 'https://bailian.console.aliyun.com/?tab=model#/api-key',
  },
  deepseek: {
    docsUrl: 'https://api-docs.deepseek.com/',
    consoleUrl: 'https://platform.deepseek.com/api_keys',
  },
  anthropic: {
    docsUrl: 'https://platform.claude.com/docs/en/api/overview',
    consoleUrl: 'https://platform.claude.com/settings/keys',
  },
};

export const TAVILY_API_KEY_URL = 'https://app.tavily.com/';
export const BRAVE_API_KEY_URL = 'https://api-dashboard.search.brave.com/app/keys';

export function getProviderLinks(key: string): ExternalLinkPair | undefined {
  return PROVIDER_LINKS[key];
}
