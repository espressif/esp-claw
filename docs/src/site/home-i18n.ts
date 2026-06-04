import type { Lang } from "./locales";

import ChatCodingWebm from "@/assets/features/chat-coding.webm?url";
import EventWebm from "@/assets/features/event.webm?url";
import LocalLuaWebm from "@/assets/features/locallua.webm?url";
import McpWebm from "@/assets/features/mcp.webm?url";
import MemoryWebm from "@/assets/features/memory.webm?url";
import MultiWebm from "@/assets/features/multi.webm?url";

import ChatCodingWebmEn from "@/assets/features/chat-coding-en.webm?url";
import EventWebmEn from "@/assets/features/event-en.webm?url";
import LocalLuaWebmEn from "@/assets/features/locallua-en.webm?url";
import McpWebmEn from "@/assets/features/mcp-en.webm?url";
import MemoryWebmEn from "@/assets/features/memory-en.webm?url";
import MultiWebmEn from "@/assets/features/multi-en.webm?url";

import VideoBV1kookBYEi7Thumbnail from "@/assets/video-thumbnail/bilibili-BV1kookBYEi7.webp?url";
import VideoS0czt7Ld5fwThumbnail from "@/assets/video-thumbnail/youtube-S0czt7Ld5fw.webp?url";
import VideoBV1UX9rBiEZ9TThumbnail from "@/assets/video-thumbnail/bilibili-BV1UX9rBiEZ9.webp?url";
import VideoBV1QiohBEEt5Thumbnail from "@/assets/video-thumbnail/bilibili-BV1QiohBEEt5.webp?url";
import VideoYoutubeCo8AjEXiu8AThumbnail from "@/assets/video-thumbnail/youtube-co8AjEXiu8A.webp?url";
import VideoYoutubeD5VSzg2dSYwThumbnail from "@/assets/video-thumbnail/youtube-d5VSzg2dSYw.webp?url";

export interface TerminalLine {
  cls: "prompt" | "output" | "info" | "warn";
  text: string;
}


export interface FeatAndDemoItem {
  title: string;
  desc: string;
  video: string;
  posterTime: number;
}

export interface HighlightItem {
  title: string;
  subtitle: string;
  detail: string;
}

export interface VideoCaseItem {
  thumbnail: string;
  title: string;
  videoUrl: string;
}

export interface HomeContent {
  metaTitle: string;
  metaDescription: string;
  terminalTitle: string;
  asciinemaLabel: string;
  heroBadge: string;
  heroDescription: string; // HTML string
  heroEspressifPrefix: string;
  flashOnline: string;
  getStartedDocs: string;
  ctaStripLine: string;
  ctaPillDoc: string;
  finalCtaHeading: string;
  getStarted: string;
  highlights: HighlightItem[];
  whatsMissingAfterIoTTitle: string;
  whatsMissingAfterIoTP1: string;
  whatsMissingAfterIoTP2: string;
  techAdvantagesTitle: string;
  featAndDemos: FeatAndDemoItem[];
  videoCasesTitle: string;
  videoCases: VideoCaseItem[];
}

const en: HomeContent = {
  metaTitle: "ESP-Claw — Chat-as-Coding Edge Agent Framework for IoT | by Espressif",
  metaDescription:
    "ESP-Claw, Espressif's \"Chat-as-Coding\" Edge Agent Framework for IoT.",
  terminalTitle: "ESP-Claw — interactive demo",
  asciinemaLabel: "Asciinema Player Demo",
  heroBadge: "Local · Chat Coding · Edge Agent",
  heroDescription: "<b>Chat Coding</b> Edge AI Agent Framework for <b>IoT</b>",
  heroEspressifPrefix: "by",
  flashOnline: "Flash Online",
  getStartedDocs: "Get Started Docs",
  ctaStripLine: "Try it today — no cloud required",
  ctaPillDoc: "Get Started Document",
  finalCtaHeading: "Launch Your Agent Now",
  getStarted: "Get Started",
  highlights: [
    {
      title: "Chat as Creation",
      subtitle: "LLM + Lua Hybrid Engine",
      detail: "Define device behavior through natural conversation. LLM handles dynamic decisions; confirmed scripts solidify into local Lua rules that execute deterministically—even offline.",
    },
    {
      title: "Millisecond Response",
      subtitle: "Event-Driven Architecture",
      detail: "Devices react to real-time events instead of polling. A local event bus drives Lua rules for sensors and triggers, guaranteeing deterministic millisecond-latency response on or offline.",
    },
    {
      title: "Plug and Play",
      subtitle: "MCP Unifies Everything",
      detail: "Devices self-declare capabilities via MCP, replacing per-device adapters. ESP-Claw acts as both MCP Server and Client—exposing hardware to agents while calling external services.",
    },
    {
      title: "Grows with You",
      subtitle: "On-Chip Private Memory",
      detail: "Structured long-term memory lives entirely on-chip. Preferences and routines auto-extracted from conversations and events never leave the device. Tag-based retrieval stays efficient within MCU constraints.",
    },
  ],
  whatsMissingAfterIoTTitle: "What's missing after IoT?",
  whatsMissingAfterIoTP1: "Traditional IoT stops at simple connectivity. Devices can connect but not think; execute but not decide. Heavy cloud reliance and rigid rules keep them purely passive.",
  whatsMissingAfterIoTP2: "ESP-Claw pushes the Agent Runtime directly to the edge, transforming ESP chips from passive \"execution nodes\" into active \"decision centers\" that perceive, reason, and act locally—breaking free from cloud dependency.",
  techAdvantagesTitle: "Technological Advantages of ESP-Claw",
  featAndDemos: [
    {
      title: "No Programming Required, \"Chat as Creation\"",
      desc: "Generate various driver codes through natural language descriptions. Just send requirements via IM.",
      video: ChatCodingWebmEn,
      posterTime: 10,
    },
    {
      title: "Complex Applications, Handled with Ease",
      desc: "Combine multiple peripherals freely, orchestrated by LLM, to realize complex applications.",
      video: MultiWebmEn,
      posterTime: 16,
    },
    {
      title: "Stable Operation for Critical Tasks",
      desc: "Critical operations can be saved as Lua scripts. Reproducible execution ensures stable operation.",
      video: LocalLuaWebmEn,
      posterTime: 8,
    },
    {
      title: "Event-Driven, Proactive Perception",
      desc: "Adopts an event-driven architecture, balancing local swiftness and LLM flexibility for cloud-edge collaboration.",
      video: EventWebmEn,
      posterTime: 16,
    },
    {
      title: "Standard Interfaces, Smart Expansion",
      desc: "Compatible with standard MCP protocol, supporting both Client and Server modes.",
      video: McpWebmEn,
      posterTime: 7,
    },
    {
      title: "Understands Your Needs, Adapts to You",
      desc: "Implements a localized structured long-term memory system. Intelligently records user preferences and habits, saved locally.",
      video: MemoryWebmEn,
      posterTime: 15,
    },
  ],
  videoCasesTitle: "How They Use ESP-Claw",
  videoCases: [
    {
      thumbnail: VideoBV1kookBYEi7Thumbnail,
      title: "Create Games Simply by Chatting",
      videoUrl: "https://www.youtube.com/watch?v=Nk3JbfvSVdw",
    },
    {
      thumbnail: VideoS0czt7Ld5fwThumbnail,
      title: "Running AI Agent on ESP32 S3 Board",
      videoUrl: "https://www.youtube.com/watch?v=S0czt7Ld5fw",
    },
    {
      thumbnail: VideoYoutubeCo8AjEXiu8AThumbnail,
      title: "How to Raise an ESP32 Claw",
      videoUrl: "https://www.youtube.com/watch?v=co8AjEXiu8A",
    },
    {
      thumbnail: VideoYoutubeD5VSzg2dSYwThumbnail,
      title: "ESP-Claw on M5Stack StickS3 & CoreS3",
      videoUrl: "https://www.youtube.com/watch?v=d5VSzg2dSYw",
    }
  ],
};

const zhCn: HomeContent = {
  metaTitle: "ESP-Claw — 「聊天造物」物联网 AI 智能体框架 | 乐鑫出品",
  metaDescription:
    "ESP-Claw, 乐鑫推出的「聊天造物」物联网 AI 智能体框架。",
  terminalTitle: "ESP-Claw — 交互演示",
  asciinemaLabel: "Asciinema 播放器演示",
  heroBadge: "本地 · 聊天造物 · 边缘智能体",
  heroDescription: "「聊天造物」物联网 AI 智能体框架",
  heroEspressifPrefix: "来自",
  flashOnline: "在线烧录",
  getStartedDocs: "阅读文档",
  ctaStripLine: "立即体验 — 无需依赖云端",
  ctaPillDoc: "开始使用文档",
  finalCtaHeading: "立即启动你的智能体",
  getStarted: "开始使用",
  highlights: [
    {
      title: "聊天即造物",
      subtitle: "LLM + Lua 混合引擎",
      detail: "通过自然对话定义设备行为。LLM 动态决策，经确认的脚本固化为本地 Lua 规则，确定性执行，离线可运行。",
    },
    {
      title: "毫秒级响应",
      subtitle: "事件驱动架构",
      detail: "设备实时响应事件而非轮询。本地事件总线驱动 Lua 规则处理传感器与触发事件，保障毫秒级确定性响应，离线亦可。",
    },
    {
      title: "即插即用",
      subtitle: "MCP 协议统一万物",
      detail: "设备通过 MCP 自声明能力，取代逐一适配。ESP-Claw 同时作为 MCP Server 和 Client，向 Agent 暴露硬件并调用外部服务。",
    },
    {
      title: "越用越懂你",
      subtitle: "芯片端本地记忆",
      detail: "结构化长期记忆完全在芯片端运行。偏好与习惯从对话和事件中自动提取，永不离开设备。标签检索在 MCU 有限资源下高效召回。",
    },
  ],
  whatsMissingAfterIoTTitle: "万物互联之后，还缺什么？",
  whatsMissingAfterIoTP1: "传统 IoT 仅停留在连接层——设备能联网却不能思考，能执行却不能决策；高度依赖云端，使 IoT 始终停留在被动响应阶段。",
  whatsMissingAfterIoTP2: "ESP-Claw 将 Agent 运行时直接下沉至设备端，让 ESP 芯片化身为主动的“决策中心”——在本地完成感知、推理与决策的闭环，大幅减少云端依赖。",
  techAdvantagesTitle: "ESP-Claw 的技术优势",
  featAndDemos: [
    {
      title: "无须编程，「聊天造物」",
      desc: "通过语言描述即可生成各种驱动代码，只需通过 IM 发送需求",
      video: ChatCodingWebm,
      posterTime: 10,
    },
    {
      title: "复杂应用，得心应手",
      desc: "多个外设可自由组合，交由 LLM 调用，实现复杂应用",
      video: MultiWebm,
      posterTime: 16,
    },
    {
      title: "关键操作，稳定运作",
      desc: "关键操作可保存为 Lua，调用可复现，稳定运作",
      video: LocalLuaWebm,
      posterTime: 8,
    },
    {
      title: "事件驱动，主动感知",
      desc: "采用事件驱动架构，兼顾本地的迅捷与 LLM 的灵活，云边协同",
      video: EventWebm,
      posterTime: 16,
    },
    {
      title: "接口标准，智能扩展",
      desc: "兼容标准 MCP 协议，Client / Server",
      video: McpWebm,
      posterTime: 7,
    },
    {
      title: "懂你所需，随你而变",
      desc: "本地实现结构化长期记忆系统，智能记录用户偏好习惯，本地保存",
      video: MemoryWebm,
      posterTime: 15,
    },
  ],
  videoCasesTitle: "Ta 们在这样使用 ESP-Claw",
  videoCases: [
    {
      thumbnail: VideoBV1kookBYEi7Thumbnail,
      title: "沉浸式组装 | 只靠聊天就能生成游戏",
      videoUrl: "https://www.bilibili.com/video/BV1kookBYEi7",
    },
    {
      thumbnail: VideoS0czt7Ld5fwThumbnail,
      title: "在 ESP32-S3 开发板上运行 AI Agent",
      videoUrl: "https://www.bilibili.com/video/BV1Mi5s6dEy2",
    },
    {
      thumbnail: VideoBV1QiohBEEt5Thumbnail,
      title: "龙虾！香！ESP-Claw实战演示",
      videoUrl: "https://www.bilibili.com/video/BV1QiohBEEt5",
    },
    {
      thumbnail: VideoBV1UX9rBiEZ9TThumbnail,
      title: "乐鑫龙虾 ESP-Claw 专属开发套件介绍",
      videoUrl: "https://www.bilibili.com/video/BV1UX9rBiEZ9",
    }
  ],
};

const table: Record<Lang, HomeContent> = {
  en,
  "zh-cn": zhCn,
};

export function getHomeContent(lang: Lang): HomeContent {
  return table[lang] ?? table.en;
}
