/**
 * satteri-doc-links.ts
 *
 * A Sätteri MDAST plugin that rewrites `@lang/...` and `@base/...` path
 * aliases in Markdown and MDX content to absolute paths at compile time,
 * respecting Astro's `base` config and the locale inferred from the file path.
 *
 * Aliases
 * ───────
 *   @lang/<rest>  →  {base}/{locale}/<rest>
 *   @base/<rest>  →  {base}/<rest>
 *
 * `base` is taken from the plugin options, then the `ASTRO_BASE` env var,
 * then defaults to `/`. When base resolves to `/` it is treated as an empty
 * prefix so that the resulting URLs are `/en/foo` rather than `//en/foo`.
 *
 * `locale` is extracted from the file URL path segment immediately after
 * `content/docs/`, e.g. `.../content/docs/zh-cn/...` → `zh-cn`.
 *
 * Node types handled
 * ──────────────────
 * • `link`               – Markdown link:  [text](@lang/foo)
 * • `image`              – Markdown image: ![alt](@base/foo.png)
 * • `mdxJsxFlowElement`  – Block-level JSX component
 * • `mdxJsxTextElement`  – Inline JSX component
 *   For JSX nodes, every attribute whose value is a string literal matching
 *   the alias pattern is rewritten. Expression attributes are skipped.
 *
 * Usage in astro.config.mjs
 * ─────────────────────────
 *   import { satteri } from '@astrojs/markdown-satteri';
 *   import { satteriDocLinks } from './src/plugins/satteri-doc-links.ts';
 *
 *   export default defineConfig({
 *     markdown: {
 *       processor: satteri({
 *         mdastPlugins: [satteriDocLinks({ base: BASE })],
 *       }),
 *     },
 *   });
 */

import { defineMdastPlugin } from 'satteri';
import type {
  MdastPluginInput,
  MdxJsxFlowElement,
  MdxJsxTextElement,
  MdxJsxAttributeNode,
} from 'satteri';

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

export interface SatteriDocLinksOptions {
  /**
   * The Astro `base` path (e.g. `"/esp-claw/"`).
   * Pass `config.base` from `astro.config.mjs` directly so the plugin is
   * always in sync with the Astro config.
   * Falls back to the `ASTRO_BASE` env variable, then `/`.
   */
  base?: string;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Matches @lang/<rest> and @base/<rest> (rest is optional). */
const ALIAS_RE = /^@(lang|base)(\/.*)?$/;

/** Names of JSX attributes whose *string* values should be rewritten. */
const REWRITTEN_ATTRS = new Set(['href', 'src']);

/**
 * Extract the locale from the compile-time file URL.
 * Starlight stores content under `src/content/docs/<locale>/…`.
 * Falls back to `'en'` if the pattern is not found.
 */
function localeFromFileURL(fileURL: URL | undefined): string {
  if (!fileURL) return 'en';
  const pathname = fileURL.pathname.replace(/\\/g, '/');
  const match = pathname.match(/\/content\/docs\/([^/]+)\//);
  return match ? match[1] : 'en';
}

/**
 * Normalise the `base` string from Astro config:
 *  - Ensure it has a leading `/`
 *  - Strip any trailing `/`
 *  - When `base` is `/` (no sub-path), return `''` so that concatenation
 *    yields `/en/foo` not `//en/foo`
 */
function normaliseBase(raw: string): string {
  let b = raw.replace(/\/+$/, '');
  if (!b.startsWith('/')) b = '/' + b;
  return b === '/' ? '' : b;
}

/**
 * Resolve an `@lang/…` or `@base/…` alias to an absolute path.
 * Returns the original string unchanged if it does not match.
 */
function resolveAlias(url: string, locale: string, base: string): string {
  const m = url.match(ALIAS_RE);
  if (!m) return url;
  const kind = m[1];
  const rest = m[2] ?? '/';
  const suffix = rest.startsWith('/') ? rest : '/' + rest;
  return kind === 'lang'
    ? `${base}/${locale}${suffix}`
    : `${base}${suffix}`;
}

/**
 * Rewrite alias attributes on an MDX JSX element node.
 * Returns a new node with updated attributes if any changed, or undefined if
 * nothing changed. The caller returns this value from the visitor so Sätteri
 * uses the op-stream replace path (which supports MDX attributes) rather than
 * the simpler setProperty path (which does not).
 */
function rewriteJsxAttrs<T extends MdxJsxFlowElement | MdxJsxTextElement>(
  node: Readonly<T>,
  fileURL: URL | undefined,
  base: string,
): T | undefined {
  const locale = localeFromFileURL(fileURL);
  let changed = false;
  const newAttrs = node.attributes.map((attr) => {
    if (attr.type !== 'mdxJsxAttribute') return attr;
    const a = attr as MdxJsxAttributeNode;
    if (!REWRITTEN_ATTRS.has(a.name)) return attr;
    if (typeof a.value !== 'string') return attr;
    if (!ALIAS_RE.test(a.value)) return attr;
    changed = true;
    return { ...a, value: resolveAlias(a.value, locale, base) };
  });
  if (!changed) return undefined;
  return { ...node, attributes: newAttrs } as T;
}

// ---------------------------------------------------------------------------
// Plugin factory
// ---------------------------------------------------------------------------

/**
 * Returns a Sätteri `MdastPluginInput` that rewrites `@lang/` and `@base/`
 * URL aliases in Markdown links, images, and MDX JSX component attributes.
 *
 * Pass the result directly to `satteri({ mdastPlugins: [...] })`.
 */
export function satteriDocLinks(options: SatteriDocLinksOptions = {}): MdastPluginInput {
  const rawBase = options?.base ?? process.env.ASTRO_BASE ?? '/';
  const base = normaliseBase(rawBase);

  return defineMdastPlugin({
    name: 'satteri-doc-links',

    link(node, ctx) {
      if (ALIAS_RE.test(node.url)) {
        const locale = localeFromFileURL(ctx.fileURL);
        ctx.setProperty(node, 'url', resolveAlias(node.url, locale, base));
      }
    },

    image(node, ctx) {
      if (ALIAS_RE.test(node.url)) {
        const locale = localeFromFileURL(ctx.fileURL);
        ctx.setProperty(node, 'url', resolveAlias(node.url, locale, base));
      }
    },

    mdxJsxFlowElement(node, ctx) {
      return rewriteJsxAttrs(node, ctx.fileURL, base);
    },

    mdxJsxTextElement(node, ctx) {
      return rewriteJsxAttrs(node, ctx.fileURL, base);
    },
  });
}
