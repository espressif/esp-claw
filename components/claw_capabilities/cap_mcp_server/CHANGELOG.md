# MCP Server 改动记录

## 2026-05-05 — 本轮无修改

`cap_mcp_server` 在本轮调试中未做任何代码修改。

### 相关上下文

MCP Server（`http://esp-claw.local:18791/mcp_server`）工作正常，服务端能力未受影响。

MCP Client 的调试和修复主要涉及：
- `cap_mcp_client` 组件（Session 提取、SSE 解析、缓冲区）
- `app_capabilities.c`（LLM 可见性配置）
