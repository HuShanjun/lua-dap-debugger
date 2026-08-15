# asyncsocket 通用网络库设计（Server/Client + 多连接）

**日期：** 2026-08-15  
**状态：** 已实现  
**范围：** 将现有 DAP 专用 `asyncsocket` 演进为通用 Lua 异步 TCP 库：Server 多连接 + Client `connect`；Lua 为 Server/Connection 两层对象；底层 `poll_loop` 重写为通用核；`luadap` 一并切到新 C API。

**前置：**
- `docs/superpowers/specs/2026-08-15-asyncsocket-dap-design.md`（V1 单连接 listen，已实现）
- `docs/superpowers/specs/2026-08-15-luadap-cpp-dap-design.md`（luadap 直接用 `as_socket_*`）

## 目标

1. `require("asyncsocket")` 支持 **listen（多 client）** 与 **connect（出站）**。  
2. Lua API：**Server + Connection** 两层；主线程 `pump()`。  
3. C 核：单 poll 线程、多 fd、事件带 `conn_id`；**禁止在 poll 线程调 Lua**。  
4. **`luadap` 迁移**到新 C API；DAP 仍为单调试客户端语义；保留 disconnect 后继续 listen（F5 可重连）。  
5. 冒烟 + DAP 全回归通过。

## 已锁定决策

| 决策 | 选择 |
|------|------|
| 交付形态 | 演进现有 `asyncsocket` 模块（方案 1） |
| 连接能力 | Server 多连接 + Client（方案 2） |
| 与 luadap | 一并改切新 C API（方案 2） |
| Lua 对象模型 | Server + Connection 两层（方案 1） |
| 实现路线 | 重写 `poll_loop` 为通用多路 I/O 核（方案 A） |
| 旧 listen 上 `on_open` | 主 API 改为 `on_accept`（破坏性；模块版本可升至 0.3） |
| V1 listen 数量 | 单进程 **一个** `listen` |
| `srv:close()` | 只关 listen；已接受 connection **保留** |
| 字节语义 | 原始 chunk，不组帧 |

## 架构

```
Lua 业务 / 测试
  require("asyncsocket")
       │
       ▼
┌────────────── asyncsocket.dll ──────────────┐
│  Lua: Server / Connection userdata           │
│  C 核 (poll_loop 重写):                      │
│    poll 线程: listen / accept / connect I/O  │
│    多 connection fd + wakeup                 │
│    事件队列: accept/open/message/close       │
│  主线程: pump() → 回调                       │
└──────────────────────────────────────────────┘
         ▲ 同 C 核（静态链）
┌────────────── luadap.dll ──────────────────┐
│  DAP: listen + 当前唯一 client conn          │
└──────────────────────────────────────────────┘
```

### 里程碑

1. **通用库可测**：`listen` 多连接 + `connect` + 更新 smoke。  
2. **`luadap` 迁移** + DAP 全回归绿。

## Lua API

```lua
local as = require("asyncsocket")

as.pump()
as.sleep(seconds)

-- Server
local srv = as.listen(host, port)
srv:on_accept(function(conn)
  conn:on_message(function(chunk) end)
  conn:on_close(function() end)
  -- 入站已连通；on_open 可选
end)
srv:close()   -- 停 listen；已有 conn 保留

-- Client
local conn = as.connect(host, port)
conn:on_open(function() end)      -- TCP 连通
conn:on_message(function(chunk) end)
conn:on_close(function() end)
conn:send(bytes)                  -- 失败：luaL_error（与现 send 一致）
conn:close()
```

| 约定 | 值 |
|------|-----|
| `pump` | 全局，V1 单引擎 drain |
| 多 `connect` | 允许 |
| 多 `listen` | V1 不允许（第二次 `listen` 报错） |

## C 核

### 事件

| type | 含义 | 载荷 |
|------|------|------|
| `ACCEPT` | 入站连接已建立 | `conn_id` |
| `OPEN` | 出站 connect 完成 | `conn_id` |
| `MESSAGE` | 可读数据 | `conn_id` + bytes |
| `CLOSE` | 连接结束 | `conn_id` |

### 公开 C API（方向；实现时落在 `poll_loop.h`）

- 引擎：`as_engine_create` / `destroy` / `take_events`（或保留全局引擎 + `as_net_init`）  
- `as_listen(host, port)` / `as_connect(host, port)`  
- `as_conn_send(conn_id, data, len)` / `as_conn_close(conn_id)`  
- `as_server_close()`（仅 listen）  
- 事件释放：`as_events_free`

线程规则与 V1 相同：poll 线程只做 I/O 与入队；Lua/`luadap` 仅在主线程 `take_events` 后处理。

### 相对旧 `as_socket_*`

旧「单 `as_socket` + 至多 1 client」模型废弃；`luadap` 与 Lua 绑定全部改用新 API。迁移期可用薄适配，但目标是删掉单连接假设（含 accept 时拒第二连接的逻辑，改为多 slot）。

## luadap 迁移

1. `start`：`as_listen`；`update`：drain 事件。  
2. `ACCEPT`：若尚无 DAP client → 记为当前 `conn_id` 并 `client_open`；若已有 → **关闭新连接**（保持单调试客户端）。  
3. `MESSAGE`/`CLOSE`：仅处理当前 DAP `conn_id`。  
4. `send`：`as_conn_send(dap_conn_id, …)`。  
5. 软重置（disconnect/CLOSE）：清会话、卸 hook、`terminated`；**不** `as_server_close`；可再 `ACCEPT`。  
6. 进程级 teardown / `start` 重建：关 server + 引擎。

## 测试

| 测试 | 覆盖 |
|------|------|
| 更新 `test_asyncsocket_smoke` | `on_accept` + 单连接收发 |
| 新：多 client | 两连接并行 message |
| 新：`connect` | client ↔ server 互通 |
| 现有 DAP 套件 | handshake / BP / step / disconnect / reconnect / coro / … |

## 非目标

- UDP、TLS、WebSocket/HTTP 组帧  
- 多 listen / 多 poll 引擎  
- 在 poll 线程调用 Lua  
- 保留旧 `listen` 对象上的 `on_open`/`on_message` 主路径兼容（破坏性升级）

## 风险与缓解

| 风险 | 缓解 |
|------|------|
| `poll_loop` 重写回归面大 | 里程碑 ① 先绿通用测试，再迁 luadap |
| Lua 破坏性 API | README + 升 `_VERSION`；改 smoke |
| 多连接 fd 泄漏 | `CLOSE`/`conn:close`/`srv:close` 路径单测 |
| luadap 误绑第二 client | ACCEPT 时显式拒/关第二连接 |
| Windows connect 非阻塞 | `WSAEWOULDBLOCK` + `OPEN` 事件；失败 `CLOSE` |

## 验收清单

1. `listen` + 两个 client 同时收发成功  
2. `connect` 与 server 互通  
3. `pump` 仅主线程触发回调  
4. `luadap` DAP 回归（含 reconnect）全绿  
5. 文档标明 Server/Connection API 与版本号  
