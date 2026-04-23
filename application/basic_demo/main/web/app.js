/* ═══════════════════════════════════════════════════
   i18n – extensible locale registry
   To add a new language, append an entry to LOCALES
   and add a matching key in `strings`.
   ═══════════════════════════════════════════════════ */

const LOCALES = [
  { id: "en", label: "English" },
  { id: "zh-cn", label: "简体中文" },
];

const strings = {
  en: {
    docTitle: "ESP-Claw Settings",
    pageTitle: "Device Settings",
    pageSubtitle:
      "Configure credentials, runtime settings, and manage FATFS storage on your ESP device.",
    tabConfig: "Configuration",
    tabMemory: "Memory",
    tabFiles: "File Manager",
    tabChat: "Chat",

    chatPlaceholder: "Type a message...",
    chatSend: "Send",
    chatEmpty: "Start a conversation with ESP-Claw.",
    chatSessionLabel: "Session",
    chatNewSession: "New Session",
    chatDeleteSession: "Delete",
    chatClear: "Clear",
    chatPending: "Thinking...",
    chatError: "Failed to send message",
    chatBusy: "Please wait for the current reply to finish.",
    chatDeleteSessionConfirm: "Delete this session?",

    statusLoading: "Loading\u2026",
    statusOnline: "Wi-Fi Connected",
    statusOffline: "Wi-Fi Offline",

    sectionWifi: "Wi-Fi Settings",
    wifiSsid: "Wi-Fi SSID",
    wifiPassword: "Wi-Fi Password",

    sectionLlm: "LLM Settings",
    llmProvider: "LLM Provider",
    llmProviderOpenai: "OpenAI",
    llmProviderQwen: "Qwen Compatible",
    llmProviderAnthropic: "Anthropic",
    llmProviderCustom: "Custom",
    llmApiKey: "API Key",
    llmModel: "Model",
    llmTimeout: "Timeout (ms)",
    llmTimeoutPlaceholder: "120000",
    llmAdvanced: "LLM Advanced Options",
    llmBackend: "Backend Type",
    llmBackendPlaceholder: "openai_compatible / anthropic / custom",
    llmProfile: "Profile",
    llmProfilePlaceholder: "openai / qwen_compatible / anthropic",
    llmBaseUrl: "Base URL",
    llmBaseUrlPlaceholder: "https://api.openai.com/v1",
    llmAuthType: "Auth Type",
    llmAuthTypePlaceholder: "bearer / api-key / none",

    sectionIm: "Instant Messaging (IM)",
    imWechatTitle: "WeChat",
    imFeishuTitle: "Feishu",
    wechatLoginDesc: "QR Code Sign-in",
    qqAppId: "QQ App ID",
    qqAppSecret: "QQ App Secret",
    feishuAppId: "Feishu App ID",
    feishuAppSecret: "Feishu App Secret",
    tgBotToken: "Telegram Bot Token",
    wechatToken: "WeChat Token",
    wechatBaseUrl: "WeChat Base URL",
    wechatBaseUrlPlaceholder: "https://ilinkai.weixin.qq.com",
    wechatCdnBaseUrl: "WeChat CDN Base URL",
    wechatCdnBaseUrlPlaceholder: "https://novac2c.cdn.weixin.qq.com/c2c",
    wechatAccountId: "WeChat Account ID",
    wechatAccountIdPlaceholder: "default",
    wechatLogin: "WeChat Login",
    wechatLoginGenerate: "Generate QR",
    wechatLoginCancel: "Cancel",
    wechatLoginStatus: "Status: idle",
    wechatLoginNote:
      "Generate a QR code here to obtain wechat_token automatically. The token is saved to NVS after confirmation. Restart the device to apply the new login.",
    wechatLoginOpenLink: "Open login link",

    sectionSearch: "Search (Optional)",
    searchNote:
      "If set, ESP-Claw can search online. Tip: Built-in weather skill also relies on online search.",
    searchBraveKey: "Brave Search API Key",
    searchTavilyKey: "Tavily API Key",

    sectionAdvanced: "Advanced Settings",
    timezone: "Timezone",
    timezonePlaceholder: "e.g. CST-8 (Beijing) or UTC0",
    timezoneHelp:
      "POSIX TZ string only. The sign is INVERTED relative to common UTC notation: Beijing (UTC+8) must be written as 'CST-8'; New York (UTC-5) is 'EST5'. Names like 'Asia/Shanghai' will NOT work — please convert manually.",
    saveBtn: "Save Changes",
    saveSuccess: "Settings saved",
    saveError: "Failed to save settings",
    memoryTitle: "Memory Files",
    memoryDescription: "Manage the device memory files.",
    memoryCardLongTerm: "Long-term Memory",
    memoryCardSoul: "Soul",
    memoryCardIdentity: "Identity",
    memoryCardUserInfo: "User Info",
    memoryMissing: "Long-term memory file does not exist yet.",
    memoryRefreshAll: "Refresh All",
    memoryRefresh: "Refresh",
    memoryAuxMissing: "Memory file does not exist yet.",
    memoryRefreshed: "Memory reloaded",
    memoryLoadError: "Failed to load memory",
    memorySaveError: "Failed to save memory",
    configNote:
      "Changes are stored in NVS. Choose an LLM provider first, then fill the shared API key and model fields. Restart the device after updating Wi-Fi, Feishu, or core LLM settings.",

    fileRefresh: "Refresh",
    fileUpDir: "Up One Level",
    fileNewFolder: "New folder name",
    fileCreateFolder: "Create Folder",
    fileUploadPath: "/example.txt",
    fileChoose: "Choose File",
    fileNoFileSelected: "No file selected",
    fileUpload: "Upload",
    fileColName: "Name",
    fileColType: "Type",
    fileColSize: "Size",
    fileColActions: "Actions",
    fileTypeFolder: "Folder",
    fileTypeFile: "File",
    fileEmpty: "This folder is empty.",
    fileOpen: "Open",
    fileEdit: "Edit",
    filePreview: "Preview",
    fileDownload: "Download",
    fileDelete: "Delete",
    fileDevMode: "Dev Mode",
    fileDevModeRequired: "Enable Dev Mode to modify files.",
    fileDevModeConfirm:
      "After entering Dev Mode, you can upload, modify, or delete files. Incorrect changes may cause the system to fail, reboot endlessly, or behave unexpectedly. Please proceed carefully.",
    fileDeleteConfirm: "Delete {path}?",
    fileUploadComplete: "Upload completed",
    fileFolderCreated: "Folder created",
    fileDeleteComplete: "Delete completed",
    fileEditorTitle: "Edit File",
    fileEditorRefresh: "Refresh",
    fileEditorClose: "Close",
    fileEditorSave: "Save",
    fileEditorSaved: "File saved",
    fileEditorLoadError: "Failed to load file",
    fileEditorSaveError: "Failed to save file",
    fileSelectAndPath:
      "Select a file and provide a target path that starts with /.",
    fileFolderNameRequired: "Enter a folder name.",
  },

  "zh-cn": {
    docTitle: "ESP-Claw 设置",
    pageTitle: "设备设置",
    pageSubtitle:
      "配置凭据、运行时设置，并管理 ESP 设备上的 FATFS 存储。",
    tabConfig: "配置管理",
    tabMemory: "记忆管理",
    tabFiles: "文件管理",
    tabChat: "聊天",

    chatPlaceholder: "输入消息...",
    chatSend: "发送",
    chatEmpty: "与 ESP-Claw 开始对话。",
    chatSessionLabel: "会话",
    chatNewSession: "新会话",
    chatDeleteSession: "删除",
    chatClear: "清除",
    chatPending: "思考中...",
    chatError: "消息发送失败",
    chatBusy: "请等待当前回复完成。",
    chatDeleteSessionConfirm: "确定删除此会话？",

    statusLoading: "加载中\u2026",
    statusOnline: "Wi-Fi 已连接",
    statusOffline: "Wi-Fi 离线",

    sectionWifi: "Wi-Fi 设置",
    wifiSsid: "Wi-Fi 名称 (SSID)",
    wifiPassword: "Wi-Fi 密码",

    sectionLlm: "LLM 设置",
    llmProvider: "LLM 提供商",
    llmProviderOpenai: "OpenAI",
    llmProviderQwen: "Qwen 兼容",
    llmProviderAnthropic: "Anthropic",
    llmProviderCustom: "自定义",
    llmApiKey: "API 密钥",
    llmModel: "模型",
    llmTimeout: "超时 (毫秒)",
    llmTimeoutPlaceholder: "120000",
    llmAdvanced: "LLM 高级选项",
    llmBackend: "后端类型",
    llmBackendPlaceholder: "openai_compatible / anthropic / custom",
    llmProfile: "配置文件",
    llmProfilePlaceholder: "openai / qwen_compatible / anthropic",
    llmBaseUrl: "Base URL",
    llmBaseUrlPlaceholder: "https://api.openai.com/v1",
    llmAuthType: "认证类型",
    llmAuthTypePlaceholder: "bearer / api-key / none",

    sectionIm: "即时通讯 (IM)",
    imWechatTitle: "微信",
    imFeishuTitle: "飞书",
    wechatLoginDesc: "二维码登录",
    qqAppId: "QQ App ID",
    qqAppSecret: "QQ App Secret",
    feishuAppId: "飞书 App ID",
    feishuAppSecret: "飞书 App Secret",
    tgBotToken: "Telegram Bot Token",
    wechatToken: "微信 Token",
    wechatBaseUrl: "微信 Base URL",
    wechatBaseUrlPlaceholder: "https://ilinkai.weixin.qq.com",
    wechatCdnBaseUrl: "微信 CDN Base URL",
    wechatCdnBaseUrlPlaceholder: "https://novac2c.cdn.weixin.qq.com/c2c",
    wechatAccountId: "微信账号 ID",
    wechatAccountIdPlaceholder: "默认",
    wechatLogin: "微信登录",
    wechatLoginGenerate: "生成二维码",
    wechatLoginCancel: "取消",
    wechatLoginStatus: "状态：空闲",
    wechatLoginNote:
      "在此生成二维码以自动获取 wechat_token。确认后令牌将保存至 NVS。重启设备以应用新的登录。",
    wechatLoginOpenLink: "打开登录链接",

    sectionSearch: "搜索（可选）",
    searchNote:
      "如填写，ESP-Claw 可在运行中检索在线资源。提示：系统自带的天气 Skill 也依赖在线搜索功能。",
    searchBraveKey: "Brave Search API Key",
    searchTavilyKey: "Tavily API Key",

    sectionAdvanced: "高级设置",
    timezone: "时区",
    timezonePlaceholder: "例如 CST-8（北京时间）或 UTC0",
    timezoneHelp:
      "仅接受 POSIX TZ 字符串，符号与日常 UTC 表示相反。北京时间（UTC+8）应写作 “CST-8”，纽约（UTC-5）写作 “EST5”。不支持 “Asia/Shanghai” 这种 IANA 名称，请自行换算。",
    saveBtn: "保存更改",
    saveSuccess: "设置已保存",
    saveError: "保存设置失败",
    memoryTitle: "记忆文件",
    memoryDescription: "管理记忆文件。",
    memoryCardLongTerm: "长期记忆",
    memoryCardSoul: "灵魂",
    memoryCardIdentity: "身份",
    memoryCardUserInfo: "用户信息",
    memoryMissing: "长期记忆文件尚不存在。",
    memoryRefreshAll: "全部刷新",
    memoryRefresh: "刷新",
    memoryAuxMissing: "记忆文件尚不存在。",
    memoryRefreshed: "记忆已重新加载",
    memoryLoadError: "加载记忆失败",
    memorySaveError: "保存记忆失败",
    configNote:
      "更改存储在 NVS 中。请先选择 LLM 提供商，然后填写 API 密钥和模型字段。更新 Wi-Fi、飞书或核心 LLM 设置后请重启设备。",

    fileRefresh: "刷新",
    fileUpDir: "返回上级",
    fileNewFolder: "新文件夹名称",
    fileCreateFolder: "创建文件夹",
    fileUploadPath: "/example.txt",
    fileChoose: "选择文件",
    fileNoFileSelected: "未选择文件",
    fileUpload: "上传",
    fileColName: "名称",
    fileColType: "类型",
    fileColSize: "大小",
    fileColActions: "操作",
    fileTypeFolder: "文件夹",
    fileTypeFile: "文件",
    fileEmpty: "此文件夹为空。",
    fileOpen: "打开",
    fileEdit: "编辑",
    filePreview: "预览",
    fileDownload: "下载",
    fileDelete: "删除",
    fileDevMode: "开发者模式",
    fileDevModeRequired: "请先启用开发者模式再修改文件。",
    fileDevModeConfirm:
      "进入开发者模式后，你可以上传、修改或删除文件。不正确的增删改可能导致系统无法正常运行、无限重启等问题。请注意合理修改。",
    fileDeleteConfirm: "确定删除 {path}？",
    fileUploadComplete: "上传完成",
    fileFolderCreated: "文件夹已创建",
    fileDeleteComplete: "删除完成",
    fileEditorTitle: "编辑文件",
    fileEditorRefresh: "刷新",
    fileEditorClose: "退出",
    fileEditorSave: "保存",
    fileEditorSaved: "文件已保存",
    fileEditorLoadError: "加载文件失败",
    fileEditorSaveError: "保存文件失败",
    fileSelectAndPath: "请选择文件并提供以 / 开头的目标路径。",
    fileFolderNameRequired: "请输入文件夹名称。",
  },
};

/* ── Current language & device state ── */
let currentLang = "en";
let wifiConnected = null; // null = unknown/loading, true/false = fetched

function t(key) {
  const table = strings[currentLang] || strings["en"];
  return table[key] ?? (strings["en"][key] || key);
}

function detectLang() {
  const params = new URLSearchParams(window.location.search);
  const fromUrl = params.get("lang");
  if (fromUrl && strings[fromUrl]) return fromUrl;

  const saved = localStorage.getItem("esp-claw-lang");
  if (saved && strings[saved]) return saved;

  const nav = (navigator.language || "").toLowerCase();
  if (nav.startsWith("zh")) return "zh-cn";
  return "en";
}

function applyI18n() {
  document.querySelectorAll("[data-i18n]").forEach((el) => {
    el.textContent = t(el.dataset.i18n);
  });
  document.querySelectorAll("[data-i18n-placeholder]").forEach((el) => {
    el.placeholder = t(el.dataset.i18nPlaceholder);
  });
  document.querySelectorAll("[data-i18n-title]").forEach((el) => {
    document.title = t(el.dataset.i18nTitle);
  });
  document.querySelectorAll("[data-i18n-option]").forEach((el) => {
    el.textContent = t(el.dataset.i18nOption);
  });
  document.documentElement.lang = currentLang === "zh-cn" ? "zh-CN" : "en";
  document.title = t("docTitle");
  refreshStatusText();
}

function refreshStatusText() {
  const el = document.getElementById("status-text");
  if (wifiConnected === null) {
    el.textContent = t("statusLoading");
  } else {
    el.textContent = wifiConnected ? t("statusOnline") : t("statusOffline");
  }
}

function buildLocaleMenu() {
  const menu = document.getElementById("locale-menu");
  menu.innerHTML = "";
  LOCALES.forEach((loc) => {
    const li = document.createElement("li");
    const a = document.createElement("a");
    a.textContent = loc.label;
    a.href = "#";
    a.setAttribute("data-lang", loc.id);
    if (loc.id === currentLang) a.classList.add("is-current");
    a.addEventListener("click", (e) => {
      e.preventDefault();
      setLang(loc.id);
      document.getElementById("lang-switcher").removeAttribute("open");
    });
    li.appendChild(a);
    menu.appendChild(li);
  });
  const label = LOCALES.find((l) => l.id === currentLang);
  document.getElementById("current-locale-label").textContent =
    label ? label.label : currentLang;
}

function setLang(lang) {
  if (!strings[lang]) return;
  currentLang = lang;
  localStorage.setItem("esp-claw-lang", lang);
  applyI18n();
  buildLocaleMenu();
  renderFileRows(lastFileEntries);
  renderMemoryState();
  updateFileDevModeUI();
}

/* ═══════════════════════════════════════════════════
   Tab switching
   ═══════════════════════════════════════════════════ */

function initTabs() {
  document.querySelectorAll(".tab-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      document
        .querySelectorAll(".tab-btn")
        .forEach((b) => b.classList.remove("active"));
      document
        .querySelectorAll(".tab-panel")
        .forEach((p) => p.classList.remove("active"));
      btn.classList.add("active");
      const panel = document.getElementById("tab-" + btn.dataset.tab);
      if (panel) panel.classList.add("active");
      if (btn.dataset.tab === "memory") {
        loadMemory().catch((err) =>
          showBanner("memoryBanner", err.message || t("memoryLoadError"), true)
        );
      }
    });
  });
}

/* ═══════════════════════════════════════════════════
   Config – NVS fields
   ═══════════════════════════════════════════════════ */

const configFields = [
  "wifi_ssid",
  "wifi_password",
  "llm_api_key",
  "llm_backend_type",
  "llm_profile",
  "llm_model",
  "llm_base_url",
  "llm_auth_type",
  "llm_timeout_ms",
  "qq_app_id",
  "qq_app_secret",
  "feishu_app_id",
  "feishu_app_secret",
  "tg_bot_token",
  "wechat_token",
  "wechat_base_url",
  "wechat_cdn_base_url",
  "wechat_account_id",
  "search_brave_key",
  "search_tavily_key",
  "time_timezone",
];

const llmProviderPresets = {
  openai: {
    llm_backend_type: "openai_compatible",
    llm_profile: "openai",
    llm_base_url: "https://api.openai.com/v1",
    llm_auth_type: "bearer",
  },
  qwen: {
    llm_backend_type: "openai_compatible",
    llm_profile: "qwen_compatible",
    llm_base_url: "https://dashscope.aliyuncs.com/compatible-mode/v1",
    llm_auth_type: "bearer",
  },
  anthropic: {
    llm_backend_type: "anthropic",
    llm_profile: "anthropic",
    llm_base_url: "https://api.anthropic.com/v1",
    llm_auth_type: "none",
  },
};

const llmProviderDefaultModels = {
  openai: "gpt-5.4",
  qwen: "qwen3.6-plus",
  anthropic: "claude-sonnet-4-6",
};

/* ── Banner helpers ── */

function showBanner(id, message, isError = false) {
  const banner = document.getElementById(id);
  banner.textContent = message;
  banner.classList.remove("success", "error");
  banner.classList.add(isError ? "error" : "success");
  banner.classList.add("visible");
}

function hideBanner(id) {
  const banner = document.getElementById(id);
  banner.classList.remove("visible", "success", "error");
}

/* ── Config form read / fill ── */

function readConfigForm() {
  const payload = {};
  configFields.forEach((field) => {
    const input = document.getElementById(field);
    payload[field] = input ? input.value.trim() : "";
  });
  return payload;
}

function fillConfigForm(data) {
  configFields.forEach((field) => {
    const input = document.getElementById(field);
    if (input && typeof data[field] === "string") {
      input.value = data[field];
    }
  });
  syncProviderPreset();
}

/* ── LLM provider preset logic ── */

function detectProviderPreset() {
  const backend = document.getElementById("llm_backend_type")?.value.trim();
  const profile = document.getElementById("llm_profile")?.value.trim();
  const baseUrl = document.getElementById("llm_base_url")?.value.trim();
  const authType = document.getElementById("llm_auth_type")?.value.trim();

  const match = Object.entries(llmProviderPresets).find(
    ([, preset]) =>
      preset.llm_backend_type === backend &&
      preset.llm_profile === profile &&
      preset.llm_base_url === baseUrl &&
      preset.llm_auth_type === authType
  );
  return match ? match[0] : "custom";
}

function syncProviderPreset() {
  const select = document.getElementById("llm_provider_preset");
  if (select) select.value = detectProviderPreset();
}

function applyProviderPreset(presetKey) {
  const preset = llmProviderPresets[presetKey];
  if (!preset) {
    syncProviderPreset();
    return;
  }
  Object.entries(preset).forEach(([field, value]) => {
    const input = document.getElementById(field);
    if (input) input.value = value;
  });
  const modelInput = document.getElementById("llm_model");
  if (modelInput && llmProviderDefaultModels[presetKey]) {
    modelInput.value = llmProviderDefaultModels[presetKey];
  }
  syncProviderPreset();
}

/* ═══════════════════════════════════════════════════
   API calls
   ═══════════════════════════════════════════════════ */

async function loadStatus() {
  try {
    const response = await fetch("/api/status", { cache: "no-store" });
    const data = await response.json();
    const badge = document.getElementById("status-badge");
    const bar = document.getElementById("status-bar");

    wifiConnected = !!data.wifi_connected;
    if (wifiConnected) {
      badge.classList.remove("offline");
      badge.classList.add("online");
      bar.classList.add("is-online");
    } else {
      badge.classList.remove("online");
      badge.classList.add("offline");
      bar.classList.remove("is-online");
    }
    refreshStatusText();

    if (data.ip) {
      document.getElementById("info-ip").textContent = "IP: " + data.ip;
      document.getElementById("info-ip").style.display = "";
      document.getElementById("meta-sep-ip").style.display = "";
    }
    if (data.storage_base_path) {
      document.getElementById("info-storage").textContent =
        "Storage: " + data.storage_base_path;
      document.getElementById("info-storage").style.display = "";
      document.getElementById("meta-sep-storage").style.display = "";
    }
  } catch (err) {
    showBanner("configBanner", err.message || t("statusLoading"), true);
  }
}

async function loadConfig() {
  hideBanner("configBanner");
  const response = await fetch("/api/config", { cache: "no-store" });
  if (!response.ok) throw new Error("Failed to load settings");
  fillConfigForm(await response.json());
}

const WECHAT_FIELDS = [
  "wechat_token", "wechat_base_url", "wechat_cdn_base_url", "wechat_account_id",
];

async function reloadWechatFields() {
  const response = await fetch("/api/config", { cache: "no-store" });
  if (!response.ok) return;
  const data = await response.json();
  WECHAT_FIELDS.forEach((field) => {
    const input = document.getElementById(field);
    if (input && typeof data[field] === "string") {
      input.value = data[field];
    }
  });
}

async function saveConfig() {
  const button = document.getElementById("saveConfigButton");
  button.disabled = true;
  hideBanner("configBanner");

  try {
    const response = await fetch("/api/config", {
      method: "POST",
      cache: "no-store",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(readConfigForm()),
    });
    const responseText = await response.text();
    let result = {};
    if (responseText) {
      try {
        result = JSON.parse(responseText);
      } catch (parseError) {
        if (response.ok) throw parseError;
        result = { error: responseText };
      }
    }
    if (!response.ok)
      throw new Error(result.error || t("saveError"));
    showBanner("configBanner", result.message || t("saveSuccess"));
    syncProviderPreset();
  } catch (err) {
    showBanner("configBanner", err.message, true);
  } finally {
    button.disabled = false;
  }
}

/* ═══════════════════════════════════════════════════
   WeChat Login
   ═══════════════════════════════════════════════════ */

let wechatLoginPollTimer = null;

function stopWechatLoginPolling() {
  if (wechatLoginPollTimer) {
    clearTimeout(wechatLoginPollTimer);
    wechatLoginPollTimer = null;
  }
}

function renderWechatLoginStatus(data) {
  const qrCanvas = document.getElementById("wechatLoginQr");
  const qrLink = document.getElementById("wechatLoginQrLink");
  const meta = document.getElementById("wechatLoginMeta");

  if (data.qr_data_url) {
    if (typeof window.renderWechatQr === "function") {
      try {
        window.renderWechatQr(qrCanvas, data.qr_data_url);
      } catch (error) {
        console.error("Failed to render WeChat QR:", error);
      }
    }
    qrCanvas.classList.remove("hidden");
    qrLink.href = data.qr_data_url;
    qrLink.textContent = t("wechatLoginOpenLink");
    qrLink.classList.remove("hidden");
  } else {
    const ctx = qrCanvas.getContext("2d");
    if (ctx) {
      ctx.clearRect(0, 0, qrCanvas.width, qrCanvas.height);
    }
    qrCanvas.classList.add("hidden");
    qrLink.removeAttribute("href");
    qrLink.textContent = "";
    qrLink.classList.add("hidden");
  }

  meta.textContent = data.status
    ? (currentLang === "zh-cn" ? "状态：" : "Status: ") + data.status
    : t("wechatLoginStatus");

  if (data.message) {
    showBanner("wechatLoginBanner", data.message);
  } else {
    hideBanner("wechatLoginBanner");
  }

  if (data.completed && data.persisted) {
    reloadWechatFields().catch(() => {});
  }
}

async function pollWechatLoginStatus() {
  try {
    const response = await fetch("/api/wechat/login/status", {
      cache: "no-store",
    });
    const data = await response.json();
    if (!response.ok)
      throw new Error(data.error || "Failed to fetch WeChat login status");
    renderWechatLoginStatus(data);
    if (data.active || (data.completed && !data.persisted)) {
      wechatLoginPollTimer = setTimeout(pollWechatLoginStatus, 1500);
    } else {
      stopWechatLoginPolling();
    }
  } catch (err) {
    showBanner("wechatLoginBanner", err.message, true);
    stopWechatLoginPolling();
  }
}

async function startWechatLogin() {
  const button = document.getElementById("wechatLoginStartButton");
  button.disabled = true;
  hideBanner("wechatLoginBanner");

  try {
    const response = await fetch("/api/wechat/login/start", {
      method: "POST",
      cache: "no-store",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        account_id: document.getElementById("wechat_account_id").value.trim(),
        force: true,
      }),
    });
    const data = await response.json();
    if (!response.ok)
      throw new Error(data.error || "Failed to start WeChat login");
    renderWechatLoginStatus(data);
    stopWechatLoginPolling();
    wechatLoginPollTimer = setTimeout(pollWechatLoginStatus, 1000);
  } catch (err) {
    showBanner("wechatLoginBanner", err.message, true);
  } finally {
    button.disabled = false;
  }
}

async function cancelWechatLogin() {
  try {
    const response = await fetch("/api/wechat/login/cancel", {
      method: "POST",
      cache: "no-store",
    });
    const data = await response.json();
    if (!response.ok)
      throw new Error(data.error || "Failed to cancel WeChat login");
    renderWechatLoginStatus({
      status: currentLang === "zh-cn" ? "已取消" : "cancelled",
      message:
        data.message ||
        (currentLang === "zh-cn" ? "已取消微信登录。" : "WeChat login cancelled."),
      qr_data_url: "",
      completed: false,
      persisted: false,
    });
    stopWechatLoginPolling();
  } catch (err) {
    showBanner("wechatLoginBanner", err.message, true);
  }
}

/* ═══════════════════════════════════════════════════
   File Manager
   ═══════════════════════════════════════════════════ */

let currentPath = "/";
let lastFileEntries = [];
let fileDevModeEnabled = false;
const EDITABLE_FILE_EXTENSIONS = [".md", ".json", ".txt", ".jsonb", ".jsonc", ".lua"];
const MEMORY_FILES = [
  {
    id: "memoryEditor",
    missingId: "memoryMissingState",
    path: "/memory/memory.md",
    editable: false,
  },
  { id: "soulEditor", missingId: "soulMissingState", path: "/memory/soul.md", editable: true },
  { id: "identityEditor", missingId: "identityMissingState", path: "/memory/identity.md", editable: true },
  { id: "userEditor", missingId: "userMissingState", path: "/memory/user.md", editable: true },
];
let memoryLoaded = false;
let memoryContents = {};
let memoryFilePresence = {};
let activeEditorPath = "";
let activeEditorSource = "";

function isFileDevModeEnabled() {
  return fileDevModeEnabled;
}

function applyFileEditorAccessMode() {
  const textarea = document.getElementById("fileEditorTextarea");
  const saveButton = document.getElementById("saveFileEditorButton");
  if (!textarea || !saveButton) return;

  const previewOnly = activeEditorSource === "files" && !isFileDevModeEnabled();
  textarea.readOnly = previewOnly;
  saveButton.classList.toggle("hidden", previewOnly);
  saveButton.disabled = previewOnly;
}

function updateFileDevModeUI() {
  const createRow = document.getElementById("fileCreateRow");
  const uploadRow = document.getElementById("fileUploadRow");
  const toggleButton = document.getElementById("devModeButton");
  const enabled = isFileDevModeEnabled();

  if (createRow) {
    createRow.classList.toggle("hidden", !enabled);
  }
  if (uploadRow) {
    uploadRow.classList.toggle("hidden", !enabled);
  }
  if (toggleButton) {
    toggleButton.classList.toggle("active", enabled);
    toggleButton.setAttribute("aria-pressed", enabled ? "true" : "false");
  }

  applyFileEditorAccessMode();
  renderFileRows(lastFileEntries);
}

function setFileDevMode(enabled) {
  fileDevModeEnabled = !!enabled;
  updateFileDevModeUI();
}

function humanSize(value) {
  if (value < 1024) return value + " B";
  if (value < 1024 * 1024) return (value / 1024).toFixed(1) + " KB";
  return (value / (1024 * 1024)).toFixed(1) + " MB";
}

function parentPath(path) {
  if (path === "/") return "/";
  const parts = path.split("/").filter(Boolean);
  parts.pop();
  return parts.length ? "/" + parts.join("/") : "/";
}

function joinPath(base, name) {
  return base === "/" ? "/" + name : base + "/" + name;
}

function isEditableFile(path) {
  const lowerPath = (path || "").toLowerCase();
  return EDITABLE_FILE_EXTENSIONS.some((ext) => lowerPath.endsWith(ext));
}

function createActionSlot(node = null, placeholderText = "") {
  const slot = document.createElement("span");
  slot.className = "action-slot";
  if (node) {
    slot.appendChild(node);
  } else {
    slot.classList.add("placeholder");
    slot.textContent = placeholderText;
  }
  return slot;
}

function renderFileRows(entries) {
  lastFileEntries = entries;
  const tbody = document.getElementById("fileTableBody");
  tbody.innerHTML = "";

  if (!entries.length) {
    const row = document.createElement("tr");
    const td = document.createElement("td");
    td.setAttribute("colspan", "4");
    td.textContent = t("fileEmpty");
    td.style.color = "var(--text-muted)";
    row.appendChild(td);
    tbody.appendChild(row);
    return;
  }

  entries
    .sort(
      (a, b) =>
        Number(b.is_dir) - Number(a.is_dir) || a.name.localeCompare(b.name)
    )
    .forEach((entry) => {
      const row = document.createElement("tr");

      const tdName = document.createElement("td");
      tdName.textContent = entry.name;
      row.appendChild(tdName);

      const tdType = document.createElement("td");
      tdType.textContent = entry.is_dir ? t("fileTypeFolder") : t("fileTypeFile");
      row.appendChild(tdType);

      const tdSize = document.createElement("td");
      tdSize.textContent = entry.is_dir ? "-" : humanSize(entry.size || 0);
      row.appendChild(tdSize);

      const tdActions = document.createElement("td");
      tdActions.className = "actions";
      let editControl = null;
      let middleControl = null;

      if (entry.is_dir) {
        const openBtn = document.createElement("button");
        openBtn.className = "link-btn";
        openBtn.textContent = t("fileOpen");
        openBtn.onclick = () => {
          currentPath = entry.path;
          loadFiles().catch((err) =>
            showBanner("fileBanner", err.message, true)
          );
        };
        middleControl = openBtn;
      } else {
        if (isEditableFile(entry.path)) {
          const editBtn = document.createElement("button");
          editBtn.className = "link-btn";
          editBtn.textContent = isFileDevModeEnabled() ? t("fileEdit") : t("filePreview");
          editBtn.onclick = () => {
            openFileEditor(entry.path).catch((err) =>
              showBanner("fileBanner", err.message || t("fileEditorLoadError"), true)
            );
          };
          editControl = editBtn;
        }

        const dlLink = document.createElement("a");
        dlLink.href = "/files" + entry.path;
        dlLink.textContent = t("fileDownload");
        dlLink.className = "link-btn";
        dlLink.target = "_blank";
        middleControl = dlLink;
      }

      const delBtn = document.createElement("button");
      delBtn.className = "link-btn danger";
      delBtn.textContent = t("fileDelete");
      delBtn.disabled = !isFileDevModeEnabled();
      delBtn.onclick = async () => {
        if (!isFileDevModeEnabled()) return;
        const msg = t("fileDeleteConfirm").replace("{path}", entry.path);
        if (!window.confirm(msg)) return;
        await deletePath(entry.path);
      };
      const editPlaceholder = entry.is_dir ? t("fileTypeFolder") : t("fileEdit");
      tdActions.appendChild(createActionSlot(editControl, editPlaceholder));
      tdActions.appendChild(createActionSlot(middleControl));
      tdActions.appendChild(createActionSlot(delBtn));

      row.appendChild(tdActions);
      tbody.appendChild(row);
    });
}

async function loadFiles() {
  hideBanner("fileBanner");
  document.getElementById("currentPath").textContent = currentPath;

  const response = await fetch(
    "/api/files?path=" + encodeURIComponent(currentPath),
    { cache: "no-store" }
  );
  if (!response.ok) throw new Error((await response.text()) || "Failed to load file list");

  const data = await response.json();
  currentPath = data.path || "/";
  document.getElementById("currentPath").textContent = currentPath;
  renderFileRows(data.entries || []);
}

async function uploadFile() {
  if (!isFileDevModeEnabled()) {
    showBanner("fileBanner", t("fileDevModeRequired"), true);
    return;
  }

  const pathInput = document.getElementById("uploadPathInput");
  const fileInput = document.getElementById("uploadFileInput");
  const button = document.getElementById("uploadButton");
  const file = fileInput.files[0];
  const relativePath =
    pathInput.value.trim() || (file ? joinPath(currentPath, file.name) : "");

  if (!file || !relativePath.startsWith("/")) {
    showBanner("fileBanner", t("fileSelectAndPath"), true);
    return;
  }

  button.disabled = true;
  hideBanner("fileBanner");
  try {
    const response = await fetch(
      "/api/files/upload?path=" + encodeURIComponent(relativePath),
      { method: "POST", cache: "no-store", body: file }
    );
    if (!response.ok) throw new Error(await response.text());
    pathInput.value = "";
    fileInput.value = "";
    document.getElementById("selectedFileName").textContent =
      t("fileNoFileSelected");
    showBanner("fileBanner", t("fileUploadComplete"));
    await loadFiles();
  } catch (err) {
    showBanner("fileBanner", err.message || "Upload failed", true);
  } finally {
    button.disabled = false;
  }
}

async function createFolder() {
  if (!isFileDevModeEnabled()) {
    showBanner("fileBanner", t("fileDevModeRequired"), true);
    return;
  }

  const input = document.getElementById("newFolderInput");
  const name = input.value.trim();
  if (!name) {
    showBanner("fileBanner", t("fileFolderNameRequired"), true);
    return;
  }

  try {
    const response = await fetch("/api/files/mkdir", {
      method: "POST",
      cache: "no-store",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ path: joinPath(currentPath, name) }),
    });
    if (!response.ok) throw new Error(await response.text());
    input.value = "";
    showBanner("fileBanner", t("fileFolderCreated"));
    await loadFiles();
  } catch (err) {
    showBanner("fileBanner", err.message || "Failed to create folder", true);
  }
}

async function deletePath(path) {
  if (!isFileDevModeEnabled()) {
    showBanner("fileBanner", t("fileDevModeRequired"), true);
    return;
  }

  try {
    const response = await fetch(
      "/api/files?path=" + encodeURIComponent(path),
      { method: "DELETE", cache: "no-store" }
    );
    if (!response.ok) throw new Error(await response.text());
    showBanner("fileBanner", t("fileDeleteComplete"));
    await loadFiles();
  } catch (err) {
    showBanner("fileBanner", err.message || "Delete failed", true);
  }
}

/* ═══════════════════════════════════════════════════
   File Editor
   ═══════════════════════════════════════════════════ */

function openFileEditorModal() {
  document.getElementById("fileEditorModal").classList.remove("hidden");
  document.body.style.overflow = "hidden";
}

function closeFileEditorModal() {
  document.getElementById("fileEditorModal").classList.add("hidden");
  document.body.style.overflow = "";
  const shouldRefreshMemory = activeEditorSource === "memory";
  activeEditorPath = "";
  activeEditorSource = "";
  hideBanner("fileEditorBanner");
  if (shouldRefreshMemory) {
    refreshMemory({ showSuccessBanner: false }).catch((err) =>
      showBanner("memoryBanner", err.message || t("memoryLoadError"), true)
    );
  }
}

async function loadFileIntoEditor(path, options = {}) {
  const { allowMissing = false } = options;
  const response = await fetch("/files" + path, { cache: "no-store" });
  if (!response.ok) {
    if (allowMissing && response.status === 404) {
      document.getElementById("fileEditorPath").textContent = path;
      document.getElementById("fileEditorTextarea").value = "";
      activeEditorPath = path;
      return;
    }
    throw new Error((await response.text()) || t("fileEditorLoadError"));
  }

  const content = await response.text();
  document.getElementById("fileEditorPath").textContent = path;
  document.getElementById("fileEditorTextarea").value = content;
  activeEditorPath = path;
}

async function openFileEditor(path) {
  hideBanner("fileEditorBanner");
  activeEditorSource = "files";
  await loadFileIntoEditor(path);
  applyFileEditorAccessMode();
  openFileEditorModal();
}

async function openMemoryEditor(path) {
  hideBanner("fileEditorBanner");
  activeEditorSource = "memory";
  await loadFileIntoEditor(path, { allowMissing: true });
  applyFileEditorAccessMode();
  openFileEditorModal();
}

async function refreshFileEditor() {
  const button = document.getElementById("refreshFileEditorButton");
  if (!activeEditorPath) return;

  button.disabled = true;
  hideBanner("fileEditorBanner");
  try {
    await loadFileIntoEditor(activeEditorPath, {
      allowMissing: activeEditorSource === "memory",
    });
  } catch (err) {
    showBanner("fileEditorBanner", err.message || t("fileEditorLoadError"), true);
  } finally {
    button.disabled = false;
  }
}

async function saveFileEditor() {
  const button = document.getElementById("saveFileEditorButton");
  const textarea = document.getElementById("fileEditorTextarea");
  if (!activeEditorPath) return;
  if (activeEditorSource === "files" && !isFileDevModeEnabled()) {
    showBanner("fileEditorBanner", t("fileDevModeRequired"), true);
    return;
  }

  button.disabled = true;
  hideBanner("fileEditorBanner");
  try {
    const response = await fetch(
      "/api/files/upload?path=" + encodeURIComponent(activeEditorPath),
      {
        method: "POST",
        cache: "no-store",
        body: new Blob([textarea.value], { type: "text/plain; charset=utf-8" }),
      }
    );
    if (!response.ok) {
      throw new Error((await response.text()) || t("fileEditorSaveError"));
    }
    showBanner("fileEditorBanner", t("fileEditorSaved"));
    await loadFiles();
  } catch (err) {
    showBanner("fileEditorBanner", err.message || t("fileEditorSaveError"), true);
  } finally {
    button.disabled = false;
  }
}

/* ═══════════════════════════════════════════════════
   Memory Manager
   ═══════════════════════════════════════════════════ */

function renderMemoryState() {
  MEMORY_FILES.forEach((file) => {
    const editor = document.getElementById(file.id);
    const missing = file.missingId ? document.getElementById(file.missingId) : null;
    const exists = !!memoryFilePresence[file.path];
    const editButton = document.querySelector(`.memory-edit-btn[data-path="${file.path}"]`);

    if (editor) {
      editor.value = memoryContents[file.path] || "";
      editor.readOnly = true;
      editor.classList.toggle("hidden", !exists);
    }
    if (missing) {
      missing.classList.toggle("hidden", exists);
    }
    if (editButton) {
      const showEdit = !!file.editable && exists;
      editButton.classList.toggle("hidden", !showEdit);
      editButton.disabled = !showEdit;
    }
  });
}

async function loadMemory() {
  hideBanner("memoryBanner");

  const dirResponse = await fetch("/api/files?path=" + encodeURIComponent("/memory"), {
    cache: "no-store",
  });

  if (!dirResponse.ok) {
    if (dirResponse.status === 404) {
      memoryContents = {};
      memoryFilePresence = {};
      MEMORY_FILES.forEach((file) => {
        memoryContents[file.path] = "";
        memoryFilePresence[file.path] = false;
      });
      memoryLoaded = true;
      renderMemoryState();
      return;
    }
    throw new Error((await dirResponse.text()) || t("memoryLoadError"));
  }

  const dirData = await dirResponse.json();
  const entries = dirData.entries || [];
  const entryNames = new Set(entries.map((entry) => entry.name));
  memoryContents = {};
  memoryFilePresence = {};

  for (const file of MEMORY_FILES) {
    const fileName = file.path.split("/").pop();
    const exists = entryNames.has(fileName);
    memoryFilePresence[file.path] = exists;
    if (!exists) {
      memoryContents[file.path] = "";
      continue;
    }

    const response = await fetch("/files" + file.path, {
      cache: "no-store",
    });
    if (!response.ok) {
      throw new Error((await response.text()) || t("memoryLoadError"));
    }
    memoryContents[file.path] = await response.text();
  }

  memoryLoaded = true;
  renderMemoryState();
}

async function refreshMemory(options = {}) {
  const { showSuccessBanner = true } = options;
  const button = document.getElementById("refreshMemoryButton");
  button.disabled = true;
  hideBanner("memoryBanner");

  try {
    await loadMemory();
    if (showSuccessBanner) {
      showBanner("memoryBanner", t("memoryRefreshed"));
    }
  } catch (err) {
    showBanner("memoryBanner", err.message || t("memoryLoadError"), true);
    throw err;
  } finally {
    button.disabled = false;
  }
}

async function refreshMemoryFile(path) {
  hideBanner("memoryBanner");

  const file = MEMORY_FILES.find((item) => item.path === path);
  if (!file) return;

  try {
    const response = await fetch("/files" + file.path, { cache: "no-store" });
    if (response.status === 404) {
      memoryFilePresence[file.path] = false;
      memoryContents[file.path] = "";
    } else if (!response.ok) {
      throw new Error((await response.text()) || t("memoryLoadError"));
    } else {
      memoryFilePresence[file.path] = true;
      memoryContents[file.path] = await response.text();
    }
    renderMemoryState();
  } catch (err) {
    showBanner("memoryBanner", err.message || t("memoryLoadError"), true);
  }
}

/* ═══════════════════════════════════════════════════
   Chat
   ═══════════════════════════════════════════════════ */

let chatSessions = {};
let chatSessionId = "";
let chatMessages = [];
let chatPollingId = null;
let chatPollTimer = null;

function chatSessionName(id) {
  return id;
}

function renderSessionSelect() {
  const select = document.getElementById("chatSessionSelect");
  if (!select) return;
  select.innerHTML = "";
  Object.keys(chatSessions).forEach((id) => {
    const opt = document.createElement("option");
    opt.value = id;
    opt.textContent = chatSessionName(id);
    if (id === chatSessionId) opt.selected = true;
    select.appendChild(opt);
  });
}

async function loadSessions() {
  try {
    const resp = await fetch("/api/sessions", { cache: "no-store" });
    const list = await resp.json();
    chatSessions = {};
    list.forEach((item) => {
      chatSessions[item.id] = { messages: [] };
    });
    if (!chatSessionId || !chatSessions[chatSessionId]) {
      const ids = Object.keys(chatSessions);
      if (ids.length > 0) {
        chatSessionId = ids[0];
      } else {
        chatSessionId = "web-" + Date.now();
        chatSessions[chatSessionId] = { messages: [] };
      }
    }
    await loadSession(chatSessionId);
    chatMessages = chatSessions[chatSessionId].messages || [];
    renderSessionSelect();
    renderChat();
  } catch (err) {
    console.error("loadSessions failed:", err);
  }
}

async function loadSession(id) {
  try {
    const resp = await fetch("/api/sessions/" + encodeURIComponent(id), { cache: "no-store" });
    if (!resp.ok) {
      chatSessions[id] = { messages: [] };
      return;
    }
    const data = await resp.json();
    chatSessions[id] = { messages: data.messages || [] };
  } catch (err) {
    console.error("loadSession failed:", err);
    chatSessions[id] = { messages: [] };
  }
}

async function switchChatSession(id) {
  if (!chatSessions[id] || id === chatSessionId) return;
  if (chatPollTimer) clearTimeout(chatPollTimer);
  chatPollingId = null;
  await loadSession(id);
  chatSessionId = id;
  chatMessages = chatSessions[id].messages || [];
  renderSessionSelect();
  renderChat();
}

async function deleteChatSession(id) {
  if (!chatSessions[id]) return;
  if (!window.confirm(t("chatDeleteSessionConfirm"))) return;
  try {
    await fetch("/api/sessions/" + encodeURIComponent(id), { method: "DELETE" });
  } catch (err) {
    console.error("deleteSession failed:", err);
  }
  delete chatSessions[id];
  if (chatSessionId === id) {
    const remaining = Object.keys(chatSessions);
    if (remaining.length > 0) {
      chatSessionId = remaining[0];
      chatMessages = chatSessions[chatSessionId].messages || [];
    } else {
      chatSessionId = "web-" + Date.now();
      chatSessions[chatSessionId] = { messages: [] };
      chatMessages = [];
    }
  }
  renderSessionSelect();
  renderChat();
}

function renderChat() {
  const container = document.getElementById("chatMessages");
  container.innerHTML = "";

  if (!chatMessages.length) {
    const p = document.createElement("p");
    p.className = "chat-empty";
    p.textContent = t("chatEmpty");
    container.appendChild(p);
    return;
  }

  chatMessages.forEach((msg) => {
    const div = document.createElement("div");
    div.className = "chat-msg " + msg.role;
    if (msg.pending) div.classList.add("pending");
    if (msg.error) div.classList.add("error");
    div.textContent = msg.text;
    container.appendChild(div);
  });
  container.scrollTop = container.scrollHeight;
}

async function sendChatMessage() {
  const input = document.getElementById("chatInput");
  const text = input.value.trim();
  if (!text) return;

  if (chatPollingId !== null) {
    showBanner("chatBanner", t("chatBusy"), true);
    return;
  }

  hideBanner("chatBanner");
  chatMessages.push({ role: "user", text });
  chatMessages.push({ role: "assistant", text: t("chatPending"), pending: true });
  renderChat();
  input.value = "";

  try {
    const resp = await fetch("/api/chat", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ text, session_id: chatSessionId }),
    });
    const data = await resp.json();
    if (!resp.ok || !data.ok) {
      throw new Error(data.message || t("chatError"));
    }
    chatPollingId = data.request_id;
    pollChatResult();
  } catch (err) {
    chatMessages.pop();
    chatMessages.push({ role: "assistant", text: err.message, error: true });
    renderChat();
    chatPollingId = null;
  }
}

function pollChatResult() {
  if (chatPollingId === null) return;
  const id = chatPollingId;

  chatPollTimer = setTimeout(async () => {
    try {
      const resp = await fetch("/api/chat/result?id=" + id + "&_=" + Date.now(), {
        cache: "no-store",
      });
      const data = await resp.json();
      if (data.status === "pending") {
        pollChatResult();
        return;
      }
      chatPollingId = null;
      chatMessages.pop();
      if (data.status === "ok") {
        chatMessages.push({ role: "assistant", text: data.text });
      } else {
        chatMessages.push({ role: "assistant", text: data.message || t("chatError"), error: true });
      }
      renderChat();
    } catch (err) {
      chatPollingId = null;
      chatMessages.pop();
      chatMessages.push({ role: "assistant", text: err.message, error: true });
      renderChat();
    }
  }, 2000);
}

function newChatSession() {
  if (chatPollTimer) clearTimeout(chatPollTimer);
  chatPollingId = null;
  chatSessionId = "web-" + Date.now();
  chatSessions[chatSessionId] = { messages: [] };
  chatMessages = [];
  renderSessionSelect();
  hideBanner("chatBanner");
  renderChat();
}

function clearChat() {
  if (chatPollTimer) clearTimeout(chatPollTimer);
  chatPollingId = null;
  chatMessages = [];
  chatSessions[chatSessionId] = { messages: [] };
  hideBanner("chatBanner");
  renderChat();
}

/* ═══════════════════════════════════════════════════
   Event binding
   ═══════════════════════════════════════════════════ */

function bindEvents() {
  initTabs();

  document.getElementById("saveConfigButton").addEventListener("click", saveConfig);

  document.getElementById("llm_provider_preset").addEventListener("change", (e) => {
    applyProviderPreset(e.target.value);
  });

  ["llm_backend_type", "llm_profile", "llm_base_url", "llm_auth_type"].forEach(
    (field) => {
      const input = document.getElementById(field);
      if (input) input.addEventListener("input", syncProviderPreset);
    }
  );

  document
    .getElementById("wechatLoginStartButton")
    .addEventListener("click", startWechatLogin);
  document
    .getElementById("wechatLoginCancelButton")
    .addEventListener("click", cancelWechatLogin);

  document.getElementById("refreshFilesButton").addEventListener("click", () =>
    loadFiles().catch((err) => showBanner("fileBanner", err.message, true))
  );
  document.getElementById("devModeButton").addEventListener("click", () => {
    const nextEnabled = !isFileDevModeEnabled();
    if (nextEnabled && !window.confirm(t("fileDevModeConfirm"))) {
      return;
    }
    setFileDevMode(nextEnabled);
  });
  document.getElementById("upDirButton").addEventListener("click", () => {
    currentPath = parentPath(currentPath);
    loadFiles().catch((err) => showBanner("fileBanner", err.message, true));
  });
  document.getElementById("uploadButton").addEventListener("click", uploadFile);
  document.getElementById("refreshMemoryButton").addEventListener("click", refreshMemory);
  document.querySelectorAll(".memory-refresh-btn").forEach((button) => {
    button.addEventListener("click", () => refreshMemoryFile(button.dataset.path));
  });
  document.querySelectorAll(".memory-edit-btn").forEach((button) => {
    button.addEventListener("click", () => {
      openMemoryEditor(button.dataset.path).catch((err) =>
        showBanner("memoryBanner", err.message || t("fileEditorLoadError"), true)
      );
    });
  });
  document
    .getElementById("closeFileEditorButton")
    .addEventListener("click", closeFileEditorModal);
  document
    .getElementById("refreshFileEditorButton")
    .addEventListener("click", refreshFileEditor);
  document
    .getElementById("saveFileEditorButton")
    .addEventListener("click", saveFileEditor);
  document.getElementById("fileEditorModal").addEventListener("click", (e) => {
    if (e.target.id === "fileEditorModal") {
      closeFileEditorModal();
    }
  });
  document.getElementById("chooseFileButton").addEventListener("click", () => {
    document.getElementById("uploadFileInput").click();
  });
  document.getElementById("createFolderButton").addEventListener("click", createFolder);
  document.getElementById("uploadFileInput").addEventListener("change", (e) => {
    const file = e.target.files[0];
    const nameSpan = document.getElementById("selectedFileName");
    if (file) {
      document.getElementById("uploadPathInput").value = joinPath(
        currentPath,
        file.name
      );
      nameSpan.textContent = file.name;
    } else {
      nameSpan.textContent = t("fileNoFileSelected");
    }
  });

  document.getElementById("chatSendBtn").addEventListener("click", sendChatMessage);
  document.getElementById("chatNewSessionBtn").addEventListener("click", newChatSession);
  document.getElementById("chatClearBtn").addEventListener("click", clearChat);
  document.getElementById("chatDeleteSessionBtn").addEventListener("click", () => {
    deleteChatSession(chatSessionId);
  });
  document.getElementById("chatSessionSelect").addEventListener("change", (e) => {
    switchChatSession(e.target.value);
  });
  document.getElementById("chatInput").addEventListener("keydown", (e) => {
    if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) {
      e.preventDefault();
      sendChatMessage();
    }
  });
}

/* ═══════════════════════════════════════════════════
   Bootstrap
   ═══════════════════════════════════════════════════ */

async function bootstrap() {
  fileDevModeEnabled = false;
  currentLang = detectLang();
  applyI18n();
  buildLocaleMenu();
  bindEvents();
  updateFileDevModeUI();

  try {
    await loadStatus();
  } catch (err) {
    showBanner("configBanner", err.message || "Failed to load device status", true);
  }

  try {
    await loadConfig();
  } catch (err) {
    showBanner("configBanner", err.message || "Failed to load settings", true);
  }

  try {
    await pollWechatLoginStatus();
  } catch (err) {
    showBanner(
      "wechatLoginBanner",
      err.message || "Failed to load WeChat login status",
      true
    );
  }

  try {
    await loadFiles();
  } catch (err) {
    showBanner("fileBanner", err.message || "Failed to load file list", true);
  }

  if (document.querySelector('.tab-btn[data-tab="memory"]')?.classList.contains("active")) {
    try {
      await loadMemory();
    } catch (err) {
      showBanner("memoryBanner", err.message || t("memoryLoadError"), true);
    }
  } else {
    renderMemoryState();
  }

  await loadSessions();
}

bootstrap().catch((err) => {
  showBanner("configBanner", err.message || "Failed to initialize the page", true);
});
