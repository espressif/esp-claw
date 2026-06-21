const TAR_BLOCK = 512;
const TAR_MAX_DEPTH = 3;
const TAR_MAX_FILES = 50;

type BlobBytes = Uint8Array<ArrayBuffer>;

export type AppConfig = {
  wifi_ssid: string;
  wifi_password: string;
  ap_ssid: string;
  ap_password: string;
  ap_behavior: string;
  llm_api_key: string;
  llm_backend_type: string;
  llm_model: string;
  llm_base_url: string;
  llm_auth_type: string;
  llm_timeout_ms: string;
  llm_max_tokens: string;
  llm_default_image_max_bytes: string;
  llm_max_tokens_field: string;
  llm_supports_tools: string;
  llm_supports_vision: string;
  llm_image_remote_url_only: string;
  qq_app_id: string;
  qq_app_secret: string;
  qq_msg_type: string;
  feishu_app_id: string;
  feishu_app_secret: string;
  tg_bot_token: string;
  wechat_token: string;
  wechat_base_url: string;
  wechat_cdn_base_url: string;
  wechat_account_id: string;
  search_brave_key: string;
  search_tavily_key: string;
  search_http_allowlist: string;
  enabled_cap_groups: string;
  llm_visible_cap_groups: string;
  enabled_lua_modules: string;
  time_timezone: string;
};

/** Server-side configuration groups (must stay in sync with
 * CONFIG_FIELDS in http_server_config_api.c). */
export type ConfigGroup = 'wifi' | 'llm' | 'im' | 'search' | 'capabilities' | 'skills' | 'time';

export const GROUP_FIELDS: Record<ConfigGroup, (keyof AppConfig)[]> = {
  wifi: ['wifi_ssid', 'wifi_password', 'ap_ssid', 'ap_password', 'ap_behavior'],
  llm: [
    'llm_api_key',
    'llm_backend_type',
    'llm_model',
    'llm_base_url',
    'llm_auth_type',
    'llm_timeout_ms',
    'llm_max_tokens',
    'llm_default_image_max_bytes',
    'llm_max_tokens_field',
    'llm_supports_tools',
    'llm_supports_vision',
    'llm_image_remote_url_only',
  ],
  im: [
    'qq_app_id',
    'qq_app_secret',
    'qq_msg_type',
    'feishu_app_id',
    'feishu_app_secret',
    'tg_bot_token',
    'wechat_token',
    'wechat_base_url',
    'wechat_cdn_base_url',
    'wechat_account_id',
  ],
  search: ['search_brave_key', 'search_tavily_key', 'search_http_allowlist'],
  capabilities: ['enabled_cap_groups', 'llm_visible_cap_groups'],
  skills: ['enabled_lua_modules'],
  time: ['time_timezone'],
};

export function blankConfig(): Partial<AppConfig> {
  return {};
}

export type StatusInfo = {
  wifi_connected: boolean;
  ip: string;
  ap_active: boolean;
  ap_ssid: string;
  ap_ip: string;
  wifi_mode: string;
  storage_base_path: string;
};

export type CapabilityItem = {
  group_id: string;
  display_name: string;
  default_llm_visible: boolean;
};

export type LuaModuleItem = {
  module_id: string;
  display_name: string;
};

export type FileEntry = {
  name: string;
  path: string;
  is_dir: boolean;
  size: number;
};

export type WechatLoginStatus = {
  ok?: boolean;
  active?: boolean;
  completed?: boolean;
  configured?: boolean;
  session_key?: string;
  status?: string;
  message?: string;
  qr_data_url?: string;
  /** Returned only when `completed=true` – the actual WeChat token. The
   * frontend must display this for user review and save it explicitly. */
  token?: string;
  account_id?: string;
  user_id?: string;
  base_url?: string;
};

export type SetupWifiResponse = {
  ok?: boolean;
  wifi_connected?: boolean;
  ap_active?: boolean;
  ip?: string;
  ap_ip?: string;
  wifi_mode?: string;
  message?: string;
};

async function parseError(response: Response, fallback: string): Promise<Error> {
  let text = '';
  try {
    text = await response.text();
  } catch {
    /* ignore */
  }
  if (text) {
    try {
      const parsed = JSON.parse(text);
      if (parsed && typeof parsed === 'object' && typeof parsed.error === 'string') {
        return new Error(parsed.error);
      }
    } catch {
      /* not JSON, return plain text */
    }
    return new Error(text);
  }
  return new Error(fallback);
}

async function request<T>(
  url: string,
  init: RequestInit | undefined,
  fallbackError: string,
): Promise<T> {
  const response = await fetch(url, {
    cache: 'no-store',
    ...init,
  });
  if (!response.ok) {
    throw await parseError(response, fallbackError);
  }
  const contentType = response.headers.get('Content-Type') || '';
  if (contentType.includes('application/json')) {
    return (await response.json()) as T;
  }
  return (await response.text()) as unknown as T;
}

export function fetchStatus(signal?: AbortSignal) {
  return request<StatusInfo>('/api/status', { signal }, 'Failed to load status');
}

/** Fetch a subset of the configuration, filtered by group names. */
export function fetchConfigGroups(groups: ConfigGroup[] | 'all') {
  if (groups === 'all') {
    return request<Partial<AppConfig>>('/api/config', undefined, 'Failed to load config');
  }
  if (groups.length === 0) {
    return Promise.resolve({} as Partial<AppConfig>);
  }
  const qs = 'groups=' + encodeURIComponent(groups.join(','));
  return request<Partial<AppConfig>>('/api/config?' + qs, undefined, 'Failed to load config');
}

/** Fetch individual named fields (advanced). */
export function fetchConfigFields(fields: (keyof AppConfig)[]) {
  if (fields.length === 0) return Promise.resolve({} as Partial<AppConfig>);
  const qs = 'fields=' + encodeURIComponent(fields.join(','));
  return request<Partial<AppConfig>>('/api/config?' + qs, undefined, 'Failed to load config');
}

/** Partial save: only keys present in the patch are written; absent
 * keys remain untouched in NVS. */
export async function saveConfigPatch(patch: Partial<AppConfig>) {
  return request<{
    ok?: boolean;
    applied?: number;
    message?: string;
    error?: string;
  }>(
    '/api/config',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(patch),
    },
    'Failed to save config',
  );
}

export async function applySetupWifi(patch: Pick<AppConfig, 'wifi_ssid' | 'wifi_password'>) {
  return request<SetupWifiResponse>(
    '/api/setup/wifi',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(patch),
    },
    'Failed to apply Wi-Fi config',
  );
}

export async function fetchCapabilities() {
  const data = await request<{ items: CapabilityItem[] }>(
    '/api/capabilities',
    undefined,
    'Failed to load capabilities',
  );
  return Array.isArray(data.items) ? data.items : [];
}

export async function fetchLuaModules() {
  const data = await request<{ items: LuaModuleItem[] }>(
    '/api/lua-modules',
    undefined,
    'Failed to load Lua modules',
  );
  return Array.isArray(data.items) ? data.items : [];
}

export async function fetchFileList(path: string, signal?: AbortSignal) {
  const data = await request<{ path: string; entries: FileEntry[] }>(
    '/api/files?path=' + encodeURIComponent(path),
    { signal },
    'Failed to load file list',
  );
  return data;
}

export async function fetchFileContent(path: string, options: { allowMissing?: boolean } = {}) {
  const response = await fetch('/files' + path, { cache: 'no-store' });
  if (!response.ok) {
    if (options.allowMissing && response.status === 404) {
      return { content: '', missing: true };
    }
    throw await parseError(response, 'Failed to load file');
  }
  return { content: await response.text(), missing: false };
}

export async function saveFileContent(path: string, content: string | Blob) {
  const body =
    content instanceof Blob ? content : new Blob([content], { type: 'text/plain; charset=utf-8' });
  return request<unknown>(
    '/api/files/upload?path=' + encodeURIComponent(path),
    { method: 'POST', body },
    'Failed to save file',
  );
}

export async function uploadFile(path: string, file: File) {
  return request<unknown>(
    '/api/files/upload?path=' + encodeURIComponent(path),
    { method: 'POST', body: file },
    'Failed to upload file',
  );
}

export async function createFolder(path: string, options: { recursive?: boolean } = {}) {
  const body: { path: string; recursive?: boolean } = { path };
  if (options.recursive) {
    body.recursive = true;
  }
  return request<unknown>(
    '/api/files/mkdir',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    },
    'Failed to create folder',
  );
}

export async function deletePath(path: string, options: { recursive?: boolean } = {}) {
  let url = '/api/files?path=' + encodeURIComponent(path);
  if (options.recursive) {
    url += '&recursive=1';
  }
  return request<unknown>(url, { method: 'DELETE' }, 'Failed to delete path');
}

function makeBlobBytes(size: number): BlobBytes {
  return new Uint8Array(new ArrayBuffer(size));
}

function tarHeader(name: string, size: number): BlobBytes {
  const buf = makeBlobBytes(TAR_BLOCK);
  const enc = new TextEncoder();
  const write = (offset: number, len: number, val: string) => {
    const bytes = enc.encode(val);
    buf.set(bytes.subarray(0, Math.min(bytes.length, len)), offset);
  };
  write(0, 100, name);
  write(100, 8, '0000644\0');
  write(108, 8, '0000000\0');
  write(116, 8, '0000000\0');
  write(124, 12, size.toString(8).padStart(11, '0') + '\0');
  write(136, 12, '00000000000\0');
  buf[156] = 48; // '0' = regular file
  // checksum placeholder (spaces)
  for (let i = 148; i < 156; i++) buf[i] = 32;
  let chksum = 0;
  for (const byte of buf) chksum += byte;
  write(148, 7, chksum.toString(8).padStart(6, '0') + '\0');
  buf[155] = 32;
  return buf;
}

type CollectedFile = { path: string; relativeName: string; size: number };

async function collectFiles(
  basePath: string,
  prefix: string,
  depth: number,
  result: CollectedFile[],
  signal?: AbortSignal,
): Promise<void> {
  if (depth > TAR_MAX_DEPTH || result.length >= TAR_MAX_FILES) return;
  signal?.throwIfAborted();
  const data = await fetchFileList(basePath, signal);
  for (const entry of data.entries ?? []) {
    signal?.throwIfAborted();
    if (result.length >= TAR_MAX_FILES) break;
    const name = prefix ? prefix + '/' + entry.name : entry.name;
    if (entry.is_dir) {
      await collectFiles(entry.path, name, depth + 1, result, signal);
    } else {
      result.push({ path: entry.path, relativeName: name, size: entry.size });
    }
  }
}

export async function downloadFolderTar(
  path: string,
  onProgress?: (done: number, total: number) => void,
  signal?: AbortSignal,
): Promise<Blob> {
  const files: CollectedFile[] = [];
  await collectFiles(path, '', 1, files, signal);
  if (files.length === 0) {
    throw new Error('No files found in directory');
  }

  const total = files.length;
  const parts: BlobPart[] = [];

  for (const [i, f] of files.entries()) {
    signal?.throwIfAborted();
    const resp = await fetch('/files' + f.path, { cache: 'no-store', signal });
    if (!resp.ok) throw new Error(`Failed to download ${f.path}`);
    const content: BlobBytes = new Uint8Array(await resp.arrayBuffer());

    parts.push(tarHeader(f.relativeName, content.length));
    parts.push(content);
    const pad = (TAR_BLOCK - (content.length % TAR_BLOCK)) % TAR_BLOCK;
    if (pad > 0) parts.push(makeBlobBytes(pad));

    onProgress?.(i + 1, total);
  }

  parts.push(makeBlobBytes(TAR_BLOCK * 2));
  return new Blob(parts, { type: 'application/x-tar' });
}

export async function startWechatLogin(accountId: string, force = true) {
  return request<WechatLoginStatus>(
    '/api/wechat/login/start',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ account_id: accountId, force }),
    },
    'Failed to start WeChat login',
  );
}

export async function pollWechatLoginStatus() {
  return request<WechatLoginStatus>(
    '/api/wechat/login/status',
    undefined,
    'Failed to read WeChat login status',
  );
}

export async function cancelWechatLogin() {
  return request<WechatLoginStatus>(
    '/api/wechat/login/cancel',
    { method: 'POST' },
    'Failed to cancel WeChat login',
  );
}

export async function restartDevice() {
  return request<{ ok?: boolean; message?: string }>(
    '/api/restart',
    { method: 'POST' },
    'Failed to restart device',
  );
}

export type WebImLink = { url: string; label: string };
export type WebImMessage = {
  seq: number;
  role: string;
  text: string;
  ts_ms?: number;
  links?: WebImLink[];
};

export type WebImStatusResponse = {
  ok?: boolean;
  bound?: boolean;
};

export async function fetchWebimStatus() {
  return request<WebImStatusResponse>(
    '/api/webim/status',
    undefined,
    'Failed to read Web IM status',
  );
}

/** Browser WebSocket URL for live assistant events (`CONFIG_HTTPD_WS_SUPPORT` must be enabled). */
export function webimWebSocketUrl(): string {
  if (typeof window === 'undefined') {
    return 'ws://127.0.0.1/ws/webim';
  }
  const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${proto}//${window.location.host}/ws/webim`;
}

export async function sendWebimMessage(chatId: string, text: string, files: string[] = []) {
  return request<{ ok?: boolean }>(
    '/api/webim/send',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ chat_id: chatId, text, files }),
    },
    'Failed to send Web IM message',
  );
}
