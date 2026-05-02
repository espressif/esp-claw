import type { Lang } from "../site/locales";

export type { Lang };

export interface Strings {
  pageTitle: string;
  pageSubtitle: string;

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
  chooseChipPlaceholder: string;
  chooseBrandPlaceholder: string;
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

  quickLinkNoHardwareTitle: string;
  quickLinkNoHardwareDesc: string;
  quickLinkBoardsTitle: string;
  quickLinkBoardsDesc: string;

  tabFlash: string;
  tabConsole: string;
  consoleTabDisabledHint: string;

  modalStep1Title: string;
  modalStep2Title: string;
  modalStep3Title: string;
  terminalLabel: string;
}

const en: Strings = {
  pageTitle: "Flash Online",
  pageSubtitle:
    "Connect a supported ESP board, flash the merged firmware in your browser, then finish Wi-Fi setup over UART.",

  connectBtn: "Connect",
  disconnectBtn: "Disconnect",
  connectingMsg: "Connecting…",
  notConnected: "No device connected",
  connectedTo: "Connected",
  webSerialUnsupported:
    "Web Serial API is not supported in this browser. Please use Chrome, Edge, or another Chromium-based browser.",
  connectErrorPrefix: "Connection failed: ",

  chooseChipLabel: "Choose Chip",
  chooseBrandLabel: "Choose Brand/Manufacturer/Series",
  chooseBoardLabel: "Choose Board",
  chooseChipPlaceholder: "Choose a chip",
  chooseBrandPlaceholder: "Choose a brand/manufacturer/series",
  boardAutoHint: "Compatible boards are filtered automatically after device detection.",
  selectedBoardLabel: "Selected board",
  noBrandSelected: "No brand/manufacturer/series selected",
  noBoardSelected: "No board selected",
  boardFlashMeta: "Flash",
  boardPsramMeta: "PSRAM",
  psramUnknown: "unknown",
  firmwareRequirementsLabel: "Firmware requirements",
  firmwareDescriptionLabel: "Firmware description",
  downloadFirmwareLocalLink: "Download firmware locally",

  downloadBtn: "Download Firmware",
  flashBtn: "Flash Firmware",
  flashBtnDisabledNoDevice: "Connect a device first",
  flashBtnDisabledNoFirmware: "Select a board first",
  flashBtnDisabledNoMatch: "The selected board is not compatible with this device",
  actionReadyHint: "Select a compatible board, start flashing, and complete Wi-Fi setup.",

  noFirmwareTitle: "No compatible firmware found",
  noFirmwareDesc:
    "We couldn't find firmware matching your selected chip or the detected flash / PSRAM size.",
  viewSupportedBoardsBtn: "View supported chips & boards",

  progressLabel: "Progress",
  downloadingFirmware: "Downloading firmware…",
  writingFlash: "Writing firmware…",
  waitingForDeviceInfo: "Waiting for device info…",
  flashSuccess: "Flash complete. Waiting for the device to boot…",
  flashError: "Flash failed: ",
  postFlashReconnectTitle: "Reconnect serial after reset",
  postFlashReconnectDesc:
    "If you are flashing over ACM (JTAG), the serial port may briefly disconnect after reset. Click below and select the device again to continue.",
  postFlashReconnectBtn: "Reconnect Serial",
  postFlashReconnectBusy: "Waiting for you to select the device in the browser…",
  postFlashReconnectSuccess: "Serial reconnected. Continuing device setup…",
  postFlashReconnectError: "Reconnection failed: ",

  wifiSectionTitle: "Wi-Fi Setup",
  wifiPrompt:
    "The device is not connected to Wi-Fi yet. Enter SSID and password to continue.",
  wifiSameNetworkHint:
    "Make sure ESP-Claw and your browser are on the same network.",
  wifiSsidLabel: "Wi-Fi SSID",
  wifiPasswordLabel: "Wi-Fi Password",
  wifiPasswordLengthError: "Wi-Fi password must be empty or at least 8 characters long.",
  wifiSubmitBtn: "Connect Wi-Fi",
  wifiConnecting: "Trying to connect to Wi-Fi…",
  wifiProbeError:
    "Unable to communicate with the device. Check whether JTAG and UART are wired correctly. If the device is still in download mode, press RESET manually.",
  wifiTimeoutError: "Wi-Fi connection timed out. Please check the credentials and try again.",
  wifiReadyTitle: "Device is online",
  wifiReadyDesc: "Open the device web setup page to continue configuration.",
  openDeviceBtn: "Open http://{ip}/#start",

  consoleToggleOpen: "Console",
  consoleToggleClose: "Console",
  consoleTitle: "UART Console",
  consoleClearBtn: "Clear",
  consoleResetBtn: "RESET",
  consoleResetUnsupported: "RESET is not supported for the current serial mode.",
  consoleWaiting: "Console output will appear here after the device enters firmware mode.",
  consoleSendBtn: "Send",
  consoleSendPlaceholder: "Type a command…",
  consoleCloseBtn: "✕",

  deviceChipLabel: "Chip",
  deviceRevisionLabel: "Revision",
  deviceFlashLabel: "Flash",
  devicePsramLabel: "PSRAM",

  quickLinkNoHardwareTitle: "No hardware yet?",
  quickLinkNoHardwareDesc: "Assemble your ESP-Claw board",
  quickLinkBoardsTitle: "Have other ESP boards?",
  quickLinkBoardsDesc: "View supported board list",

  tabFlash: "Flash Online",
  tabConsole: "Console",
  consoleTabDisabledHint: "Connect a device and complete all setup first",

  modalStep1Title: "Download & Flash",
  modalStep2Title: "Wi-Fi Setup",
  modalStep3Title: "Wi-Fi Connected",
  terminalLabel: "Terminal",
};

const zhCn: Strings = {
  pageTitle: "在线烧录",
  pageSubtitle:
    "连接支持的 ESP 开发板，在浏览器内烧录合并固件，并通过 UART 完成 Wi‑Fi 初始化。",

  connectBtn: "连接",
  disconnectBtn: "断开",
  connectingMsg: "连接中…",
  notConnected: "未连接设备",
  connectedTo: "已连接",
  webSerialUnsupported:
    "当前浏览器不支持 Web Serial API，请使用 Chrome、Edge 或其他基于 Chromium 的浏览器。",
  connectErrorPrefix: "连接失败：",

  chooseChipLabel: "选择芯片",
  chooseBrandLabel: "选择品牌/生产商/开发版系列",
  chooseBoardLabel: "选择开发板",
  chooseChipPlaceholder: "请选择芯片",
  chooseBrandPlaceholder: "请选择品牌/生产商/开发版系列",
  boardAutoHint: "连接设备后会根据芯片、Flash 和 PSRAM 自动筛选兼容开发板。",
  selectedBoardLabel: "当前开发板",
  noBrandSelected: "尚未选择品牌/生产商/开发版系列",
  noBoardSelected: "尚未选择开发板",
  boardFlashMeta: "Flash",
  boardPsramMeta: "PSRAM",
  psramUnknown: "未知",
  firmwareRequirementsLabel: "固件要求",
  firmwareDescriptionLabel: "固件说明",
  downloadFirmwareLocalLink: "下载固件到本地",

  downloadBtn: "下载固件",
  flashBtn: "开始烧录",
  flashBtnDisabledNoDevice: "请先连接设备",
  flashBtnDisabledNoFirmware: "请先选择开发板",
  flashBtnDisabledNoMatch: "当前开发板与已连接设备不兼容",
  actionReadyHint: "请选择兼容的开发版，开始烧录，并完成 Wi-Fi 配置。",

  noFirmwareTitle: "未找到兼容固件",
  noFirmwareDesc:
    "当前所选芯片或检测到的 Flash / PSRAM 容量没有匹配的固件。",
  viewSupportedBoardsBtn: "查看支持的芯片和开发板",

  progressLabel: "进度",
  downloadingFirmware: "正在下载固件…",
  writingFlash: "正在烧录固件…",
  waitingForDeviceInfo: "正在等待设备信息…",
  flashSuccess: "烧录完成，正在等待设备启动…",
  flashError: "烧录失败：",
  postFlashReconnectTitle: "重启后重新连接串口",
  postFlashReconnectDesc:
    "如果你使用的是 ACM（JTAG）烧录，设备重启后串口会短暂断开。请点击下面的按钮，并在浏览器里重新选择设备以继续。",
  postFlashReconnectBtn: "重新连接串口",
  postFlashReconnectBusy: "请在浏览器弹窗中重新选择设备…",
  postFlashReconnectSuccess: "串口已重新连接，正在继续设备初始化…",
  postFlashReconnectError: "重新连接失败：",

  wifiSectionTitle: "Wi‑Fi 配置",
  wifiPrompt: "设备当前尚未连上 Wi‑Fi，请输入 SSID 和密码继续。",
  wifiSameNetworkHint: "请让 ESP-Claw 与当前浏览器连接到同一个网络。",
  wifiSsidLabel: "Wi‑Fi 名称 (SSID)",
  wifiPasswordLabel: "Wi‑Fi 密码",
  wifiPasswordLengthError: "Wi‑Fi 密码必须留空或至少 8 个字符。",
  wifiSubmitBtn: "连接 Wi‑Fi",
  wifiConnecting: "正在尝试连接 Wi‑Fi…",
  wifiProbeError:
    "未能与设备通信，请注意 JTAG 与 UART 口是否链接错误。如果设备仍在下载模式，请手动 RESET",
  wifiTimeoutError: "20 秒内未连接成功，请检查 Wi‑Fi 名称和密码后重试。",
  wifiReadyTitle: "设备已联网",
  wifiReadyDesc: "请打开设备网页继续后续配置。",
  openDeviceBtn: "打开 http://{ip}/#start",

  consoleToggleOpen: "控制台",
  consoleToggleClose: "控制台",
  consoleTitle: "UART 控制台",
  consoleClearBtn: "清空",
  consoleResetBtn: "RESET",
  consoleResetUnsupported: "当前串口模式不支持 RESET。",
  consoleWaiting: "设备进入固件运行模式后，这里会显示串口日志。",
  consoleSendBtn: "发送",
  consoleSendPlaceholder: "输入命令…",
  consoleCloseBtn: "✕",

  deviceChipLabel: "芯片",
  deviceRevisionLabel: "Revision",
  deviceFlashLabel: "Flash",
  devicePsramLabel: "PSRAM",

  quickLinkNoHardwareTitle: "还没有硬件？",
  quickLinkNoHardwareDesc: "组装 ESP-Claw 开发板",
  quickLinkBoardsTitle: "有其他 ESP 开发板？",
  quickLinkBoardsDesc: "查看支持的开发板列表",

  tabFlash: "在线烧录",
  tabConsole: "控制台",
  consoleTabDisabledHint: "请先连接设备并完成全部设置",

  modalStep1Title: "下载 / 烧录",
  modalStep2Title: "Wi-Fi 配置",
  modalStep3Title: "联网成功",
  terminalLabel: "终端",
};

const strings: Record<Lang, Strings> = {
  en,
  "zh-cn": zhCn,
};

export function getStrings(lang: Lang): Strings {
  return strings[lang] ?? strings.en;
}
