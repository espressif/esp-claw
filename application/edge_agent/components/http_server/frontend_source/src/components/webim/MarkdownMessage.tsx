import { Show, type Component } from 'solid-js';

const MARKED_CDN_URL = 'https://esp-claw.com/clientjs/marked@18.0.4/marked.umd.min.js';
const DOMPURIFY_CDN_URL = 'https://esp-claw.com/clientjs/dompurify@3.4.5/purify.min.js';
const MARKED_CDN_INTEGRITY =
  'sha384-QIom/Ao3tGhg4C4VY5VTDrHMTPzgsih5cGuY30rd/xp6hWQ+xIGIZ4kxhaQQY+PB';
const DOMPURIFY_CDN_INTEGRITY =
  'sha384-7FXQySTrDscwsLx1i8RIqZM/JHoUVstx4CuL2b7tziI4Glhp3/3dm/j3qUTheVXE';

type MarkedRuntime = {
  parse: (text: string, options?: { async?: false }) => string | Promise<string>;
};

type DomPurifyRuntime = {
  sanitize: (html: string, config?: Record<string, unknown>) => string;
};

declare global {
  interface Window {
    marked?: MarkedRuntime;
    DOMPurify?: DomPurifyRuntime;
  }
}

const MARKDOWN_SANITIZE_CONFIG = {
  ALLOWED_TAGS: [
    'a',
    'blockquote',
    'br',
    'code',
    'em',
    'h1',
    'h2',
    'h3',
    'h4',
    'h5',
    'h6',
    'hr',
    'li',
    'ol',
    'p',
    'pre',
    's',
    'strong',
    'table',
    'tbody',
    'td',
    'th',
    'thead',
    'tr',
    'ul',
  ],
  ALLOWED_ATTR: ['href', 'title'],
  ALLOWED_URI_REGEXP: /^(?:(?:(?:https?|mailto|tel):|[#/]))/i,
};

let markdownRuntimePromise: Promise<void> | null = null;

function escapeMarkdownHtml(text: string): string {
  return text.replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

function loadExternalScript(
  src: string,
  integrity: string,
  testReady: () => boolean,
): Promise<void> {
  if (testReady()) {
    return Promise.resolve();
  }

  const existing = document.querySelector<HTMLScriptElement>(`script[data-webim-md="${src}"]`);
  if (existing) {
    if (existing.dataset.loaded === '1') {
      return testReady() ? Promise.resolve() : Promise.reject(new Error(src));
    }
    return new Promise((resolve, reject) => {
      existing.addEventListener('load', () => resolve(), { once: true });
      existing.addEventListener(
        'error',
        () => {
          existing.remove();
          reject(new Error(src));
        },
        { once: true },
      );
    });
  }

  return new Promise((resolve, reject) => {
    const script = document.createElement('script');
    script.src = src;
    script.async = true;
    script.crossOrigin = 'anonymous';
    script.integrity = integrity;
    script.dataset.webimMd = src;
    script.onload = () => {
      script.dataset.loaded = '1';
      testReady() ? resolve() : reject(new Error(src));
    };
    script.onerror = () => {
      script.remove();
      reject(new Error(src));
    };
    document.head.append(script);
  });
}

export async function loadMarkdownRuntime(): Promise<void> {
  if (window.marked?.parse && window.DOMPurify?.sanitize) {
    return;
  }

  markdownRuntimePromise ??= Promise.all([
    loadExternalScript(MARKED_CDN_URL, MARKED_CDN_INTEGRITY, () => !!window.marked?.parse),
    loadExternalScript(
      DOMPURIFY_CDN_URL,
      DOMPURIFY_CDN_INTEGRITY,
      () => !!window.DOMPurify?.sanitize,
    ),
  ]).then(() => undefined);

  try {
    await markdownRuntimePromise;
  } catch (error) {
    markdownRuntimePromise = null;
    throw error;
  }
}

function renderMarkdown(text: string): string {
  const html = window.marked?.parse(escapeMarkdownHtml(text), { async: false });
  if (typeof html !== 'string') {
    return '';
  }
  return window.DOMPurify?.sanitize(html, MARKDOWN_SANITIZE_CONFIG) ?? '';
}

export const MarkdownMessage: Component<{ preview: boolean; text: string }> = (props) => (
  <Show
    when={props.preview}
    fallback={<p class="m-0 whitespace-pre-wrap break-words">{props.text}</p>}
  >
    <div class="webim-markdown break-words" innerHTML={renderMarkdown(props.text)} />
  </Show>
);
