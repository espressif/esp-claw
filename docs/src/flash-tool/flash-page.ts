import { connect, connectWithPort, type ESPLoader, type Logger } from "tasmota-webserial-esptool";

const FLASH_UART_BAUD_FAST = 921600;
const FLASH_UART_BAUD_ROM = 115200;
const ESP_USB_JTAG_VID = 0x303a;
const ESP_USB_JTAG_PID = 0x1001;

const WIFI_STATUS_PROBE_ATTEMPTS = 3;
const WIFI_STATUS_PROBE_WAIT_MS = 1000;
const WIFI_STATUS_PROBE_RETRY_GAP_MS = 3000;

const FIRMWARE_SITE_URL = import.meta.env.PUBLIC_FIRMWARE_SITE_URL ?? "https://esp-claw.com/versions";

// ── Types ────────────────────────────────────────────────────────────────────

/** Stub-only APIs exist on EspStubLoader but are not exported by the package. */
type ESPStubLoader = ESPLoader & {
  eraseFlash(): Promise<void>;
  eraseRegion(offset: number, size: number): Promise<void>;
};

type PartitionEntry = {
  name: string;
  type: string;
  subtype: string;
  offset: string;
  size: string;
};

type FlashSettings = {
  flash_mode: string;
  flash_freq: string;
  flash_size: string;
};

type FirmwareRecord = {
  flash_files: Record<string, string>;
  flash_settings: FlashSettings;
  partition_table: PartitionEntry[];
  nvs_info?: { start_addr: string; size: string };
  min_flash_size: number;
  min_psram_size: number;
};

type BoardsTree = Record<string, Record<string, Record<string, Record<string, FirmwareRecord>>>>;

type VersionEntry = {
  commit_timestamp?: string;
  boards: BoardsTree;
};

type MergedDb = Record<string, Record<string, VersionEntry>>;

type MasterFirmwareDb = Record<string, BoardsTree>;

type TaggedVersionsDb = Record<string, Record<string, {
  commit_timestamp: string;
  boards: BoardsTree;
}>>;

type Strings = {
  connectBtn: string;
  disconnectBtn: string;
  connectingMsg: string;
  notConnected: string;
  connectedTo: string;
  webSerialUnsupported: string;
  connectErrorPrefix: string;
  chooseChipLabel: string;
  chooseBrandLabel: string;
  chooseBoardLabel: string;
  chooseConsoleOutputLabel: string;
  chooseChipPlaceholder: string;
  chooseBrandPlaceholder: string;
  chooseConsoleOutputPlaceholder: string;
  boardAutoHint: string;
  selectedBoardLabel: string;
  noBrandSelected: string;
  noBoardSelected: string;
  boardFlashMeta: string;
  boardPsramMeta: string;
  psramUnknown: string;
  firmwareRequirementsLabel: string;
  firmwareDescriptionLabel: string;
  downloadFirmwareLocalLink: string;
  downloadBtn: string;
  flashBtn: string;
  flashBtnDisabledNoDevice: string;
  flashBtnDisabledNoFirmware: string;
  flashBtnDisabledNoConsoleOutput: string;
  flashBtnDisabledNoMatch: string;
  actionReadyHint: string;
  noFirmwareTitle: string;
  noFirmwareDesc: string;
  viewSupportedBoardsBtn: string;
  progressLabel: string;
  downloadingFirmware: string;
  writingFlash: string;
  waitingForDeviceInfo: string;
  flashSuccess: string;
  flashError: string;
  postFlashReconnectTitle: string;
  postFlashReconnectDesc: string;
  postFlashReconnectBtn: string;
  postFlashReconnectBusy: string;
  postFlashReconnectSuccess: string;
  postFlashReconnectError: string;
  wifiSectionTitle: string;
  wifiPrompt: string;
  wifiSameNetworkHint: string;
  wifiSsidLabel: string;
  wifiPasswordLabel: string;
  wifiPasswordLengthError: string;
  wifiSubmitBtn: string;
  wifiConnecting: string;
  wifiStatusProbeAttempt: string;
  wifiProbeError: string;
  wifiTimeoutError: string;
  wifiReadyTitle: string;
  wifiReadyDesc: string;
  openDeviceBtn: string;
  consoleToggleOpen: string;
  consoleToggleClose: string;
  consoleTitle: string;
  consoleClearBtn: string;
  consoleResetBtn: string;
  consoleResetUnsupported: string;
  consoleWaiting: string;
  consoleSendBtn: string;
  consoleSendPlaceholder: string;
  consoleCloseBtn: string;
  deviceChipLabel: string;
  deviceRevisionLabel: string;
  deviceFlashLabel: string;
  devicePsramLabel: string;
  tabFlash: string;
  tabConsole: string;
  consoleTabDisabledHint: string;
  modalStep1Title: string;
  modalStep2Title: string;
  modalStep3Title: string;
  terminalLabel: string;
  chooseApplicationLabel: string;
  chooseApplicationPlaceholder: string;
  chooseVersionLabel: string;
  chooseVersionPlaceholder: string;
  versionMaster: string;
  advancedSettingsLabel: string;
  flashModeLabel: string;
  flashModeFullErase: string;
  flashModeEraseSelectPartitions: string;
  flashModeSelectPartitions: string;
  partitionSelectionHintErase: string;
  partitionSelectionHintPreserve: string;
  erasingFlash: string;
  downloadingPartitions: string;
  flashingPartition: string;
  loadingVersions: string;
  loadVersionsError: string;
};

type BootData = {
  lang: string;
  firmwareDb: MasterFirmwareDb;
  masterRef: string;
  strings: Strings;
  boardsHref: string;
};

type DeviceInfo = {
  chipName: string | null;
  chipKey: string | null;
  chipRevision: number | null;
  flashSizeMb: number | null;
  psramSizeMb: number | null;
};

type VisibleBoard = {
  chipKey: string;
  brandKey: string;
  boardKey: string;
  firmware: FirmwareRecord;
};

type WifiStatus = {
  connected: boolean;
  configured: boolean;
  ip: string | null;
};

type DetectedConsoleOutput = "UART" | "JTAG" | null;

type Waiter<T> = {
  resolve: (value: T) => void;
  reject: (reason?: unknown) => void;
  timer: number;
};

// ── Boot ─────────────────────────────────────────────────────────────────────

const bootEl = document.getElementById("flash-boot");
if (!bootEl?.textContent) {
  throw new Error("Missing flash boot data");
}

const boot = JSON.parse(bootEl.textContent) as BootData;
const s = boot.strings;

const els = {
  unsupportedBanner: must("unsupported-banner"),
  connectBar: must("connect-bar"),
  connectBarIdle: must("connect-bar-idle"),
  connectBarActive: must("connect-bar-active"),
  connectBtn: must<HTMLButtonElement>("connect-btn"),
  connectBtnLabel: must("connect-btn-label"),
  disconnectBtn: must<HTMLButtonElement>("disconnect-btn"),
  statusText: must("status-text"),
  infoChip: must("info-chip"),
  infoRevision: must("info-revision"),
  infoRevisionSep: must("info-revision-sep"),
  infoFlash: must("info-flash"),
  infoFlashSep: must("info-flash-sep"),
  infoPsram: must("info-psram"),
  infoPsramSep: must("info-psram-sep"),
  connectError: must("connect-error"),
  selectionCard: must("selection-card"),
  selectionFields: must("selection-fields"),
  appSelect: must<HTMLSelectElement>("app-select"),
  versionSelect: must<HTMLSelectElement>("version-select"),
  chipSelect: must<HTMLSelectElement>("chip-select"),
  brandSelect: must<HTMLSelectElement>("brand-select"),
  boardSelect: must<HTMLSelectElement>("board-select"),
  consoleOutputSelect: must<HTMLSelectElement>("console-output-select"),
  selectedBoardName: must("selected-board-name"),
  selectedBoardDesc: must("selected-board-desc"),
  selectedBoardMeta: must("selected-board-meta"),
  downloadBtn: must<HTMLAnchorElement>("download-btn"),
  flashBtn: must<HTMLButtonElement>("flash-btn"),
  flashHint: must("flash-hint"),
  actionConnectBtn: must<HTMLButtonElement>("action-connect-btn"),
  actionConnectBtnLabel: must("action-connect-btn-label"),
  actionDisconnectBtn: must<HTMLButtonElement>("action-disconnect-btn"),
  consoleToggleBtn: must<HTMLButtonElement>("console-toggle-btn"),
  consoleToggleBtnLabel: must("console-toggle-btn-label"),
  noFirmwareCard: must("no-firmware-card"),
  selectedBoardSummary: must("selected-board-summary"),
  partitionList: must("partition-list"),
  partitionSelection: must("partition-selection"),
  partitionSelectionHint: must("partition-selection-hint"),

  consolePanel: must("console-panel"),
  consoleOutput: must("console-output"),
  consoleEmpty: must("console-empty"),
  consoleClearBtn: must<HTMLButtonElement>("console-clear-btn"),
  consoleResetBtn: must<HTMLButtonElement>("console-reset-btn"),
  consoleSendInput: must<HTMLInputElement>("console-send-input"),
  consoleSendBtn: must<HTMLButtonElement>("console-send-btn"),

  flashModalBg: must("flash-modal-bg"),
  modalCloseBtn: must<HTMLButtonElement>("modal-close-btn"),
  modalStep1Indicator: must("modal-step1-indicator"),
  modalStep2Indicator: must("modal-step2-indicator"),
  modalStep3Indicator: must("modal-step3-indicator"),
  modalStep1Body: must("modal-step1-body"),
  modalStep2Body: must("modal-step2-body"),
  modalStep3Body: must("modal-step3-body"),
  modalFlashResult: must("modal-flash-result"),
  modalProgressWrap: must("modal-progress-wrap"),
  modalProgressStage: must("modal-progress-stage"),
  modalProgressPct: must("modal-progress-pct"),
  modalProgressBar: must("modal-progress-bar"),
  modalProgressLog: must("modal-progress-log"),
  modalReconnectPrompt: must("modal-reconnect-prompt"),
  modalReconnectBtn: must<HTMLButtonElement>("modal-reconnect-btn"),
  modalReconnectStatus: must("modal-reconnect-status"),
  modalWifiPrompt: must("modal-wifi-prompt"),
  modalWifiSsid: must<HTMLInputElement>("modal-wifi-ssid"),
  modalWifiPassword: must<HTMLInputElement>("modal-wifi-password"),
  modalWifiSubmitBtn: must<HTMLButtonElement>("modal-wifi-submit-btn"),
  modalWifiStatus: must("modal-wifi-status"),
  modalReadyTitle: must("modal-ready-title"),
  modalReadyDesc: must("modal-ready-desc"),
  modalReadyLink: must<HTMLAnchorElement>("modal-ready-link"),
  modalTerminalDetails: must<HTMLDetailsElement>("modal-terminal-details"),
  modalTerminalOutput: must("modal-terminal-output"),
  modalTerminalEmpty: must("modal-terminal-empty"),
};

const state = {
  serial: "idle" as "unsupported" | "idle" | "connecting" | "connected" | "error",
  flash: "idle" as "idle" | "downloading" | "flashing" | "flashed" | "postflash" | "error",
  provision:
    "idle" as
      | "idle"
      | "waiting_boot"
      | "waiting_reconnect"
      | "probing_wifi"
      | "need_wifi"
      | "connecting_wifi"
      | "ready"
      | "error",
  activeTab: "flash" as "flash" | "console",
  modalStep: 0 as 0 | 1 | 2 | 3,
  modalCanClose: false,
  loader: null as ESPLoader | null,
  device: {
    chipName: null,
    chipKey: null,
    chipRevision: null,
    flashSizeMb: null,
    psramSizeMb: null,
  } as DeviceInfo,
  mergedDb: {} as MergedDb,
  selectedApp: null as string | null,
  selectedVersion: null as string | null,
  selectedChip: null as string | null,
  selectedBrand: null as string | null,
  selectedBoardId: null as string | null,
  selectedConsoleOutput: null as string | null,
  flashMode: "full-erase" as "full-erase" | "erase-select-partitions" | "select-partitions",
  selectedPartitions: new Set<string>(),
  visibleBoards: [] as VisibleBoard[],
  detectedConsoleOutput: null as DetectedConsoleOutput,
  readyIp: null as string | null,
  progressLines: [] as string[],
  consoleText: "",
  consoleReader: null as ReadableStreamDefaultReader<Uint8Array> | null,
  consoleReading: false,
  consoleLineBuffer: "",
  statusWaiters: [] as Waiter<WifiStatus>[],
  readyWaiters: [] as Waiter<string>[],
  inConsoleMode: false,
  reconnectingPort: false,
};

const logger: Logger = {
  log(message: string) {
    addProgressLine(message);
    appendConsole(`[tool] ${message}\n`);
  },
  error(message: string) {
    addProgressLine(message);
    appendConsole(`[error] ${message}\n`);
  },
  debug(message: string) {
    appendConsole(`[debug] ${message}\n`);
  },
};

init().catch((error) => {
  console.error(error);
  showConnectError(`${s.connectErrorPrefix}${getErrorMessage(error)}`);
});

async function init() {
  if (!("serial" in navigator)) {
    state.serial = "unsupported";
    els.unsupportedBanner.classList.add("visible");
    els.connectBtn.disabled = true;
  }

  buildMergedDb();
  await loadTaggedVersions();

  const appKeys = getAppKeys();
  state.selectedApp = appKeys[0] ?? null;
  state.selectedVersion = "master";

  renderAppOptions();
  renderVersionOptions();
  renderChipOptions();
  refreshBoards();
  renderActionState();
  renderConsole();

  els.appSelect.addEventListener("change", () => {
    state.selectedApp = els.appSelect.value || null;
    state.selectedVersion = "master";
    state.selectedChip = null;
    state.selectedBrand = null;
    state.selectedBoardId = null;
    renderVersionOptions();
    renderChipOptions();
    refreshBoards();
  });
  els.versionSelect.addEventListener("change", () => {
    state.selectedVersion = els.versionSelect.value || null;
    state.selectedChip = null;
    state.selectedBrand = null;
    state.selectedBoardId = null;
    renderChipOptions();
    refreshBoards();
  });
  els.chipSelect.addEventListener("change", () => {
    state.selectedChip = els.chipSelect.value || null;
    state.selectedBrand = null;
    state.selectedBoardId = null;
    refreshBoards();
  });
  els.brandSelect.addEventListener("change", () => {
    state.selectedBrand = els.brandSelect.value || null;
    state.selectedBoardId = null;
    refreshBoards();
  });
  els.boardSelect.addEventListener("change", () => {
    state.selectedBoardId = els.boardSelect.value || null;
    normalizeConsoleOutputSelection();
    renderConsoleOutputOptions();
    renderSelectedBoard();
    renderPartitionList();
    renderActionState();
  });
  els.consoleOutputSelect.addEventListener("change", () => {
    state.selectedConsoleOutput = els.consoleOutputSelect.value || null;
    renderSelectedBoard();
    renderPartitionList();
    renderActionState();
  });

  for (const radio of document.querySelectorAll<HTMLInputElement>('input[name="flash-mode"]')) {
    radio.addEventListener("change", () => {
      state.flashMode = radio.value as typeof state.flashMode;
      updatePartitionSelectionUi();
      renderPartitionList();
    });
  }

  els.connectBtn.addEventListener("click", () => { void connectDevice(); });
  els.actionConnectBtn.addEventListener("click", () => { void connectDevice(); });
  els.disconnectBtn.addEventListener("click", () => { void disconnectDevice(); });
  els.actionDisconnectBtn.addEventListener("click", () => { void disconnectDevice(); });
  els.flashBtn.addEventListener("click", () => { void flashSelectedFirmware(); });
  els.consoleToggleBtn.addEventListener("click", () => {
    if (!els.consoleToggleBtn.disabled) {
      switchTab(state.activeTab === "console" ? "flash" : "console");
    }
  });
  els.consoleClearBtn.addEventListener("click", () => { state.consoleText = ""; renderConsole(); });
  els.consoleResetBtn.addEventListener("click", () => { void resetDeviceFromConsole(); });
  els.consoleSendBtn.addEventListener("click", () => { void sendConsoleInput(); });
  els.consoleSendInput.addEventListener("keydown", (e) => {
    if ((e as KeyboardEvent).key === "Enter") { void sendConsoleInput(); }
  });
  els.modalWifiSubmitBtn.addEventListener("click", () => { void submitWifiCredentials(); });
  els.modalReconnectBtn.addEventListener("click", () => { void reconnectDeviceAfterReset(); });
  els.modalCloseBtn.addEventListener("click", () => { if (state.modalCanClose) closeModal(); });
  els.flashModalBg.addEventListener("click", (e) => {
    if (e.target === els.flashModalBg && state.modalCanClose) closeModal();
  });
}

function must<T extends HTMLElement = HTMLElement>(id: string): T {
  const el = document.getElementById(id);
  if (!el) throw new Error(`Missing element #${id}`);
  return el as T;
}

// ── Data merging ─────────────────────────────────────────────────────────────

function buildMergedDb() {
  const db: MergedDb = {};
  for (const [app, boards] of Object.entries(boot.firmwareDb)) {
    if (!db[app]) db[app] = {};
    db[app]["master"] = { boards };
  }
  state.mergedDb = db;
}

async function loadTaggedVersions() {
  if (!FIRMWARE_SITE_URL) return;
  try {
    const resp = await fetch(`${FIRMWARE_SITE_URL}/versions.json`);
    if (!resp.ok) return;
    const tagged = (await resp.json()) as TaggedVersionsDb;
    for (const [app, versions] of Object.entries(tagged)) {
      if (!state.mergedDb[app]) state.mergedDb[app] = {};
      for (const [refs, entry] of Object.entries(versions)) {
        state.mergedDb[app][refs] = {
          commit_timestamp: entry.commit_timestamp,
          boards: entry.boards,
        };
      }
    }
  } catch {
    // versions unavailable, continue with master only
  }
}

function getAppKeys(): string[] {
  return Object.keys(state.mergedDb).sort();
}

function getVersionKeys(): string[] {
  const app = state.selectedApp;
  if (!app || !state.mergedDb[app]) return [];
  const keys = Object.keys(state.mergedDb[app]);
  keys.sort((a, b) => {
    if (a === "master") return -1;
    if (b === "master") return 1;
    return b.localeCompare(a);
  });
  return keys;
}

function getCurrentBoardsTree(): BoardsTree {
  const app = state.selectedApp;
  const version = state.selectedVersion;
  if (!app || !version) return {};
  return state.mergedDb[app]?.[version]?.boards ?? {};
}

function getChipKeys(): string[] {
  return Object.keys(getCurrentBoardsTree());
}

// ── Tab management ───────────────────────────────────────────────────────────

function switchTab(tab: "flash" | "console") {
  state.activeTab = tab;
  const isConsole = tab === "console";
  els.selectionCard.hidden = isConsole;
  els.consolePanel.hidden = !isConsole;
  els.consoleToggleBtn.classList.toggle("active", isConsole);
  els.consoleToggleBtn.setAttribute("aria-pressed", String(isConsole));
  els.consoleToggleBtnLabel.textContent = isConsole ? s.consoleToggleClose : s.consoleToggleOpen;
  renderActionState();
}

function canOpenConsoleTab(): boolean {
  return (
    state.serial === "connected" &&
    state.flash !== "downloading" &&
    state.flash !== "flashing" &&
    state.flash !== "postflash" &&
    state.provision !== "waiting_boot" &&
    state.provision !== "waiting_reconnect"
  );
}

function updateConsoleTabState() {
  const enabled = canOpenConsoleTab();
  els.consoleToggleBtn.disabled = !enabled;
  if (enabled) {
    els.consoleToggleBtn.removeAttribute("title");
  } else {
    els.consoleToggleBtn.title = s.consoleTabDisabledHint;
  }
  if (!enabled && state.activeTab === "console") {
    switchTab("flash");
  }
}

// ── Modal management ─────────────────────────────────────────────────────────

function openModal() {
  state.modalStep = 1;
  state.modalCanClose = false;
  els.flashModalBg.classList.add("open");
  renderModalStep(1);
}

function closeModal() {
  state.modalStep = 0;
  els.flashModalBg.classList.remove("open");
}

function renderModalStep(step: 1 | 2 | 3) {
  state.modalStep = step;
  const indicators = [els.modalStep1Indicator, els.modalStep2Indicator, els.modalStep3Indicator];
  indicators.forEach((el, i) => {
    el.classList.remove("active", "done");
    if (i + 1 === step) el.classList.add("active");
    else if (i + 1 < step) el.classList.add("done");
  });
  els.modalStep1Body.style.display = step === 1 ? "" : "none";
  els.modalStep2Body.style.display = step === 2 ? "" : "none";
  els.modalStep3Body.style.display = step === 3 ? "" : "none";
  els.modalTerminalDetails.style.display = step === 3 ? "none" : "";
}

function setModalCloseable(closeable: boolean) {
  state.modalCanClose = closeable;
  els.modalCloseBtn.classList.toggle("visible", closeable);
}

// ── Rendering: App / Version / Board selection ───────────────────────────────

function renderAppOptions() {
  els.appSelect.innerHTML = "";
  const placeholder = document.createElement("option");
  placeholder.value = "";
  placeholder.textContent = s.chooseApplicationPlaceholder;
  placeholder.selected = !state.selectedApp;
  els.appSelect.appendChild(placeholder);

  for (const appKey of getAppKeys()) {
    const option = document.createElement("option");
    option.value = appKey;
    option.textContent = appKey.replace(/_/g, " ");
    if (appKey === state.selectedApp) option.selected = true;
    els.appSelect.appendChild(option);
  }
}

function renderVersionOptions() {
  els.versionSelect.innerHTML = "";
  const placeholder = document.createElement("option");
  placeholder.value = "";
  placeholder.textContent = s.chooseVersionPlaceholder;
  placeholder.selected = !state.selectedVersion;
  els.versionSelect.appendChild(placeholder);

  for (const versionKey of getVersionKeys()) {
    const option = document.createElement("option");
    option.value = versionKey;
    option.textContent = versionKey === "master" ? `master (${boot.masterRef})` : versionKey;
    if (versionKey === state.selectedVersion) option.selected = true;
    els.versionSelect.appendChild(option);
  }

  els.versionSelect.disabled = !state.selectedApp;
}

function renderChipOptions() {
  els.chipSelect.innerHTML = "";
  const chipKeys = getChipKeys();

  const placeholder = document.createElement("option");
  placeholder.value = "";
  placeholder.textContent = s.chooseChipPlaceholder;
  placeholder.selected = !state.selectedChip;
  els.chipSelect.appendChild(placeholder);

  for (const chipKey of chipKeys) {
    const option = document.createElement("option");
    option.value = chipKey;
    option.textContent = chipLabel(chipKey);
    if (chipKey === state.selectedChip) option.selected = true;
    els.chipSelect.appendChild(option);
  }

  els.chipSelect.disabled = chipKeys.length === 0 || Boolean(state.device.chipKey);
}

function refreshBoards() {
  normalizeSelectionState();
  renderChipOptions();
  renderBrandOptions();
  state.visibleBoards = getVisibleBoards();
  if (!currentSelectionStillVisible()) {
    state.selectedBoardId = null;
  }
  renderBoardOptions();
  normalizeConsoleOutputSelection();
  renderConsoleOutputOptions();
  renderSelectedBoard();
  renderPartitionList();
  renderNoFirmwareState();
  renderActionState();
}

function normalizeSelectionState() {
  const chipKeys = getChipKeys();
  state.selectedChip = getNormalizedSelectedChip(chipKeys);
  if (!state.selectedChip) {
    state.selectedBrand = null;
    state.selectedBoardId = null;
    return;
  }
  const brandKeys = getBrandKeys(state.selectedChip);
  if (!brandKeys.includes(state.selectedBrand ?? "")) {
    state.selectedBrand = brandKeys[0] ?? null;
  }
}

function normalizeConsoleOutputSelection() {
  const selected = getSelectedFirmware();
  const consoleOutputs = getVisibleConsoleOutputKeys(selected?.firmware ?? null);
  if (!consoleOutputs.includes(state.selectedConsoleOutput ?? "")) {
    state.selectedConsoleOutput = consoleOutputs[0] ?? null;
  }
}

function getNormalizedSelectedChip(chipKeys: string[]) {
  if (state.device.chipKey) {
    const compatibleChipKeys = chipKeys.filter((ck) => hasCompatibleBoardsForChip(ck));
    if (compatibleChipKeys.includes(state.selectedChip ?? "")) return state.selectedChip;
    return compatibleChipKeys[0] ?? null;
  }
  if (state.selectedChip && chipKeys.includes(state.selectedChip)) return state.selectedChip;
  return chipKeys[0] ?? null;
}

function hasCompatibleBoardsForChip(chipKey: string) {
  const boards = getCurrentBoardsTree();
  const brands = boards[chipKey] ?? {};
  return Object.values(brands).some((boardMap) =>
    Object.values(boardMap).some((consoles) =>
      Object.values(consoles).some((fw) => isCompatibleWithCurrentDevice(chipKey, fw)),
    ),
  );
}

function renderBrandOptions() {
  els.brandSelect.innerHTML = "";
  const placeholder = document.createElement("option");
  placeholder.value = "";
  placeholder.textContent = s.chooseBrandPlaceholder;
  placeholder.selected = !state.selectedBrand;
  els.brandSelect.appendChild(placeholder);

  const brandKeys = state.selectedChip ? getBrandKeys(state.selectedChip) : [];
  for (const brandKey of brandKeys) {
    const option = document.createElement("option");
    option.value = brandKey;
    option.textContent = brandKey;
    option.selected = option.value === state.selectedBrand;
    els.brandSelect.appendChild(option);
  }
  els.brandSelect.disabled = !state.selectedChip || brandKeys.length === 0;
}

function getBrandKeys(chipKey: string) {
  const boards = getCurrentBoardsTree();
  return Object.keys(boards[chipKey] ?? {}).sort(compareBrandKey);
}

function compareBrandKey(a: string, b: string) {
  return brandSortWeight(a) - brandSortWeight(b) || a.localeCompare(b);
}

function brandSortWeight(brandKey: string) {
  const n = brandKey.toLowerCase();
  if (n === "espressif") return 0;
  if (n === "m5stack") return 1;
  return 2;
}

function getVisibleBoards(): VisibleBoard[] {
  const chipKey = state.selectedChip;
  const brandKey = state.selectedBrand;
  if (!chipKey || !brandKey) return [];
  const boards = getCurrentBoardsTree();
  const boardMap = boards[chipKey]?.[brandKey] ?? {};
  const result: VisibleBoard[] = [];
  for (const [boardKey, consoles] of Object.entries(boardMap)) {
    const firstConsole = Object.values(consoles)[0];
    if (firstConsole && isCompatibleWithCurrentDevice(chipKey, firstConsole)) {
      result.push({ chipKey, brandKey, boardKey, firmware: firstConsole });
    }
  }
  return result;
}

function isCompatibleWithCurrentDevice(chipKey: string, firmware: FirmwareRecord) {
  if (!state.device.chipKey) return true;
  if (!isChipKeyCompatibleWithCurrentDevice(chipKey)) return false;
  if (
    state.device.flashSizeMb != null &&
    firmware.min_flash_size != null &&
    state.device.flashSizeMb < firmware.min_flash_size
  ) return false;
  if (
    state.device.psramSizeMb != null &&
    firmware.min_psram_size != null &&
    state.device.psramSizeMb < firmware.min_psram_size
  ) return false;
  return true;
}

function getVisibleConsoleOutputKeys(firmware: FirmwareRecord | null): string[] {
  if (!firmware) return [];
  const selected = getSelectedFirmware();
  if (!selected) return [];
  const boards = getCurrentBoardsTree();
  const consoles = boards[selected.chipKey]?.[selected.brandKey]?.[selected.boardKey] ?? {};
  return Object.keys(consoles)
    .filter((co) => isConsoleOutputVisibleForCurrentDevice(co))
    .sort(compareConsoleOutputKey);
}

function isConsoleOutputVisibleForCurrentDevice(consoleOutput: string) {
  if (state.detectedConsoleOutput === "JTAG" && !consoleOutput.includes("JTAG")) return false;
  if (state.detectedConsoleOutput === "UART" && consoleOutput.includes("JTAG")) return false;
  return true;
}

function compareConsoleOutputKey(a: string, b: string) {
  return consoleOutputSortWeight(a) - consoleOutputSortWeight(b) || a.localeCompare(b);
}

function consoleOutputSortWeight(co: string) {
  if (co === "UART") return 0;
  if (co.includes("JTAG")) return 1;
  if (co === "unknown") return 2;
  return 3;
}

function isChipKeyCompatibleWithCurrentDevice(chipKey: string) {
  if (!state.device.chipKey) return true;
  const parsed = parseChipKey(chipKey);
  if (parsed.baseChipKey !== state.device.chipKey) return false;
  if (parsed.rev == null) return true;
  if (state.device.chipRevision == null) return false;
  if (parsed.baseChipKey === "esp32p4") {
    if (parsed.rev === 1) return state.device.chipRevision < 300;
    if (parsed.rev === 3) return state.device.chipRevision >= 300;
  }
  return state.device.chipRevision === parsed.rev;
}

function renderBoardOptions() {
  els.boardSelect.innerHTML = "";
  const placeholder = document.createElement("option");
  placeholder.value = "";
  placeholder.textContent = s.noBoardSelected;
  placeholder.selected = !state.selectedBoardId;
  els.boardSelect.appendChild(placeholder);

  for (const { chipKey, brandKey, boardKey } of state.visibleBoards) {
    const option = document.createElement("option");
    option.value = makeBoardId(chipKey, brandKey, boardKey);
    option.textContent = boardKey;
    option.selected = option.value === state.selectedBoardId;
    els.boardSelect.appendChild(option);
  }
  els.boardSelect.disabled = state.visibleBoards.length === 0;
}

function renderConsoleOutputOptions() {
  els.consoleOutputSelect.innerHTML = "";
  const placeholder = document.createElement("option");
  placeholder.value = "";
  placeholder.textContent = s.chooseConsoleOutputPlaceholder;
  placeholder.selected = !state.selectedConsoleOutput;
  els.consoleOutputSelect.appendChild(placeholder);

  const selected = getSelectedFirmware();
  const consoleOutputs = getVisibleConsoleOutputKeys(selected?.firmware ?? null);
  for (const co of consoleOutputs) {
    const option = document.createElement("option");
    option.value = co;
    option.textContent = consoleOutputLabel(co);
    option.selected = option.value === state.selectedConsoleOutput;
    els.consoleOutputSelect.appendChild(option);
  }
  els.consoleOutputSelect.disabled = !selected || consoleOutputs.length === 0;
}

function renderSelectedBoard() {
  const selected = getSelectedFirmware();
  const consoleRecord = getSelectedConsoleRecord();
  if (!selected || !consoleRecord) {
    els.boardSelect.value = "";
    els.selectedBoardSummary.hidden = true;
    els.selectedBoardName.textContent = s.noBoardSelected;
    els.selectedBoardDesc.textContent = "";
    els.selectedBoardMeta.innerHTML = "";
    els.downloadBtn.classList.add("disabled");
    els.downloadBtn.setAttribute("aria-disabled", "true");
    els.downloadBtn.href = "#";
    return;
  }

  els.boardSelect.value = state.selectedBoardId ?? "";
  els.consoleOutputSelect.value = state.selectedConsoleOutput ?? "";
  els.selectedBoardSummary.hidden = false;
  els.selectedBoardName.textContent = selected.boardKey;
  els.selectedBoardMeta.innerHTML = `
    <span>
      <strong>${escapeHtml(s.firmwareRequirementsLabel)}:</strong>
      ${escapeHtml(s.boardFlashMeta)} &gt;= ${escapeHtml(formatSizeRequirement(consoleRecord.min_flash_size))},
      ${escapeHtml(s.boardPsramMeta)} &gt;= ${escapeHtml(formatSizeRequirement(consoleRecord.min_psram_size))}
    </span>
  `;
  els.selectedBoardDesc.innerHTML = "";

  // Download button: disabled for per-partition model (individual files)
  els.downloadBtn.classList.add("disabled");
  els.downloadBtn.setAttribute("aria-disabled", "true");
  els.downloadBtn.href = "#";
}

function updatePartitionSelectionUi() {
  const visible = state.flashMode === "erase-select-partitions" || state.flashMode === "select-partitions";
  els.partitionSelection.style.display = visible ? "" : "none";
  els.partitionSelectionHint.textContent =
    state.flashMode === "erase-select-partitions"
      ? s.partitionSelectionHintErase
      : s.partitionSelectionHintPreserve;
}

function renderPartitionList() {
  els.partitionList.innerHTML = "";
  const record = getSelectedConsoleRecord();
  if (!record) return;

  state.selectedPartitions.clear();
  for (const [offset] of Object.entries(record.flash_files)) {
    state.selectedPartitions.add(offset);
  }

  for (const [offset, filePath] of Object.entries(record.flash_files)) {
    const fileName = filePath.split("/").pop() ?? filePath;
    const partName = findPartitionName(record.partition_table, offset) ?? fileName;

    const label = document.createElement("label");
    label.className = "partition-item";

    const checkbox = document.createElement("input");
    checkbox.type = "checkbox";
    checkbox.checked = true;
    checkbox.dataset.offset = offset;
    checkbox.addEventListener("change", () => {
      if (checkbox.checked) state.selectedPartitions.add(offset);
      else state.selectedPartitions.delete(offset);
    });

    const offsetSpan = document.createElement("span");
    offsetSpan.className = "partition-offset";
    offsetSpan.textContent = offset;

    const nameSpan = document.createElement("span");
    nameSpan.className = "partition-name";
    nameSpan.textContent = partName;

    label.appendChild(checkbox);
    label.appendChild(offsetSpan);
    label.appendChild(nameSpan);
    els.partitionList.appendChild(label);
  }
}

function findPartitionName(table: PartitionEntry[], offset: string): string | null {
  const normalizedOffset = parseHexAddress(offset);
  if (normalizedOffset === null) return null;
  for (const entry of table) {
    const entryOffset = parseHexAddress(entry.offset);
    if (entryOffset === normalizedOffset) return entry.name;
  }
  return null;
}

function renderNoFirmwareState() {
  const chipKeys = getChipKeys();
  const noCompatibleChips = Boolean(state.device.chipKey) &&
    chipKeys.filter((ck) => hasCompatibleBoardsForChip(ck)).length === 0;
  const noVisibleBoards =
    Boolean(state.device.chipKey) && Boolean(state.selectedChip) && state.visibleBoards.length === 0;
  const shouldShow = noCompatibleChips || noVisibleBoards;
  els.selectionCard.classList.toggle("selection-empty", shouldShow);
  els.selectionFields.hidden = shouldShow;
  els.noFirmwareCard.classList.toggle("visible", shouldShow);
}

function renderActionState() {
  const selected = getSelectedFirmware();
  const consoleRecord = getSelectedConsoleRecord();
  let hint = s.actionReadyHint;
  let flashDisabled = false;

  if (!selected) {
    flashDisabled = true;
    hint = state.device.chipKey ? s.flashBtnDisabledNoFirmware : s.flashBtnDisabledNoDevice;
  } else if (!consoleRecord) {
    flashDisabled = true;
    hint = s.flashBtnDisabledNoConsoleOutput;
  } else if (!state.device.chipKey) {
    flashDisabled = true;
    hint = s.flashBtnDisabledNoDevice;
  } else if (!isCompatibleWithCurrentDevice(selected.chipKey, consoleRecord)) {
    flashDisabled = true;
    hint = s.flashBtnDisabledNoMatch;
  }

  if (state.serial === "unsupported") flashDisabled = true;
  if (state.serial === "connecting" || state.flash === "downloading" || state.flash === "flashing") flashDisabled = true;
  if (state.activeTab === "console") flashDisabled = true;

  els.flashBtn.disabled = flashDisabled;
  els.flashHint.textContent = hint;
  renderConsoleSendState();
  updateConsoleTabState();
}

function renderConnectionState() {
  const connected = Boolean(state.device.chipKey);
  els.connectBar.classList.toggle("is-connected", connected);
  els.connectBarIdle.style.display = connected ? "none" : "";
  els.connectBarActive.style.display = connected ? "" : "none";

  if (connected) {
    els.statusText.textContent = `${s.connectedTo}`;
    els.infoChip.style.display = "";
    els.infoChip.textContent = `${s.deviceChipLabel}: ${state.device.chipName ?? "—"}`;

    const chipRevision = formatChipRevision(state.device.chipRevision);
    els.infoRevisionSep.style.display = chipRevision ? "" : "none";
    els.infoRevision.style.display = chipRevision ? "" : "none";
    if (chipRevision) els.infoRevision.textContent = `${s.deviceRevisionLabel}: ${chipRevision}`;

    els.infoFlashSep.style.display = "";
    els.infoFlash.style.display = "";
    els.infoFlash.textContent = `${s.deviceFlashLabel}: ${formatSizeRequirement(state.device.flashSizeMb)}`;

    const psramSize = state.device.psramSizeMb;
    els.infoPsramSep.style.display = psramSize != null ? "" : "none";
    els.infoPsram.style.display = psramSize != null ? "" : "none";
    if (psramSize != null) els.infoPsram.textContent = `${s.devicePsramLabel}: ${formatSizeRequirement(psramSize)}`;

    els.chipSelect.disabled = true;
  } else {
    els.infoChip.style.display = "none";
    els.infoRevisionSep.style.display = "none";
    els.infoRevision.style.display = "none";
    els.infoFlashSep.style.display = "none";
    els.infoFlash.style.display = "none";
    els.infoPsramSep.style.display = "none";
    els.infoPsram.style.display = "none";
    els.chipSelect.disabled = false;
  }

  els.connectBtn.disabled = state.serial === "connecting";
  els.connectBtnLabel.textContent = state.serial === "connecting" ? s.connectingMsg : s.connectBtn;
  els.actionConnectBtn.style.display = connected ? "none" : "";
  els.actionDisconnectBtn.style.display = connected ? "" : "none";
  els.actionConnectBtn.disabled = state.serial === "connecting";
  els.actionConnectBtnLabel.textContent = state.serial === "connecting" ? s.connectingMsg : s.connectBtn;

  renderConsoleSendState();
  updateConsoleTabState();
  renderReconnectState();
}

// ── Device connection ────────────────────────────────────────────────────────

async function connectDevice() {
  if (state.serial === "connecting") return;
  clearConnectError();
  state.serial = "connecting";
  renderConnectionState();

  let connectingLoader: ESPLoader | null = null;
  try {
    connectingLoader = await connect(logger);
    await connectingLoader.initialize();
    const activeLoader = await connectingLoader.runStub();
    connectingLoader = activeLoader;
    if (!activeLoader.flashSize) await activeLoader.detectFlashSize();

    state.loader = activeLoader;
    connectingLoader = null;
    state.serial = "connected";
    state.detectedConsoleOutput = detectConsoleOutputFromLoader(activeLoader);
    state.device = {
      chipName: activeLoader.chipName,
      chipKey: normalizeChipKey(activeLoader.chipName),
      chipRevision: activeLoader.chipRevision ?? null,
      flashSizeMb: parseFlashSize(activeLoader.flashSize),
      psramSizeMb: null,
    };
    renderConnectionState();
    refreshBoards();
  } catch (error) {
    if (connectingLoader) {
      try { await connectingLoader.disconnect(); } catch { /* ignore */ }
    }
    await disconnectDevice({ silent: true });
    state.serial = "error";
    renderConnectionState();
    showConnectError(`${s.connectErrorPrefix}${getErrorMessage(error)}`);
  }
}

async function disconnectDevice(options?: { silent?: boolean }) {
  if (state.modalStep > 0) closeModal();
  stopConsoleReader();
  rejectWaiters(state.statusWaiters, new Error("Disconnected"));
  rejectWaiters(state.readyWaiters, new Error("Disconnected"));
  state.statusWaiters = [];
  state.readyWaiters = [];
  state.inConsoleMode = false;
  state.readyIp = null;
  state.provision = "idle";
  state.flash = "idle";
  state.reconnectingPort = false;
  renderReconnectPrompt(false);

  const loader = state.loader;
  state.loader = null;
  state.device = { chipName: null, chipKey: null, chipRevision: null, flashSizeMb: null, psramSizeMb: null };
  state.detectedConsoleOutput = null;
  state.serial = "idle";
  renderConnectionState();
  refreshBoards();

  if (state.activeTab === "console") switchTab("flash");
  if (loader) {
    try { await loader.disconnect(); } catch (error) { if (!options?.silent) console.warn(error); }
  }
}

// ── Flash firmware (per-partition) ───────────────────────────────────────────

async function flashSelectedFirmware() {
  const selected = getSelectedFirmware();
  const record = getSelectedConsoleRecord();
  if (!selected || !record) { renderActionState(); return; }
  if (!state.loader || !state.device.chipKey) { renderActionState(); return; }
  if (state.inConsoleMode) {
    openModal();
    renderModalStep(1);
    showModalFlashError("Please disconnect and reconnect the device before flashing again.");
    setModalCloseable(true);
    return;
  }

  openModal();
  setModalCloseable(false);
  state.readyIp = null;
  state.progressLines = [];
  state.reconnectingPort = false;
  els.modalProgressLog.textContent = "";
  hideModalFlashResult();
  renderModalProgress(true);
  renderReconnectPrompt(false);

  try {
    const filesToFlash = getFilesToFlash(record);
    if (filesToFlash.length === 0) {
      throw new Error("No partition files selected for flashing.");
    }

    // Download all partition files
    state.flash = "downloading";
    updateConsoleTabState();
    updateModalProgress(s.downloadingPartitions, 0);
    addProgressLine(s.downloadingPartitions);

    const downloadedFiles: Array<{ offset: number; data: ArrayBuffer; name: string }> = [];
    for (let i = 0; i < filesToFlash.length; i++) {
      const { offset, url, name } = filesToFlash[i];
      const pctBase = Math.round((i / filesToFlash.length) * 100);
      updateModalProgress(s.downloadingPartitions, pctBase);

      const resp = await fetch(url);
      if (!resp.ok) throw new Error(`HTTP ${resp.status} downloading ${name}`);
      const data = await resp.arrayBuffer();
      downloadedFiles.push({ offset, data, name });
    }
    updateModalProgress(s.downloadingPartitions, 100);

    // Flash
    state.flash = "flashing";
    const loader = state.loader;
    let fastBaudForFlash = false;
    if (loader.IS_STUB) {
      try {
        await loader.setBaudrate(FLASH_UART_BAUD_FAST);
        fastBaudForFlash = true;
      } catch (baudErr) {
        addProgressLine(`UART speed-up skipped: ${getErrorMessage(baudErr)}`);
      }
    }

    try {
      if (state.flashMode === "full-erase" || state.flashMode === "erase-select-partitions") {
        updateModalProgress(s.erasingFlash, 0);
        addProgressLine(s.erasingFlash);
        await (loader as ESPStubLoader).eraseFlash();
        updateModalProgress(s.erasingFlash, 100);
      }

      const totalBytes = downloadedFiles.reduce((sum, f) => sum + f.data.byteLength, 0);
      let writtenBefore = 0;

      for (const file of downloadedFiles) {
        const msg = s.flashingPartition
          .replace("{name}", file.name)
          .replace("{offset}", `0x${file.offset.toString(16)}`);
        addProgressLine(msg);

        await flashBinaryChunk(loader, file.data, file.offset, (segPct) => {
          const writtenNow = Math.round((file.data.byteLength * segPct) / 100);
          const totalPct = Math.round(((writtenBefore + writtenNow) / totalBytes) * 100);
          updateModalProgress(s.writingFlash, totalPct);
        });
        writtenBefore += file.data.byteLength;
        updateModalProgress(s.writingFlash, Math.round((writtenBefore / totalBytes) * 100));
      }
    } finally {
      if (fastBaudForFlash && loader.IS_STUB) {
        try { await loader.setBaudrate(FLASH_UART_BAUD_ROM); }
        catch (restoreErr) { addProgressLine(`UART restore failed: ${getErrorMessage(restoreErr)}`); }
      }
    }

    state.flash = "flashed";
    showModalFlashSuccess(s.flashSuccess);
    await beginPostFlashFlow();
  } catch (error) {
    state.flash = "error";
    const message = getErrorMessage(error);
    if (state.provision === "error" || state.provision === "probing_wifi") {
      showModalFlashError(message);
    } else {
      showModalFlashError(`${s.flashError}${message}`);
    }
    setModalCloseable(true);
  } finally {
    renderActionState();
  }
}

function getFilesToFlash(record: FirmwareRecord) {
  const isFullErase = state.flashMode === "full-erase";
  const files: Array<{ offset: number; url: string; name: string }> = [];

  for (const [offsetStr, urlPath] of Object.entries(record.flash_files)) {
    if (!isFullErase && !state.selectedPartitions.has(offsetStr)) continue;

    const offsetNum = Number.parseInt(offsetStr, 0);
    if (!Number.isFinite(offsetNum) || offsetNum < 0) continue;

    const fileName = urlPath.split("/").pop() ?? urlPath;
    const url = resolveFileUrl(urlPath);
    files.push({ offset: offsetNum, url, name: fileName });
  }

  files.sort((a, b) => a.offset - b.offset);
  return files;
}

function resolveFileUrl(urlPath: string): string {
  if (urlPath.startsWith("http://") || urlPath.startsWith("https://")) return urlPath;
  if (state.selectedVersion && state.selectedVersion !== "master" && FIRMWARE_SITE_URL) {
    return `${FIRMWARE_SITE_URL}${urlPath}`;
  }
  return urlPath;
}

async function flashBinaryChunk(
  loader: ESPLoader,
  binary: ArrayBuffer,
  flashOffset: number,
  onProgress: (pct: number) => void,
) {
  let lastWritePct = 0;
  try {
    await loader.flashData(
      binary,
      (written, total) => {
        const pct = total > 0 ? Math.round((written / total) * 100) : 0;
        lastWritePct = pct;
        onProgress(pct);
      },
      flashOffset,
      true,
    );
  } catch (flashError) {
    const isFinalizeTimeout =
      lastWritePct >= 100 && getErrorMessage(flashError).includes("Timed out waiting for packet");
    if (!isFinalizeTimeout) throw flashError;
    addProgressLine("Note: stub finalization timed out after write; continuing with hardware reset.");
    onProgress(100);
  }
}

// ── Post-flash flow ──────────────────────────────────────────────────────────

async function beginPostFlashFlow() {
  if (!state.loader) return;
  state.flash = "postflash";
  state.provision = "waiting_boot";
  updateConsoleTabState();
  updateModalProgress(s.waitingForDeviceInfo, 100);
  addProgressLine(s.waitingForDeviceInfo);
  await sleep(1000);

  const portChanged = await state.loader.enterConsoleMode();
  if (portChanged) {
    state.provision = "waiting_reconnect";
    renderReconnectPrompt(true);
    return;
  }
  renderReconnectPrompt(false);
  await continuePostFlashConsoleFlow();
}

async function reconnectDeviceAfterReset() {
  if (!state.loader || state.reconnectingPort) return;
  state.reconnectingPort = true;
  renderReconnectState();
  els.modalReconnectStatus.textContent = s.postFlashReconnectBusy;

  try {
    const port = await navigator.serial.requestPort();
    const nextLoader = await connectWithPort(port, logger);
    copyLoaderMetadata(state.loader, nextLoader);
    nextLoader.setConsoleMode(true);
    state.loader = nextLoader;
    state.detectedConsoleOutput = detectConsoleOutputFromLoader(nextLoader);
    els.modalReconnectStatus.textContent = s.postFlashReconnectSuccess;
    renderReconnectPrompt(false);
    await continuePostFlashConsoleFlow();
  } catch (error) {
    if (els.modalReconnectPrompt.hidden) {
      state.provision = "error";
      showModalFlashError(getErrorMessage(error));
      setModalCloseable(true);
    } else {
      els.modalReconnectStatus.textContent = `${s.postFlashReconnectError}${getErrorMessage(error)}`;
    }
  } finally {
    state.reconnectingPort = false;
    renderReconnectState();
  }
}

async function continuePostFlashConsoleFlow() {
  if (!state.loader) return;
  state.inConsoleMode = true;
  state.provision = "probing_wifi";
  renderConsoleSendState();
  updateConsoleTabState();
  await startConsoleReader();
  await sleep(5000);

  let status: WifiStatus | null = null;
  for (let attempt = 1; attempt <= WIFI_STATUS_PROBE_ATTEMPTS; attempt++) {
    const stage = s.wifiStatusProbeAttempt
      .replace("{current}", String(attempt))
      .replace("{total}", String(WIFI_STATUS_PROBE_ATTEMPTS));
    updateModalProgress(stage, 100);
    addProgressLine(stage);
    await sendConsoleCommand("wifi --status\n");
    status = await waitForWifiStatus(WIFI_STATUS_PROBE_WAIT_MS).catch(() => null);
    if (status) break;
    if (attempt < WIFI_STATUS_PROBE_ATTEMPTS) await sleep(WIFI_STATUS_PROBE_RETRY_GAP_MS);
  }

  if (!status) { state.provision = "error"; throw new Error(s.wifiProbeError); }
  if (status.connected && status.configured && status.ip) { handleReady(status.ip); return; }
  state.provision = "need_wifi";
  updateConsoleTabState();
  renderModalStep(2);
  els.modalWifiStatus.textContent = "";
}

async function submitWifiCredentials() {
  if (!state.loader || !state.inConsoleMode) return;
  const ssid = els.modalWifiSsid.value.trim();
  const password = els.modalWifiPassword.value;
  if (!ssid) { els.modalWifiSsid.focus(); return; }
  if (password.length > 0 && password.length < 8) {
    els.modalWifiPassword.focus();
    els.modalWifiPassword.reportValidity();
    els.modalWifiStatus.textContent = s.wifiPasswordLengthError;
    return;
  }

  state.provision = "connecting_wifi";
  els.modalWifiSubmitBtn.disabled = true;
  updateConsoleTabState();
  els.modalWifiStatus.textContent = s.wifiConnecting;

  try {
    await sendConsoleCommand(
      `wifi --set --ssid "${escapeConsoleArgument(ssid)}" --password "${escapeConsoleArgument(password)}" --apply\n`,
    );
    const pollTimer = window.setInterval(() => {
      void sendConsoleCommand("wifi --status\n").catch(() => undefined);
    }, 5000);
    try {
      const readyIp = await waitForReady(20000);
      handleReady(readyIp);
    } finally { window.clearInterval(pollTimer); }
  } catch (error) {
    state.provision = "need_wifi";
    els.modalWifiStatus.textContent = getErrorMessage(error) || s.wifiTimeoutError;
  } finally {
    els.modalWifiSubmitBtn.disabled = false;
    updateConsoleTabState();
  }
}

// ── Console reader ───────────────────────────────────────────────────────────

async function startConsoleReader() {
  if (!state.loader || state.consoleReading || !state.loader.port.readable) return;
  state.consoleReader = state.loader.port.readable.getReader();
  state.consoleReading = true;
  const decoder = new TextDecoder();

  void (async () => {
    try {
      while (state.consoleReading && state.consoleReader) {
        const { value, done } = await state.consoleReader.read();
        if (done) break;
        if (!value) continue;
        const text = decoder.decode(value, { stream: true });
        appendConsole(text);
        processConsoleText(text);
      }
    } catch (error) {
      appendConsole(`[console-error] ${getErrorMessage(error)}\n`);
    } finally {
      state.consoleReading = false;
      if (state.consoleReader) {
        try { state.consoleReader.releaseLock(); } catch { /* ignore */ }
      }
      state.consoleReader = null;
    }
  })();
}

function stopConsoleReader() {
  state.consoleReading = false;
  if (state.consoleReader) void state.consoleReader.cancel().catch(() => undefined);
}

function processConsoleText(text: string) {
  state.consoleLineBuffer += text;
  const parts = state.consoleLineBuffer.split(/\r?\n/);
  state.consoleLineBuffer = parts.pop() ?? "";
  for (const rawLine of parts) {
    const line = rawLine.trim();
    if (!line) continue;
    const ip = extractReadyIp(line);
    if (ip) handleReady(ip);
    const status = parseWifiStatus(line);
    if (status) {
      notifyStatusWaiters(status);
      if (status.connected && status.configured && status.ip) handleReady(status.ip);
    }
  }
}

async function sendConsoleCommand(command: string) {
  if (!state.loader?.port.writable) throw new Error(s.wifiProbeError);
  const writer = state.loader.port.writable.getWriter();
  try { await writer.write(new TextEncoder().encode(command)); appendConsole(`> ${command}`); }
  finally { writer.releaseLock(); }
}

async function resetDeviceFromConsole() {
  if (!state.loader || !state.inConsoleMode) return;
  try {
    if (!state.loader.isConsoleResetSupported()) throw new Error(s.consoleResetUnsupported);
    await state.loader.resetInConsoleMode();
  } catch (error) { appendConsole(`[error] ${getErrorMessage(error)}\n`); }
}

function waitForWifiStatus(timeoutMs: number) {
  return new Promise<WifiStatus>((resolve, reject) => {
    const timer = window.setTimeout(() => {
      state.statusWaiters = state.statusWaiters.filter((w) => w.timer !== timer);
      reject(new Error(s.wifiProbeError));
    }, timeoutMs);
    state.statusWaiters.push({ resolve, reject, timer });
  });
}

function waitForReady(timeoutMs: number) {
  if (state.readyIp) return Promise.resolve(state.readyIp);
  return new Promise<string>((resolve, reject) => {
    const timer = window.setTimeout(() => {
      state.readyWaiters = state.readyWaiters.filter((w) => w.timer !== timer);
      reject(new Error(s.wifiTimeoutError));
    }, timeoutMs);
    state.readyWaiters.push({ resolve, reject, timer });
  });
}

function notifyStatusWaiters(status: WifiStatus) {
  const waiters = [...state.statusWaiters];
  state.statusWaiters = [];
  for (const w of waiters) { window.clearTimeout(w.timer); w.resolve(status); }
}

function handleReady(ip: string) {
  if (state.readyIp === ip && state.provision === "ready") return;
  state.readyIp = ip;
  state.provision = "ready";
  updateConsoleTabState();
  const href = `http://${ip}/#start`;
  els.modalReadyLink.href = href;
  els.modalReadyLink.textContent = s.openDeviceBtn.replace("{ip}", ip);
  els.modalReadyTitle.textContent = s.wifiReadyTitle;
  els.modalReadyDesc.textContent = s.wifiReadyDesc;
  renderModalStep(3);
  setModalCloseable(true);
  const waiters = [...state.readyWaiters];
  state.readyWaiters = [];
  for (const w of waiters) { window.clearTimeout(w.timer); w.resolve(ip); }
}

// ── Progress helpers ─────────────────────────────────────────────────────────

function renderModalProgress(visible: boolean) {
  els.modalProgressWrap.classList.toggle("visible", visible);
}

function renderReconnectPrompt(visible: boolean) {
  els.modalReconnectPrompt.hidden = !visible;
  if (!visible) els.modalReconnectStatus.textContent = "";
  renderReconnectState();
}

function renderReconnectState() {
  const visible = !els.modalReconnectPrompt.hidden;
  els.modalReconnectBtn.disabled = !visible || state.reconnectingPort || !("serial" in navigator);
}

function updateModalProgress(stage: string, percent: number) {
  els.modalProgressStage.textContent = stage;
  const clamped = Math.max(0, Math.min(100, percent));
  els.modalProgressPct.textContent = `${clamped}%`;
  els.modalProgressBar.style.width = `${clamped}%`;
}

function addProgressLine(line: string) {
  state.progressLines.push(line);
  if (state.progressLines.length > 60) state.progressLines = state.progressLines.slice(-60);
  els.modalProgressLog.textContent = state.progressLines.join("\n");
  els.modalProgressLog.scrollTop = els.modalProgressLog.scrollHeight;
}

function showModalFlashError(message: string) {
  els.modalFlashResult.textContent = message;
  els.modalFlashResult.className = "modal-flash-result error visible";
}

function showModalFlashSuccess(message: string) {
  els.modalFlashResult.textContent = message;
  els.modalFlashResult.className = "modal-flash-result success visible";
}

function hideModalFlashResult() {
  els.modalFlashResult.textContent = "";
  els.modalFlashResult.className = "modal-flash-result";
}

// ── Console helpers ──────────────────────────────────────────────────────────

function appendConsole(text: string) {
  state.consoleText += text;
  if (state.consoleText.length > 120000) state.consoleText = state.consoleText.slice(-120000);
  renderConsole();
}

function renderConsole() {
  if (!state.consoleText) {
    els.consoleOutput.textContent = "";
    els.consoleOutput.appendChild(els.consoleEmpty);
  } else {
    els.consoleOutput.textContent = state.consoleText;
    els.consoleOutput.scrollTop = els.consoleOutput.scrollHeight;
  }
  if (!state.consoleText) {
    els.modalTerminalOutput.textContent = "";
    els.modalTerminalOutput.appendChild(els.modalTerminalEmpty);
  } else {
    els.modalTerminalOutput.textContent = state.consoleText;
    if (els.modalTerminalDetails.open) {
      els.modalTerminalOutput.scrollTop = els.modalTerminalOutput.scrollHeight;
    }
  }
}

function renderConsoleSendState() {
  const canSend =
    state.serial === "connected" &&
    state.inConsoleMode &&
    state.flash !== "downloading" &&
    state.flash !== "flashing" &&
    state.provision !== "connecting_wifi";
  els.consoleSendInput.disabled = !canSend;
  els.consoleSendBtn.disabled = !canSend;
}

async function sendConsoleInput() {
  const input = els.consoleSendInput.value.trim();
  if (!input) return;
  els.consoleSendInput.value = "";
  await sendConsoleCommand(`${input}\n`).catch((error) => {
    appendConsole(`[send-error] ${getErrorMessage(error)}\n`);
  });
}

// ── Show/hide helpers ────────────────────────────────────────────────────────

function showConnectError(message: string) {
  els.connectError.textContent = message;
  els.connectError.classList.add("visible");
}

function clearConnectError() {
  els.connectError.textContent = "";
  els.connectError.classList.remove("visible");
}

// ── Selection helpers ────────────────────────────────────────────────────────

function getSelectedFirmware() {
  const boardId = state.selectedBoardId;
  if (!boardId) return null;
  return (
    state.visibleBoards.find(
      ({ chipKey, brandKey, boardKey }) => makeBoardId(chipKey, brandKey, boardKey) === boardId,
    ) ?? null
  );
}

function getSelectedConsoleRecord(): FirmwareRecord | null {
  const selected = getSelectedFirmware();
  const co = state.selectedConsoleOutput;
  if (!selected || !co) return null;
  const boards = getCurrentBoardsTree();
  return boards[selected.chipKey]?.[selected.brandKey]?.[selected.boardKey]?.[co] ?? null;
}

function currentSelectionStillVisible() {
  if (!state.selectedBoardId) return true;
  return state.visibleBoards.some(
    ({ chipKey, brandKey, boardKey }) => makeBoardId(chipKey, brandKey, boardKey) === state.selectedBoardId,
  );
}

function consoleOutputLabel(co: string) {
  return co === "unknown" ? "unknown" : co;
}

function detectConsoleOutputFromLoader(loader: ESPLoader): DetectedConsoleOutput {
  const info = getPortInfo(loader);
  if (info?.usbVendorId === ESP_USB_JTAG_VID && info?.usbProductId === ESP_USB_JTAG_PID) return "JTAG";
  return "UART";
}

function getPortInfo(loader: ESPLoader): SerialPortInfo | null {
  const port = (loader as ESPLoader & { port?: { getInfo?: () => SerialPortInfo } }).port;
  if (!port?.getInfo) return null;
  try { return port.getInfo(); } catch { return null; }
}

// ── Utility ──────────────────────────────────────────────────────────────────

function makeBoardId(chipKey: string, brandKey: string, boardKey: string) {
  return `${chipKey}:${brandKey}:${boardKey}`;
}

function parseHexAddress(value: string | undefined) {
  if (!value || !value.trim()) return null;
  const trimmed = value.trim();
  const isHex = /^0x[0-9a-f]+$/i.test(trimmed);
  const isDecimal = /^[0-9]+$/.test(trimmed);
  if (!isHex && !isDecimal) return null;
  const parsed = Number.parseInt(trimmed, isHex ? 16 : 10);
  if (!Number.isFinite(parsed) || !Number.isSafeInteger(parsed) || parsed < 0) return null;
  return parsed;
}

function chipLabel(chipKey: string) {
  const parsed = parseChipKey(chipKey);
  const upper = parsed.baseChipKey.toUpperCase();
  const label = upper
    .replace("ESP32S", "ESP32-S")
    .replace("ESP32C", "ESP32-C")
    .replace("ESP32P", "ESP32-P")
    .replace("ESP32H", "ESP32-H");
  return parsed.rev == null ? label : `${label} (Rev ${parsed.rev})`;
}

function parseChipKey(chipKey: string) {
  const [baseChipKey, revPart] = chipKey.split("|", 2);
  const revMatch = revPart?.match(/^rev(\d+)$/i);
  return { baseChipKey, rev: revMatch ? Number.parseInt(revMatch[1], 10) : null };
}

function normalizeChipKey(chipName: string | null) {
  if (!chipName) return null;
  return chipName.toLowerCase().replace(/[^a-z0-9]/g, "");
}

function parseFlashSize(value: string | null) {
  if (!value) return null;
  const match = value.match(/^(\d+)(KB|MB)$/i);
  if (!match) return null;
  const amount = Number(match[1]);
  return match[2].toUpperCase() === "KB" ? amount / 1024 : amount;
}

function formatSizeRequirement(sizeMb: number | null | undefined) {
  if (sizeMb == null) return s.psramUnknown;
  return `${sizeMb} MB`;
}

function formatChipRevision(revision: number | null | undefined) {
  if (revision == null) return null;
  return revision >= 300 ? `v${Math.floor(revision / 100)}` : `v${revision}`;
}

function parseWifiStatus(line: string): WifiStatus | null {
  if (!line.includes("CMD_WIFI:") || !line.includes("cmd=status") || !line.includes("ok=1")) return null;
  const connectedMatch = line.match(/sta_connected=(\d)/);
  const configuredMatch = line.match(/sta_configured=(\d)/);
  const ipMatch = line.match(/sta_ip=([0-9.\-]+)/);
  if (!connectedMatch || !configuredMatch || !ipMatch) return null;
  const ip = isIpv4(ipMatch[1]) ? ipMatch[1] : null;
  return { connected: connectedMatch[1] === "1", configured: configuredMatch[1] === "1", ip };
}

function extractReadyIp(line: string) {
  const match = line.match(/esp_netif_handlers:\s+sta ip:\s+(\d+\.\d+\.\d+\.\d+)/);
  return match?.[1] && isIpv4(match[1]) ? match[1] : null;
}

function isIpv4(value: string) {
  return /^(25[0-5]|2[0-4]\d|1?\d?\d)(\.(25[0-5]|2[0-4]\d|1?\d?\d)){3}$/.test(value);
}

function escapeConsoleArgument(value: string) {
  return value.replace(/\\/g, "\\\\").replace(/"/g, '\\"');
}

function copyLoaderMetadata(source: ESPLoader, target: ESPLoader) {
  target.chipName = source.chipName;
  target.chipFamily = source.chipFamily;
  target.chipRevision = source.chipRevision;
  target.chipVariant = source.chipVariant;
  target.flashSize = source.flashSize;
  target._isUsbJtagOrOtg = source._isUsbJtagOrOtg;
}

function rejectWaiters<T>(waiters: Waiter<T>[], reason: Error) {
  for (const w of waiters) { window.clearTimeout(w.timer); w.reject(reason); }
}

function getErrorMessage(error: unknown) {
  return error instanceof Error ? error.message : String(error);
}

function escapeHtml(text: string) {
  return text.replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;").replaceAll("'", "&#39;");
}

function sleep(ms: number) {
  return new Promise<void>((resolve) => { window.setTimeout(resolve, ms); });
}
