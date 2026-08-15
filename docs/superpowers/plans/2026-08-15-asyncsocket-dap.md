# Asyncsocket + DAP 读协程 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用独立 `asyncsocket` C 扩展（WSAPoll/poll 后台线程）替换 DAP 阻塞 luasocket 读写，Lua 侧用协程组帧（半包 yield），`dbg.update()` 泵事件。

**Architecture:** poll 线程只做 I/O 与入队；主线程 `asyncsocket.pump()` 调 `on_*`；`on_message` 只追加 buffer；`M.update()` resume DAP 读协程直到再 yield；`listen`/`pause_loop` 内循环 `update`。

**Tech Stack:** C11、Lua 5.4、Windows WSAPoll / POSIX poll、CMake、现有 `debugger.lua` / Python DAP 测试

**Spec:** `docs/superpowers/specs/2026-08-15-asyncsocket-dap-design.md`

## Global Constraints

- poll 线程禁止调用任何 Lua API
- `on_message` 只追加 `recv_buf`，不 `resume`；仅 `M.update()` 在 `pump` 后 resume（可连吃多帧）
- `listen` 对外仍阻塞到 `configurationDone`
- V1：单 server + 至多 1 client；C 侧不解析 DAP
- 产出 `bin/asyncsocket.dll`，导出 `luaopen_asyncsocket`；链接策略对齐仓库 Lua
- Windows 必须有 wakeup，避免 `close` 卡在 poll
- 保持现有 DAP 语义（断点/变量/步进/条件/循环引用）；提交信息英文 concise

---

## File Structure

| 文件 | 职责 |
|------|------|
| `native/asyncsocket/asyncsocket.c` | 模块入口、userdata、Lua API |
| `native/asyncsocket/poll_loop.c` / `.h` | 线程、WSAPoll/poll、队列、wakeup |
| `native/asyncsocket/CMakeLists.txt` | 构建 DLL → `bin/` |
| `CMakeLists.txt` | `add_subdirectory(native/asyncsocket)` |
| `script/lua-runtime/debugger.lua` | 改异步传输 + 读协程 + `M.update` |
| `main/main.cpp` | 循环调用 `dbg.update()` |
| `script/test/test_asyncsocket_smoke.py` | 扩展 listen/半包/close 冒烟 |
| `script/test/test_dap_partial_frame.py` | DAP 半包组帧 |
| 现有 `test_dap_*.py` | 回归 |
| `README.md` | 文档 |

---

### Task 1: 脚手架 — 可 `require("asyncsocket")` 的空模块

**Files:**
- Create: `native/asyncsocket/asyncsocket.c`
- Create: `native/asyncsocket/CMakeLists.txt`
- Modify: `CMakeLists.txt`（根）
- Test: `bin/lua.exe -e "require('asyncsocket'); print('ok')"`

**Interfaces:**
- Produces: `luaopen_asyncsocket` → 返回 table（可先只有 `_VERSION`）

- [ ] **Step 1: 写最小 C 模块**

`native/asyncsocket/asyncsocket.c`:

```c
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

int luaopen_asyncsocket(lua_State *L) {
    lua_newtable(L);
    lua_pushstring(L, "0.1.0");
    lua_setfield(L, -2, "_VERSION");
    return 1;
}
```

- [ ] **Step 2: CMake**

`native/asyncsocket/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
add_library(asyncsocket SHARED asyncsocket.c)
target_include_directories(asyncsocket PRIVATE
    ${CMAKE_SOURCE_DIR}/3rd/lua-5.4.8/inc)
# Link same liblua as luasocket_shared / lua.exe — inspect 3rd/luasocket/CMakeLists.txt
# and mirror: typically target_link_libraries(asyncsocket PRIVATE liblua) or lua54.
# On Windows also link ws2_32 later (Task 2).
set_target_properties(asyncsocket PROPERTIES
    OUTPUT_NAME "asyncsocket"
    PREFIX "")
if(WIN32)
    target_link_libraries(asyncsocket PRIVATE ws2_32)
endif()
```

根 `CMakeLists.txt` 增加：`add_subdirectory(native/asyncsocket)`。

对照 `3rd/luasocket/CMakeLists.txt` / `3rd/lua-5.4.8` 里 `liblua` 目标名，**必须**能成功 `require`，避免静态链两份 Lua。

- [ ] **Step 3: 编译并验证**

```powershell
cmake --build E:\demo\lua-dap-debugger\build\msvc --config Debug --target asyncsocket
# 确认 bin/asyncsocket.dll 存在
$lua = (Get-ChildItem -Recurse -Filter lua.exe bin,build | Select-Object -First 1).FullName
& $lua -e "package.cpath='bin/?.dll;'..package.cpath; local a=require('asyncsocket'); assert(a._VERSION); print('asyncsocket ok')"
```

Expected: `asyncsocket ok`

- [ ] **Step 4: Commit**

```powershell
git add native/asyncsocket CMakeLists.txt
git commit -m "chore: scaffold asyncsocket Lua C module"
```

---

### Task 2: listen + poll 线程 + pump + on_open/on_message/on_close

**Files:**
- Create: `native/asyncsocket/poll_loop.h`, `poll_loop.c`
- Modify: `native/asyncsocket/asyncsocket.c`
- Test: `script/test/test_asyncsocket_smoke.py`

**Interfaces:**
- Produces:
  - `asyncsocket.listen(host: string, port: number) -> userdata sock`
  - `sock:on_open(fn)` / `on_message(fn)` / `on_close(fn)`
  - `asyncsocket.pump()`
  - 事件：OPEN / MESSAGE(bytes) / CLOSE
- Consumes: Task 1 模块骨架

- [ ] **Step 1: 写失败的 Python 冒烟（先于完整实现）**

`script/test/test_asyncsocket_smoke.py`：用 `lua.exe` 跑一小段脚本：`listen` → 注册回调 → 循环 `pump`；Python TCP 客户端连接并发送 `"hello"`；断言 Lua 打印 `OPEN` 与含 `hello` 的 `MSG`；客户端关闭后见 `CLOSE`。

（具体脚本可用 `lua -e` 或 `script/test/run_asyncsocket_smoke.lua`。）

- [ ] **Step 2: 实现 poll 循环核心（建议结构）**

`poll_loop` 内部状态（每 listen 实例一份）：

- `SOCKET listen_fd`, `client_fd`（`INVALID` 若无）
- wakeup 对（Windows：`socketpair` 模拟用 loopback TCP 或 `WSAEvent`+额外 socket；推荐 **loopback TCP 自连作 wakeup**）
- `std::mutex` / `CRITICAL_SECTION` + 事件队列 `vector<{type, string payload}>`
- `std::thread` / `_beginthreadex` 跑 `poll_thread_main`
- 标志 `running`

线程循环：

1. 组装 `pollfd`：listen(`POLLIN`)、client(`POLLIN` [| `POLLOUT` if send buf])、wakeup(`POLLIN`)
2. `WSAPoll` / `poll` timeout ~100ms 或无限 + wakeup
3. wakeup 可读：排空
4. listen 可读：`accept` → 设非阻塞 → 若已有 client 则关掉新连接或旧连接（V1：**拒绝第二连接**或关掉旧的，选一种并写进注释；推荐拒绝/关闭新连接）
5. client 可读：`recv` → 入队 MESSAGE；0 → 入队 CLOSE并清 client
6. client 可写：冲刷发送缓冲

`pump()`（主线程）：加锁 swap 队列；对每个事件查 userdata 上注册的回调并 `lua_pcall`。

Lua 绑定：userdata 存 `AsyncSocket*`；方法表 `on_open`/`on_message`/`on_close`/`send`/`close`；模块函数 `listen`/`pump`。

回调存储：在 userdata 关联表里用 registry ref 存三个 function。

- [ ] **Step 3: 跑冒烟至 GREEN**

```powershell
python script/test/test_asyncsocket_smoke.py
```

Expected: 打印成功摘要（如 `asyncsocket smoke ok`）

- [ ] **Step 4: Commit**

```powershell
git add native/asyncsocket script/test/test_asyncsocket_smoke.py script/test/run_asyncsocket_smoke.lua
git commit -m "feat: asyncsocket listen/poll thread and pump callbacks"
```

---

### Task 3: send + close + 半包不丢数据

**Files:**
- Modify: `native/asyncsocket/*`
- Modify: `script/test/test_asyncsocket_smoke.py`（或新文件）扩 send/半包

**Interfaces:**
- Produces: `sock:send(str)`；`sock:close()` 停线程并 JOIN；多段 `recv` 原样多段 `on_message`

- [ ] **Step 1: 实现 send**

主线程：若无 client 返回错误；非阻塞 `send`；剩余追加 `send_buf`；标记需 `POLLOUT`；wakeup 线程。

- [ ] **Step 2: 实现 close**

设 `running=false`；wakeup；`join` 线程；关所有 fd；入队或同步触发 `on_close`（若尚未 CLOSE）；清回调。

- [ ] **Step 3: 测试客户端分两次发送 `"hel"` + `"lo"`，Lua 侧拼接等于 `hello`**

- [ ] **Step 4: Commit**

```powershell
git commit -am "feat: asyncsocket send/close and fragmented messages"
```

---

### Task 4: debugger.lua — DAP 读协程 + asyncsocket 传输

**Files:**
- Modify: `script/lua-runtime/debugger.lua`
- Test: 现有 `script/test/test_dap_handshake.py` 等（改 debugee 的 cpath 含 `asyncsocket.dll`）

**Interfaces:**
- Consumes: Task 2–3 `asyncsocket` API
- Produces: `M.update()`、`M.listen` 内部泵、`pause_loop` 非阻塞、`send_raw` → `sock:send`

- [ ] **Step 1: 引入状态字段**

```lua
-- 替换 require("socket")
local asyncsocket = require("asyncsocket")
-- state 增加：
-- sock, recv_buf, reader_coro, client_open
```

- [ ] **Step 2: 实现组帧**

```lua
local function try_parse_one_dap_frame()
    -- 在 state.recv_buf 中找 "\r\n\r\n"
    -- 解析 Content-Length
    -- 若 #buf 不足 header+body 返回 nil
    -- 否则切出 body，json.decode，收缩 recv_buf，返回 object
end

local function reader_main()
    while true do
        local msg = try_parse_one_dap_frame()
        if not msg then
            coroutine.yield()
        else
            dispatch(msg)
        end
    end
end
```

- [ ] **Step 3: 接线 listen**

```lua
function M.listen(host, port)
    ...
    state.sock = asyncsocket.listen(host, port)
    state.recv_buf = ""
    state.reader_coro = coroutine.create(reader_main)
    state.sock:on_open(function() state.client_open = true end)
    state.sock:on_message(function(chunk)
        state.recv_buf = state.recv_buf .. chunk
    end)
    state.sock:on_close(function()
        -- 对齐现 shutdown：卸 hook、terminated 等
        shutdown_session()
    end)
    while not state.configured do
        M.update()
        -- 避免空转占满 CPU：socket.sleep(0.001) 若仍有 luasocket；
        -- 或 asyncsocket.pump 内部已阻塞等待则可不 sleep。
        -- 推荐扩展增加 asyncsocket.wait(ms) 仅 sleep；或 pump 前短 sleep。
    end
    install_hook()
end

function M.update()
    asyncsocket.pump()
    local co = state.reader_coro
    if not co or coroutine.status(co) == "dead" then return end
    for _ = 1, 32 do  -- 限制连吃帧数，防疯狂
        local ok, err = coroutine.resume(co)
        if not ok then error(err) end
        if coroutine.status(co) == "suspended" then
            -- yielded for more data
            break
        end
        if coroutine.status(co) == "dead" then break end
    end
end
```

注意：`reader_main` 在 `dispatch` 后若不 yield 会立刻 parse 下一帧——`for` 循环 resume 直到 yield。若协程在 `dispatch` 后直接再 `try_parse` 失败而 `yield`，status 为 suspended，正确。

- [ ] **Step 4: `pause_loop` 改为**

```lua
while state.paused do
    M.update()
end
```

去掉阻塞 `read_message`。

- [ ] **Step 5: `send_raw` 用 `state.sock:send(frame)`**

- [ ] **Step 6: 更新所有 `run_debugee*.lua` 的 `package.cpath` 包含 `bin/?.dll`（已有则确认能 load asyncsocket）**

- [ ] **Step 7: 跑握手测试**

```powershell
cd script/test
python test_dap_handshake.py
```

Expected: `handshake ok`

- [ ] **Step 8: Commit**

```powershell
git add script/lua-runtime/debugger.lua script/test/run_debugee*.lua
git commit -m "feat: DAP over asyncsocket with reader coroutine"
```

---

### Task 5: 半包 DAP 帧测试 + 全量回归

**Files:**
- Create: `script/test/test_dap_partial_frame.py`
- Modify: none unless bugs

**Interfaces:**
- Consumes: Task 4 debugger

- [ ] **Step 1: 写测试** — 对握手中某请求（如 `initialize`）拆成两截 TCP `sendall`，中间 `sleep(0.05)`，断言仍收到正确 response + `initialized`

- [ ] **Step 2: 跑全套**

```powershell
python test_dap_partial_frame.py
python test_dap_handshake.py
python test_dap_breakpoint.py
python test_dap_step.py
python test_dap_condition.py
python test_dap_table_cycle.py
python test_dap_disconnect.py
```

Expected: 全部 ok

- [ ] **Step 3: Commit**

```powershell
git add script/test/test_dap_partial_frame.py
git commit -m "test: DAP partial-frame framing over asyncsocket"
```

---

### Task 6: 宿主 `main.cpp` + README + spec 状态

**Files:**
- Modify: `main/main.cpp`
- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-08-15-asyncsocket-dap-design.md`（状态→已批准/已实现）
- Modify: `docs/superpowers/specs/2026-08-15-lua-dap-debugger-design.md`（实现备注：传输层 asyncsocket）

**Interfaces:**
- Consumes: `M.update` / `M.shutdown`

- [ ] **Step 1: main 循环**

在现有 `while (true) { lua_main_update(...); }` 内增加：

```cpp
lua.safe_script("require('lua-runtime.debugger').update()");
```

或绑定 `sol::function dbg_update = lua["require"]("lua-runtime.debugger")["update"];` 后每帧调用。若无业务 `update`，仍应空转调用 `dbg.update()`（或 sleep），以便运行中处理 disconnect。

- [ ] **Step 2: 更新 README**（中文）：asyncsocket、`dbg.update()`、不再依赖 DAP 路径上的阻塞 luasocket

- [ ] **Step 3: 编译 main，手动确认 listen 打印后 F5 仍可用（或 Python attach 握手）**

- [ ] **Step 4: Commit**

```powershell
git commit -am "feat: pump debugger.update from host; docs for asyncsocket"
```

---

### Task 7: 收尾自检

- [ ] **Step 1: 再跑 Task 5 全套冒烟**
- [ ] **Step 2: 确认 `bin/asyncsocket.dll` 与 `require` 在 Debug 输出目录一致**
- [ ] **Step 3: 若有未提交的条件断点/table cycle 改动，一并整理进合理 commit（勿混进无关文件）**
- [ ] **Step 4: Commit docs 状态（若未在 Task 6 提交）**

---

## Plan Self-Review

| Spec 要求 | Task |
|-----------|------|
| WSAPoll/poll 独立线程 | Task 2 |
| on_open/on_message/on_close | Task 2 |
| DAP 协程半包 yield | Task 4–5 |
| listen 泵到 configurationDone | Task 4 |
| dbg.update = pump + resume | Task 4、6 |
| 单客户端 | Task 2 |
| 不停用 Lua 在 poll 线程 | Task 2 约束 |
| 验收半包/回归 | Task 5 |
| wakeup/close | Task 3 |

无 TBD；resume 约定与 spec 推荐一致（仅 update 后 resume）。
