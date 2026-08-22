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
  kimi_global: {
    docsUrl: 'https://platform.kimi.ai/docs/overview',
    consoleUrl: 'https://platform.kimi.ai/console',
  },
  kimi_cn: {
    docsUrl: 'https://platform.kimi.com/docs/overview',
    consoleUrl: 'https://platform.kimi.com/console',
  },
  minimax_global: {
    docsUrl: 'https://platform.minimaxi.com/docs/api-reference/api-overview',
    consoleUrl: 'https://platform.minimax.io/login',
  },
  minimax_cn: {
    docsUrl: 'https://platform.minimaxi.com/docs/api-reference/api-overview',
    consoleUrl: 'https://platform.minimaxi.com/user-center/basic-information',
  },
};

export const TAVILY_API_KEY_URL = 'https://app.tavily.com/';
export const BRAVE_API_KEY_URL = 'https://api-dashboard.search.brave.com/app/keys';

export function getProviderLinks(key: string): ExternalLinkPair | undefined {
  return PROVIDER_LINKS[key];
}
