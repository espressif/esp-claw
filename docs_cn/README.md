# Claw Agent 系统架构文档

> 本目录为 esp-claw 项目的逆向工程文档，目标是完整描述 Claw Agent 的设计与实现，使其可以在 PC 架构操作系统上完全还原。所有文档使用中文编写。

## 文档目录

| 文件 | 内容 |
|------|------|
| [01_系统架构总览.md](01_系统架构总览.md) | 宏观架构、组件关系图、数据流 |
| [02_核心Agent循环.md](02_核心Agent循环.md) | Agent 主循环状态机、请求处理流程、迭代机制 |
| [03_能力系统设计.md](03_能力系统设计.md) | Capability 注册/分组/生命周期/LLM可见性 |
| [04_内存系统设计.md](04_内存系统设计.md) | 长期记忆存储、检索算法、压缩策略 |
| [05_事件路由系统.md](05_事件路由系统.md) | 事件结构、规则匹配、动作执行 |
| [06_技能系统设计.md](06_技能系统设计.md) | Skill 文档格式、注册、会话激活 |
| [07_LLM后端抽象.md](07_LLM后端抽象.md) | 后端 vtable 接口、协议适配层 |
| [08_移植指南_ESP32到PC.md](08_移植指南_ESP32到PC.md) | FreeRTOS → POSIX、esp_err_t → 标准错误、VFS 等替换方案 |
| [09_数据结构参考.md](09_数据结构参考.md) | 所有核心数据结构定义与字段说明 |
| [10_四维架构分析.md](10_四维架构分析.md) | Agent = LLM × 工具 × 记忆 × 控制流 的权衡分析与洞察 |
| [11_深度机制分析.md](11_深度机制分析.md) | LLM会话架构、提示词构建、容错、工具调度、记忆注入、控制流、角色身份 |
| [12_初始化顺序与依赖图.md](12_初始化顺序与依赖图.md) | 各模块 init/start/stop 顺序、依赖约束、app_claw 启动序列 |
| [13_并发模型与线程安全.md](13_并发模型与线程安全.md) | FreeRTOS 任务清单、同步原语、线程安全 API 边界 |
| [14_MCP协议适配层.md](14_MCP协议适配层.md) | MCP 客户端（工具发现/调用）与服务端（能力暴露）实现 |
| [15_多模态视觉管道.md](15_多模态视觉管道.md) | 图像处理、base64 编码、OpenAI/Anthropic 视觉消息格式 |
| [16_调度器实现.md](16_调度器实现.md) | once/interval/cron 实现、状态机、NVS 持久化、事件联动 |
| [17_IM平台适配层.md](17_IM平台适配层.md) | Telegram/微信/飞书/QQ 适配、去重机制、附件下载 |
| [18_观测性与调试接口.md](18_观测性与调试接口.md) | stage_note、completion_observer、日志格式参考 |
| [19_会话策略深度分析.md](19_会话策略深度分析.md) | CHAT/TRIGGER/GLOBAL/EPHEMERAL/NOSAVE 语义与记忆系统交互 |
| [20_Lua脚本引擎.md](20_Lua脚本引擎.md) | VM 生命周期、35 个 lua_modules、capability 调用约定 |
| [21_安全与访问控制.md](21_安全与访问控制.md) | caller 枚举语义、request_gate、LLM 可见性五条件 |
| [22_资源预算与容量规划.md](22_资源预算与容量规划.md) | 任务栈/队列/缓冲区常量表、PC 移植放大建议 |

## 项目简介

**esp-claw** 是运行在 ESP32 上的嵌入式 AI Agent 框架。它以 LLM（大语言模型）为推理核心，通过"能力（Capability）"机制向 LLM 暴露工具，通过"事件路由（Event Router）"将外部输入（IM 消息、定时器、传感器等）转发给 Agent，并具备持久化的长期记忆和可动态激活的技能系统。

## 核心模块速览

```
claw_modules/
├── claw_core        ← Agent 主循环、LLM 调用、会话持久化
├── claw_cap         ← Capability 注册表与调用分发
├── claw_event_router ← 事件队列与规则引擎
├── claw_skill       ← 技能文档注册表
├── claw_memory      ← 长期记忆 CRUD + 自动提取
└── claw_ramfs       ← RAM 文件系统（可移植为内存文件系统）

claw_capabilities/
├── cap_im_platform  ← IM 平台适配（TG/微信/QQ/飞书）
├── cap_scheduler    ← 定时器/Cron 调度
├── cap_router_mgr   ← 路由规则管理（可被 LLM 调用）
├── cap_skill_mgr    ← 技能激活/停用（可被 LLM 调用）
├── cap_session_mgr  ← 会话管理
├── cap_memory (内嵌于 claw_memory) ← 记忆操作（可被 LLM 调用）
├── cap_mcp_client   ← MCP 协议客户端
├── cap_mcp_server   ← MCP 协议服务端
├── cap_lua          ← Lua 脚本执行
├── cap_web_search   ← 网络搜索
├── cap_http_request ← 通用 HTTP 请求
├── cap_llm_inspect  ← 图像分析
├── cap_files        ← 文件读写
├── cap_time         ← 时间查询
└── cap_cli          ← 控制台命令
```
