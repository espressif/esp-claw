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
