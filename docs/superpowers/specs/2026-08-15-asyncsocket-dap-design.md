# 异步网络 I/O（WSAPoll/poll）+ DAP 读协程 设计

**日期：** 2026-08-15  
**状态：** 已批准，待实现  
**范围：** 用独立 Lua C 扩展做异步 TCP；DAP 组帧在 Lua 协程中 yield/resume；替换 `debugger.lua` 阻塞 luasocket 读写

**前置：** V1 DAP 调试器已可用（`docs/superpowers/specs/2026-08-15-lua-dap-debugger-design.md`）

## 目标

1. 使用 **WSAPoll（Windows）/ poll（POSIX）** 做异步网络 I/O。  
2. **poll 运行在独立线程**；不在该线程调用任何 Lua API。  
3. Lua 层可设置 **`on_open` / `on_message` / `on_close`**。  
4. DAP 读逻辑跑在 **独立 Lua 协程**：无数据或半包则 `yield`；整帧则 `dispatch` handler。  
5. **`dbg.listen` 对外仍阻塞到 `configurationDone`**（内部通过 `update`/`pump` 泵事件）。  
6. 宿主只调 **`dbg.update()`**（内部 `asyncsocket.pump` + resume 读协程）。

## 已锁定决策

| 决策 | 选择 |
|------|------|
| 实现形态 | 方案 1：通用 `asyncsocket` C 扩展 + DAP 协程组帧 |
| 模块形态 | 独立 Lua C DLL（`require("asyncsocket")`），非嵌在 main 里 |
| 事件上送 | 宿主调 `dbg.update()`；扩展提供 `pump()` |
| `listen` 语义 | 类型 A：内部泵到握手完成再返回 |
| 客户端数 | V1：单 TCP server + 至多 1 个 client |
| DAP 解析位置 | 留在 Lua（C 只传原始字节） |

## 架构

```
┌─────────────────────┐     事件队列      ┌──────────────────────────────┐
│  asyncsocket.dll    │  open/msg/close  │  Lua debugger.lua             │
│  poll 线程:         │ ───────────────► │  on_* 回调 → 字节缓冲         │
│   WSAPoll / poll    │                  │  DAP 读协程: 组帧 / yield     │
│  非阻塞 recv/send   │ ◄─────────────── │  dispatch handlers            │
│  主线程: pump()     │   send / listen  │  update() = pump + resume     │
└─────────────────────┘                  └──────────────────────────────┘
```

### 线程模型

| 线程 | 职责 | 禁止 |
|------|------|------|
| poll 线程 | wait I/O、`accept`/`recv`、入队事件、可写时冲刷发送缓冲 | 调用 Lua / sol2 |
| Lua 主线程 | `pump` 出队、触发回调、`coroutine.resume` DAP 协程、`send` API | 在 poll 线程跑 Lua |

唤醒手段：自管道 / `socketpair`（Windows 用等价 wakeup socket），以便 `close` 与停线程时打断阻塞的 poll。

## C 扩展 API（Lua）

```lua
local asyncsocket = require("asyncsocket")

local s = asyncsocket.listen(host, port)  -- 非阻塞启动 listen + poll 线程
s:on_open(function() end)
s:on_message(function(chunk) end)       -- chunk: 原始字节，可能半包
s:on_close(function() end)
s:send(bytes)                           -- 主线程投递发送（短消息优先一次写完）
s:close()                               -- 停线程、关 fd

asyncsocket.pump()                      -- 主线程 drain 事件队列并调回调
```

### 事件语义

| 事件 | 时机 |
|------|------|
| `on_open` | `accept` 成功，client 已设为非阻塞并加入 poll 集 |
| `on_message` | 每次 `recv` 得到的一段字节（不保证整 DAP 帧） |
| `on_close` | 对端关闭、`recv==0`、致命错误、或主动 `close` |

### 实现要点（C）

- Windows：`WSAStartup`、`WSAPoll`；POSIX：`poll`。  
- listen fd + client fd（0 或 1）+ wakeup fd 进入同一 poll 集。  
- 事件队列：互斥锁保护；元素为 `{type, payload?}`。  
- `send`：主线程对 client 非阻塞 `send`；若只写部分，剩余入 per-socket 发送缓冲，poll 线程在 `POLLOUT` 时继续写。V1 DAP 响应通常很小，以“尽量写完、失败则入队”为准。  
- 链接：与仓库中 `lua.exe` / `main` **同一套 Lua 链接策略**，导出 `luaopen_asyncsocket`，输出到 `bin/asyncsocket.dll`。

## debugger.lua 改造

### 读协程

```
on_message(chunk):
  recv_buf = recv_buf .. chunk
  resume(dap_reader_coro)

dap_reader_coro:
  while true do
    msg = try_parse_one_dap_frame(recv_buf)  -- 不够则返回 nil
    if not msg then
      yield()
    else
      dispatch(msg)   -- 可能改变 paused / configured
    end
  end
```

`try_parse_one_dap_frame`：解析 `Content-Length` + body；半包不消费错误数据（或按现逻辑报错关会话）。

### `M.update()`

```lua
function M.update()
  asyncsocket.pump()
  if dap_reader_coro and coroutine.status(dap_reader_coro) ~= "dead" then
    coroutine.resume(dap_reader_coro)
  end
end
```

（`on_message` 里若已 resume，则 `update` 末尾 resume 可为 no-op / 仅处理无新字节时的空转；实现时避免双 resume 竞态，约定 **只在 `pump` 触发的回调里 resume，或只在 `update` 末尾 resume 一次**。）

**推荐约定（消除歧义）：**  
- `on_message` **只追加** `recv_buf`，不 `resume`。  
- `M.update()`：`pump()` 后 **统一** `resume` 读协程（可循环 resume 直到再 yield，以吃掉缓冲里多帧）。

### `M.listen(host, port)`

1. `asyncsocket.listen` + 注册 `on_*`。  
2. 创建读协程。  
3. `while not state.configured do M.update(); 短暂 sleep 或依赖 pump 空转 end`。  
4. `install_hook()` 后返回。

### `pause_loop`

`while state.paused do M.update(); ... end`  
不再阻塞在 `socket:receive`。

### `send_raw`

改为 `state.sock:send(frame)`。

### 宿主

```cpp
dbg.listen(...);          // 内部泵到 configurationDone
RunFile(sample);
while (true) {
  lua["update"](...);     // 业务
  dbg.update();           // 或业务 update 内调用
}
dbg.shutdown();
```

## 文件改动

| 路径 | 动作 |
|------|------|
| `native/asyncsocket/`（源码 + CMake） | 新增 |
| 根 `CMakeLists.txt` | `add_subdirectory`，产出 `bin/asyncsocket.dll` |
| `script/lua-runtime/debugger.lua` | 改异步 + 读协程 |
| `main/main.cpp` | 循环中调用 `dbg.update()` |
| `script/test/*` | asyncsocket 冒烟 + 现有 DAP 回归 |
| `README.md` | 文档更新 |
| 本 spec | 定稿 |

## 明确不做（本版）

- 多客户端、TLS、UDP  
- C 侧解析 DAP JSON  
- 完全公平的大规模可写调度  
- 改 VS Code `debugServer` 用法  

## 验收标准

1. `require("asyncsocket")` 成功（`bin/asyncsocket.dll` 在 cpath）。  
2. `listen` 仍等到握手完成再跑 sample。  
3. 断点暂停下 Variables / continue / step 正常。  
4. 半包 DAP 帧：协程 yield，拼齐后再 handler。  
5. 断开连接：`on_close` → 与现网一致的 teardown / `terminated`。  
6. 现有 Python DAP 冒烟通过。  

## 风险与缓解

| 风险 | 缓解 |
|------|------|
| 双份 liblua / 缺 `luaopen_*` | 对齐现有 luasocket 修复经验；单测 `require` |
| hook 内 `update` 重入 | 回调只追加 buffer；resume/dispatch 规则单一 |
| Windows 停线程卡在 poll | wakeup fd 必做 |
| 发送截断 | 发送缓冲 + POLLOUT；测大 `variables` 响应 |

## 与 V1 spec 的关系

本设计 **替换** V1 中“luasocket 阻塞读写”的传输实现；DAP 协议表面、断点/变量/步进语义不变。V1 spec 可在实现后增补「传输层：asyncsocket」实现备注。
