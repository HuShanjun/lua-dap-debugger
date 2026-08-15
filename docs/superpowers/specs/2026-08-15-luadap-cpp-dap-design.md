# luadap C++ DAP 重写设计

**日期：** 2026-08-15  
**状态：** 待实现  
**范围：** 保留 `require("luadap")` 的 `start` / `update` 对外接口；**移除 Lua 脚本嵌入**；DAP 协议、断点/步进/变量等逻辑全部用 C/C++ 实现；传输继续复用 `asyncsocket` C API。

**前置 / 替代关系：**
- 行为金标准：现有 `script/lua-runtime/debugger.lua`（对照参考，非运行时）
- 传输：`docs/superpowers/specs/2026-08-15-asyncsocket-dap-design.md`
- 门面历史：`docs/superpowers/specs/2026-08-15-luadap-dll-design.md`（嵌入 Lua 方案；本设计在实现落地后取代其运行时路径）

## 目标

1. 交付 **`bin/luadap.dll`**：`require("luadap")` → 仅 **`start(host, port, is_wait_connect)`** 与 **`update()`**。  
2. **不再**嵌入 / `load` `debugger.lua` / `dkjson`；删除 `gen_embed.py`、`embed_lua.cmake` 及生成 blob。  
3. DAP 帧解析、JSON、会话状态、line hook、断点（含条件）、步进、栈/局部/表成员（含环）、disconnect/terminated：**全部在 C/C++**。  
4. 传输：**静态链接**并直接调用 `poll_loop.h`（`as_socket_*`），不经 Lua `require("asyncsocket")`。  
5. 功能与现 `debugger.lua` **同等**；现有 Python DAP 回归为验收标准。  
6. `is_wait_connect == true` 阻塞到 `configurationDone`；`false` 立即返回，靠后续 `update` 握手。

## 已锁定决策

| 决策 | 选择 |
|------|------|
| 调试目标 | 仍调试同一 `lua_State` 上的 Lua 业务脚本 |
| 对外 API | 保持 luadap：`start` / `update`（可选 `_VERSION`） |
| 实现结构 | 分层：framing / json / session / lua_debug / luadap_api |
| JSON | cJSON |
| 传输 | 复用 asyncsocket C API（方案 1） |
| 嵌入 Lua | 删除产品路径 |
| `debugger.lua` | 可留仓库作对照；不参与构建与默认测试路径 |
| 多实例 | V1 单 session |
| VS Code | 继续 `debugServer`（如 8172），无自定义扩展 |

## 架构

```
宿主 Lua / C++(sol2)
    require("luadap") → start / update
              │
              ▼
┌─────────────────── luadap.dll ───────────────────┐
│  luadap_api          luaopen + start/update      │
│  dap_session         DAP 请求/响应/事件路由        │
│  dap_framing         Content-Length 缓冲/拆帧     │
│  dap_json            cJSON 编解码                 │
│  lua_debug           hook / 断点 / 步进 / 变量     │
│  静态链 asyncsocket   as_socket_* (poll 线程)       │
└──────────────────────────────────────────────────┘
         ▲ 同一 lua_State*
业务脚本
```

**线程模型（与现行为对齐）：**
- Poll 在 asyncsocket 后台线程；**禁止**在 poll 线程调 Lua。  
- `update()`（及 `start` 的 wait 循环、`pause_loop`）在**宿主 Lua 线程**泵事件、拆帧、dispatch、装 hook。  
- Line hook 必须安装在跑业务脚本的同一线程（`update` / wait 结束后），避免 Lua per-thread hook 装错线程。

**相对 Lua 版的刻意变化：**
- **无** `reader_coro`：MESSAGE 追加 `recv_buf` 后由同线程同步拆帧并 dispatch（可限次）。  
- **无**跨协程 `pause_thread` / `debug.getinfo(thread, …)`；pause 时 dispatch 嵌在同线程 `pause_loop` 内，用帧过滤校准 `stackTrace` / `getlocal`。

## 对外 API 与生命周期

```lua
local dap = require("luadap")
dap.start(host, port, is_wait_connect)  -- wait 默认 true
dap.update()
```

不强制导出 `shutdown`（与现门面一致）；内部可有 C++ teardown。环境变量：`LUADAP_HOST` / `LUADAP_PORT`（缺省 `127.0.0.1` / `8172`）。

### `start(host, port, wait)`

1. `as_net_init` + `as_socket_listen`。  
2. 重置 session（breakpoints、seq、recv_buf、configured、hook 标志等）。  
3. `wait == true`：循环 `update` + 短 sleep 直到 `configurationDone`；若仍 `client_open` 且未 dead，在此线程 `lua_sethook`。  
4. `wait == false`：立即返回；后续 `update` 在 `configured && !hook_installed` 时装 hook。

### `update()`

1. `as_socket_take_events` → OPEN / MESSAGE（追加缓冲）/ CLOSE（标 `close_pending`，不立即 teardown）。  
2. 从缓冲拆完整 DAP 帧并 dispatch（限次，避免饿死）。  
3. 若 `close_pending`：先尽量 drain，再 teardown（可选 disconnect 响应、`terminated`、卸 hook、`as_socket_stop`）。  
4. 否则若已 configured 且未装 hook：在当前线程安装 line hook。

### 暂停路径

Line hook → 匹配断点/步进 → 发 `stopped` → `pause_loop`（循环泵+拆帧直到 continue/next/stepIn/stepOut/disconnect）→ 返回业务代码。

## DAP 能力与 `lua_debug` 对齐

**initialize 能力：**

| 标志 | 值 |
|------|-----|
| `supportsConfigurationDoneRequest` | true |
| `supportsConditionalBreakpoints` | true |
| `supportsSetVariable` | false |
| `supportsEvaluateForHovers` | false |

**线程：** 单一 `{ id = 1, name = "main" }`。

**必须实现的 command：**  
`initialize`, `attach`, `threads`, `setExceptionBreakpoints`, `setBreakpoints`, `configurationDone`, `continue`, `next`, `stepIn`, `stepOut`, `stackTrace`, `scopes`, `variables`, `disconnect`。

**事件：** `initialized`, `stopped`（breakpoint/step 等）, `terminated`。

**行为对齐点：**

1. **路径规范化**：去前导 `@`、`\`→`/`、盘符小写、去尾 `/`。  
2. **条件断点**：局部 + upvalue 组成 env，执行 `return (condition)`；编译/运行失败视为不命中。  
3. **步进**：`in` / `over` / `out` + 用户帧深度 `step_depth`。  
4. **变量 ref**：locals = `100000 + frameId`；upvalues = `200000 + frameId`；表对象从 `1000` 起、同 stop 同表复用；祖先链环 → `table (circular)` 且 `variablesReference = 0`。  
5. **帧过滤**：只暴露 `@` 源的用户帧；跳过非文件源；C++ 实现无 debugger.lua 帧，按 hook/`pause_loop` 实际栈深校准 level。  
6. **Hook**：`lua_sethook(L, on_line, LUA_MASKLINE, 0)`；teardown 时清除。

## 目录与 CMake

```
native/luadap/
  luadap.c|cpp           # luaopen_luadap + start/update
  dap_framing.{h,c|cpp}
  dap_json.{h,c|cpp}     # 封装 cJSON
  dap_session.{h,c|cpp}
  lua_debug.{h,c|cpp}
  CMakeLists.txt         # SHARED → bin/luadap.dll
                         # link: asyncsocket_static, liblua, ws2_32, cjson
third_party/cJSON/       # 或现有 vendor 路径（实现时选定一处）
```

**删除产品路径：** `gen_embed.py`、`embed_lua.cmake`、嵌入 blob 生成规则及对 `debugger.lua`/`dkjson` 的 preload。

**asyncsocket：** 继续提供 STATIC（供 luadap）与可选 SHARED（`test_asyncsocket_smoke`）。

**宿主：** `main.cpp` 保持 `require("luadap")` + `start(..., true)` + 循环 `update`；调试路径不依赖 `script/lua-runtime`。

## 测试与迁移

| 项 | 说明 |
|----|------|
| 保留并适配 | `test_dap_luadap_handshake.py`、`test_dap_luadap_nowait.py` |
| 改走 luadap | `handshake` / `breakpoint` / `step` / `disconnect` / `partial_frame` / `condition` / `table_cycle` 的 debugee：`require("luadap")`，不再 `lua-runtime.debugger` |
| 保留 | `test_asyncsocket_smoke.py` |
| 验收 | 全套相关 Python 测试 exit 0；去掉 `lua-runtime` 的 `package.path` 仍能握手 |

`script/lua-runtime/debugger.lua` 可保留作对照；README 标明非运行时依赖。

## 非目标

- 多 session / 多 Lua 线程调试  
- 自定义 VS Code 扩展  
- `setVariable` / evaluate / 异常断点语义增强  
- 重写 asyncsocket  
- 本轮删除仓库内 `debugger.lua` 文件本身  

## 风险与缓解

| 风险 | 缓解 |
|------|------|
| 栈帧 level 与 Lua 版漂移 | 以 breakpoint/step/variables 回归为金标准逐条对齐 |
| 条件断点 env 与 `load` 语义差异 | 复现 `test_dap_condition`；失败不命中 |
| CLOSE+MESSAGE 同批 | 先 drain 再 teardown（对齐现逻辑） |
| Hook 装错线程 | 仅在宿主 `update`/wait 结束路径 `lua_sethook` |
| cJSON 与 dkjson 数字/转义差异 | DAP 用整数 seq/line；字符串路径做回归覆盖 |

## 验收清单

1. `bin/luadap.dll` + `require("luadap")`，无嵌入 Lua 字符串。  
2. `start(..., true)` 等到 `configurationDone`；`start(..., false)` + `update` 可握手。  
3. 断点（含条件）、步进、栈/局部/表（含环）、disconnect/partial frame 回归通过。  
4. 部署不需要旁路 `debugger.lua` / `dkjson` / 独立 `asyncsocket.dll`（luadap 路径）。  
5. 静态链接 dumpbin/依赖检查：luadap 不依赖 `asyncsocket.dll` 作为运行时导入（开发期 SHARED 可选另测）。  
