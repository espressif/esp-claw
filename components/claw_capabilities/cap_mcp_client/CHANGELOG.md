# MCP Client 改动记录

## 2026-05-05 — Streamable HTTP 支持 + Session 管理修复

### 背景
MCP Client 之前通过 `mcp/$smart` 端点连接 MCPHub，但存在 session ID 提取失败导致 `tools/list` 返回 HTTP 400 的问题。经调试发现根因是响应头 `mcp-session-id` 全小写，代码用驼峰 `Mcp-Session-Id` 比较。

### 修改列表

#### `src/cap_mcp_client_core.c`

| 改动 | 说明 |
|------|------|
| **Session 提取修复** | `HTTP_EVENT_ON_HEADER` 中加小写 `"mcp-session-id"` 匹配，服务器返回 header 是全小写 |
| **SSE 解析简化** | 去掉双循环计数+合并逻辑，改用单 `data:` 行直接提取 JSON，避免堆元数据破坏导致 `tlsf_free` 断言崩溃 |
| **ON_FINISH 空终止** | 在 `HTTP_EVENT_ON_FINISH` 中加 `buf->data[buf->len] = '\0'` 兜底 |
| **perform 后空终止** | `esp_http_client_perform` 返回后显式空终止，双重保险 |
| **响应缓冲区 8KB→64KB** | MCPHub 统一端点 `/mcp` 返回 37KB+ 工具列表，原 8KB 严重不足 |
| **日志分级** | `[HEADER]`/`[REQ]`/`[RESP]Body`/`[RPC]`/`[SSE]JSON` 降级为 `ESP_LOGD`；保留 `[SESSION]`/`[INIT]`/`[LIST]`/HTTP status 为 `ESP_LOGI` |

#### `src/cap_mcp_client_internal.h`

| 改动 | 说明 |
|------|------|
| **默认端点** | `CAP_MCP_DEFAULT_ENDPOINT` 从 `"mcp_server"` 改为 `"mcp"`，匹配 MCPHub 标准端点 |

#### `src/cap_mcp_client.c`

| 改动 | 说明 |
|------|------|
| **Schema 补全 description** | `mcp_list_tools` 和 `mcp_call_tool` 的 `input_schema_json` 每个属性加 `description` 字段，LLM 能理解参数含义 |
| **工具描述增强** | `.description` 补充使用示例，如 `"Pass server_url (e.g. http://host:port) and optionally auth_token..."` |

#### `app_capabilities.c`（在 `components/common/app_claw/`）

| 改动 | 说明 |
|------|------|
| **LLM 可见性** | `cap_mcp_client` 组 `llm_visible` 从 `false` 改为 `true`，DeepSeek 才能看到这三个工具 |

### 协议层改动

| 协议 | 之前 | 之后 |
|------|------|------|
| 端点 | `POST /mcp/$smart`（智能路由） | `POST /mcp`（MCPHub 统一端点） |
| Session | 响应头 `Mcp-Session-Id`（驼峰，不匹配） | `mcp-session-id`（小写，匹配） |
| 缓冲区 | 8KB（截断 37KB 响应） | 64KB（完整接收） |
| 传输 | Streamable HTTP（已支持） | 不变，SSE/JSON 自动检测 |

### 遗留问题

- `tools/call` 在 MCPHub 统一端点 `/mcp` 下需要通过工具名前缀匹配服务器，形如 `网页抓取-firecrawl_scrape`
- 也可走 `/mcp/{server}` 端点直接选服务器
