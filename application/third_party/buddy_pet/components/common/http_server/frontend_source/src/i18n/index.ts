import { createMemo, createSignal } from 'solid-js';
import * as i18n from '@solid-primitives/i18n';

import { en, type Dict } from './en';
import { zhCn } from './zh-cn';

export type Locale = 'en' | 'zh-cn';

const dictionaries: Record<Locale, Dict> = {
  en,
  'zh-cn': zhCn,
};

export const LOCALES: { id: Locale; label: string }[] = [
  { id: 'en', label: 'English' },
  { id: 'zh-cn', label: '简体中文' },
];

const STORAGE_KEY = 'esp-claw-lang';

function detectLocale(): Locale {
  try {
    const params = new URLSearchParams(window.location.search);
    const fromUrl = params.get('lang');
    if (fromUrl && fromUrl in dictionaries) return fromUrl as Locale;
  } catch {
    /* ignore */
  }

  try {
    const saved = localStorage.getItem(STORAGE_KEY);
    if (saved && saved in dictionaries) return saved as Locale;
  } catch {
    /* ignore */
  }

  const nav = (navigator.language || '').toLowerCase();
  if (nav.startsWith('zh')) return 'zh-cn';
  return 'en';
}

const [locale, setLocaleInternal] = createSignal<Locale>(detectLocale());

const dict = createMemo(() => i18n.flatten(dictionaries[locale()]));
export const t = i18n.translator(dict);

export function currentLocale() {
  return locale();
}

export function setLocale(next: Locale) {
  if (!(next in dictionaries)) return;
  setLocaleInternal(next);
  try {
    localStorage.setItem(STORAGE_KEY, next);
  } catch {
    /* ignore */
  }
  document.documentElement.lang = next === 'zh-cn' ? 'zh-CN' : 'en';
  document.title = dictionaries[next].docTitle;
}

export function tf(key: keyof Dict, vars: Record<string, string | number>) {
  let out = t(key) as string;
  for (const [k, v] of Object.entries(vars)) {
    out = out.replaceAll(`{${k}}`, String(v));
  }
  return out;
}

export type TKey = keyof Dict;
