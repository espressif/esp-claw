import { For, type Component } from 'solid-js';
import type { AppConfig } from '../../api/client';
import {
  BOCHA_API_KEY_URL,
  BRAVE_API_KEY_URL,
  TAVILY_API_KEY_URL,
} from '../../constants/externalLinks';
import { t } from '../../i18n';
import { SelectInput, TextInput } from '../ui/FormField';
import { LabelLink } from '../ui/LabelLink';

export type SearchConfig = Pick<
  AppConfig,
  'search_provider' | 'search_bocha_key' | 'search_tavily_key' | 'search_brave_key'
>;

const SEARCH_KEYS = [
  { field: 'search_bocha_key', label: 'webreqBochaKey', url: BOCHA_API_KEY_URL },
  { field: 'search_tavily_key', label: 'webreqTavilyKey', url: TAVILY_API_KEY_URL },
  { field: 'search_brave_key', label: 'webreqBraveKey', url: BRAVE_API_KEY_URL },
] as const;

export const SearchFields: Component<{
  form: SearchConfig;
  onChange: (field: keyof SearchConfig, value: string) => void;
}> = (props) => (
  <>
    <SelectInput
      full
      label={t('webreqSearchProvider')}
      value={props.form.search_provider}
      onChange={(event) => props.onChange('search_provider', event.currentTarget.value)}
    >
      <option value="bocha">Bocha</option>
      <option value="tavily">Tavily</option>
      <option value="brave">Brave Search</option>
    </SelectInput>
    <For each={SEARCH_KEYS.filter((key) => key.field === `search_${props.form.search_provider}_key`)}>
      {(key) => (
        <TextInput
          full
          type="password"
          maxLength={319}
          label={
            <>
              {t(key.label)}{' '}
              <LabelLink href={key.url}>{t('llmProviderConsole') as string} ↗</LabelLink>
            </>
          }
          value={props.form[key.field]}
          onInput={(event) => props.onChange(key.field, event.currentTarget.value)}
        />
      )}
    </For>
    <p class="sm:col-span-2 text-[0.78rem] text-[var(--color-text-muted)] m-0">
      {t('webreqSearchNote')}
    </p>
  </>
);
