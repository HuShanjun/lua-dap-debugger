# luadap 多协程（DAP 多线程）设计

**日期：** 2026-08-15  
**状态：** 已实现  
**范围：** 在现有 C++ `luadap` 上，将 Lua 协程映射为 DAP threads，支持分协程断点/栈/步进；保持 `start`/`update`，新增可选 `track`。

**前置：**
- `docs/superpowers/specs/2026-08-15-luadap-cpp-dap-design.md`（已实现）
- 行为对照：现 `native/luadap/lua_debug.c` / `dap_session.c`（当前单合成线程 `id=1`）

## 目标

1. DAP `threads` 列出主 state + 已登记存活协程。  
2. 子协程可命中断点/步进；`stopped.threadId` 为命中协程。  
3. `stackTrace` / `scopes` / `variables` / `step*` / `continue` 按 `threadId`（暂停协程）工作。  
4. 默认挂钩 `coroutine.create` / `coroutine.wrap`；并提供 `luadap.track(co, name?)`。  
5. 现有单线程 Python 回归仍通过。

## 已锁定决策

| 决策 | 选择 |
|------|------|
| 能力层级 | 完整 DAP 多线程（方案 2） |
| 暂停语义 | 仅命中协程在 DAP 中算停：`allThreadsStopped=false` |
| 发现方式 | 挂钩 `coroutine.create`/`wrap` + 可选 `track`（方案 3） |
| 实现结构 | 协程注册表 + 每协程 `lua_sethook`（方案 A） |
| 主线程 | 固定 `threadId=1`，`name="main"` |
| 非暂停协程的 stackTrace | 返回空 `stackFrames`（不假装可读） |
| DAP `pause` 请求 | 本轮不做 |

## 架构

```
宿主 Lua
  require("luadap").start / update / track(co, name?)
        │
        ▼
┌──────────────── luadap.dll ─────────────────┐
│  coro_registry   id ↔ lua_State* + name      │
│  wrap coroutine.create / wrap（start 时）     │
│  lua_debug       每登记协程同一 line hook     │
│  dap_session     threads / stopped.threadId  │
│                  stackTrace 按 threadId       │
└─────────────────────────────────────────────┘
```

不改变 asyncsocket 传输与 `Content-Length` 帧路径。

## DAP 语义

| 命令/事件 | 行为 |
|-----------|------|
| `threads` | 注册表中仍存活的条目：`main` + `coro-<id>`（或 track 自定义名） |
| `stopped` | `threadId` = 命中协程；**`allThreadsStopped=false`** |
| `stackTrace` | `threadId` → `lua_State*`。当前暂停协程：完整用户 `@` 帧；其它已登记：空 `stackFrames`；未知 id：失败响应 |
| `scopes` / `variables` | 仅当前暂停协程的帧/ref（暂停时重建 var map，与现一致） |
| `continue` | 清除暂停；V1 可忽略 `threadId` 或不匹配则失败（实现时二选一并写死；推荐：仅当省略或等于 paused id 时成功） |
| `next` / `stepIn` / `stepOut` | 步进状态绑定**当前暂停** `L`；深度用该 `L`；其它协程行 hook 不因该 step 停（除非撞断点） |

协作式调度下其它协程实际也不会前进；DAP 仍不将它们标为 stopped（与锁定语义一致）。

无子协程时与今日单线程行为兼容。

## 登记与生命周期

1. **`start`**：登记主 `lua_State*` 为 id=1；包装全局 `coroutine.create` / `coroutine.wrap`（先调原函数，再走内部 track）。  
2. **内部 track / `luadap.track(co, name?)`**：未登记则分配递增 id、设名、对该 thread `lua_sethook`；已登记则幂等；非 thread → Lua 错误。  
3. **清理**：在 `update` 和/或 `threads` 时移除 dead / 失效引用，并卸 hook（若 state 仍有效）。  
4. **`shutdown`**：卸所有 hook、恢复原 `coroutine.create`/`wrap`、清空注册表。  
5. Registry 持有对 coroutine 的引用，避免调试期间 id 悬空。

默认名：`coro-<id>`；`track` 可选第二参覆盖。

## 对外 API

```lua
local dap = require("luadap")
dap.start(host, port, is_wait_connect)
dap.update()
dap.track(co, name?)   -- 新增；挂钩路径内部共用
```

`start`/`update`/`_VERSION` 语义不变。

## 实现落点（指导）

| 单元 | 职责 |
|------|------|
| `coro_registry`（新 .c/.h 或并入 `lua_debug`） | id 分配、lookup、list、gc 清理、wrap install/uninstall |
| `lua_debug` | hook 安装到任意 `L`；`pause_loop` 记录 paused id/`L`；stack walk 用目标 `L` |
| `dap_session` | `handle_threads`、`send_stopped` 带真实 threadId、`stackTrace` 按 id 路由 |
| `luadap.c` | 导出 `track` |

## 测试

新增（建议）：

- debugee：`coroutine.create` 内设断点 → `stopped.threadId >= 2`（或非 1）  
- `threads` 含 main + 子协程  
- 对该 id `stackTrace` + locals 可读；对 main 在子协程暂停时可要空栈  

回归：现有 handshake / breakpoint / step / disconnect / partial / condition / cycle / luadap wait+nowait 全绿。

## 非目标

- DAP `pause` 请求  
- 真实 OS 多线程 / 多 `lua_State` 宿主  
- 读取**未暂停**协程的完整调用栈  
- 自定义 VS Code 扩展  
- 改变单 DLL / 无嵌入 Lua 的产品形态  

## 风险与缓解

| 风险 | 缓解 |
|------|------|
| 子协程未装 hook | create/wrap 包装 + track；文档说明绕过包装的创建需 `track` |
| 步进串协程 | step 状态带 paused `L`；`on_line` 校验 `L == paused` 或 step 仅对发起步进的线程生效 |
| 包装与宿主自改 `coroutine` 冲突 | shutdown 恢复原函数；start 时包装一次 |
| 死协程残留 | threads/update 清理；registry 引用 + status 检查 |
| `allThreadsStopped=false` 与 VS Code 预期 | 用多协程冒烟测 UI；语义已锁定 |

## 验收清单

1. `threads` 反映 main + 存活已登记协程  
2. 子协程断点 `stopped.threadId` 正确且 `allThreadsStopped=false`  
3. 暂停协程上栈/变量/步进可用  
4. `track` 与挂钩路径均可登记  
5. 单线程回归全过  
