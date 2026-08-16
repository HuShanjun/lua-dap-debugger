# 多 lua_State 共用 DAP 会话 — 设计

**日期：** 2026-08-16  
**状态：** 已批准待实现  
**范围：** `native/luadap`（`dap_session` / `coro_registry` / `lua_debug` / `luadap.c`）、宿主用法、测试；VS Code 扩展无需改协议面（仍一个 TCP DAP）

---

## 背景与目标

当前 `luadap` 进程内只有一个全局 `dap_session`，`coro_registry` 挂在单一 `g_mainL` 上。宿主若创建多个独立 `lua_State`（多 VM），只能调试其中一个，或开多个端口（本设计不做）。

**目标：** 同一进程、同一 DAP listen / 同一 VS Code 调试会话内，注册多个 main `lua_State`；Threads 扁平展示各 state 的主线程及其协程；断点只暂停命中的那个执行流。

**非目标（V1）：**

- 多个 DAP listen / 多个 VS Code 会话并行
- 命中断点时冻结其它 state（`allThreadsStopped` 保持 false）
- 跨 state 的 variables / evaluate
- 改变 `asyncsocket`「全进程一个 listen」限制

---

## 已锁定决策

| 项 | 选择 |
|----|------|
| 产品形态 | 多 VM **共用一个** DAP 会话 |
| 暂停语义 | **只停**触发 hook 的那个 `lua_State` / 协程 |
| Threads | **扁平**：每个 mainL + 其协程均为独立 DAP thread；名称带 state 前缀 |
| 注册 API | 每个 state 均可 `require("luadap"); dap.start(...)`；**同 host+port 自动并入** |
| 并入策略 | 方案 A：首 `start` 建 listen；同 host/port 再 `start` 只 join；不同 port → 错误 |

---

## API

### Lua

```lua
-- 首次：创建 listen + 登记本 state
dap.start(host, port, wait [, name])

-- 其它 lua_State：同 host/port → join（不新建 listen）
dap.start(host, port, wait [, name])

dap.update()   -- 任一已注册 state 调用均可；泵同一 asyncsocket
dap.track(co [, name])  -- 语义不变；归属调用方所在 mainL 的 state
```

- `name`（可选 string）：该 main state 在 Threads 中的前缀；缺省 `state-1`、`state-2`、…
- `wait=true`：若会话尚未 `configurationDone`，join 与首次 start 一样阻塞等待；若已 configured，立即返回
- 已有会话且 `(host, port)` 不一致：`start` 失败（Lua error / C 非 0）

### 行为不变

- 仍只接受 **一个** DAP client；第二连接关掉
- `disconnect` 结束 client、保留 listen；已登记的 states / hooks 策略见下文「Disconnect」

---

## 内部模型

### `state_registry`（新）

```
state {
  id          -- 稳定序号 1..N（用于缺省命名）
  mainL       -- 该 VM 的主 lua_State*
  name[64]    -- Threads 前缀
}
```

- 进程内列表；`start`/join 时追加；shutdown 时清空
- `mainL` 指针相等则视为已登记：重复 `start` 同 L → no-op 成功（或仅更新 name，V1 选 no-op）

### `coro_registry`（扩展）

今日：`g_mainL` + 全局 entries，thread id 从 2 起，id 1 = main。

改为：

- entries 增加 `state_id`（或 `mainL` 指针）归属
- **每个**登记的 mainL：创建 id 时登记一条「main」线程；安装 `coroutine.create`/`wrap` 包装（包装函数闭包在该 mainL）
- DAP `threadId` **全局唯一**（继续用单调 `g_next_id`，不按 state 重置）
- 显示名：
  - main：`{stateName}`（若仅一 state 可仍显示 `main` 以兼容？**V1 统一用 stateName**，单 state 缺省名 `state-1`；或单 state 时缺省名 `main`——**采用：仅一个 state 且未传 name 时名为 `main`；多 state 未传 name 时为 `state-N`**）
  - 协程：`{stateName}/{coroName}`，`coroName` 同今日（`coro-%d` 或 `track` 名）

### `dap_session`

- 仍单例；增加「已 listen 的 host/port」缓存供 join 比对
- `paused_L` / `step_L` / `paused_thread_id` 语义不变（只指向当前暂停执行流）
- `reset_client` / hooks：须对 **所有** 已登记 mainL 清/重装 hook（见 Disconnect）
- 断点表仍会话级共享

### Hook / pause

- 各 mainL（及 track 的协程）均 `lua_sethook`
- `on_line_hook` → `pause_loop(L)` 只阻塞该 `L`；`stopped` 事件带该 threadId，`allThreadsStopped: false`
- 其它 state 继续执行；对其 `stackTrace` 在未暂停时返回空栈（与现协程行为一致）

### `update`

- `dap_session_update(L)` 不依赖传入 L 的身份（只要会话 listening）；传入 L 仅用于 dispatch 里需要「默认 state」的少数路径
- `stackTrace` / `variables` / `evaluate` 仍以 `paused_L` / `threadId → coro_registry_state_for` 解析，须能解析跨 state 的 thread

### Disconnect / re-attach

- **Client disconnect（保留 listen）：** 清除 paused/step、断点、recv；对 **所有** 登记 mainL `lua_debug_clear_hook`；`configured=0` 等待再 attach。states 与 coro 包装 **保留**（与今日「不拆 coro wrappers」一致），再 attach 成功 / `configurationDone` 后对所有 mainL 重新 `install_hook`
- **进程级 shutdown：** 卸所有包装、clear registry、关 listen（今日 `dap_session_shutdown` 扩展为遍历 states）

---

## 宿主用法示例

```cpp
// VM A
lua_State* A = luaL_newstate();
luaL_requiref(A, "luadap", luaopen_luadap, 0); lua_pop(A, 1);
lua_getglobal(A, "require"); // or set package.cpath and dostring:
// dap.start("127.0.0.1", 8172, false, "logic")

// VM B
lua_State* B = luaL_newstate();
// dap.start("127.0.0.1", 8172, false, "ui")  -- joins

for (;;) {
  // 业务 tick A / B
  // 任选一处泵 DAP：
  dap_update_from(A);  // or B
}
```

`sample` 可后续加可选第二 state 演示；**V1 以测试为准，不强制改 sample。**

---

## 测试

新增（建议）：

1. **`test_dap_multi_state_threads.py`**  
   - 两个 Lua 进程内 state：实际用 **一个** `lua.exe` 无法两 state；应用 **C 小宿主** 或 **Lua 无法跨 newstate**——应用 **嵌入式测试**：扩展现有 debugee，或新增 `tools`/`test` 下用 `lua-runner` 变体。  
   - 实用路径：新增 `test/run_debugee_multi_state.c` 或在 Python 中起 **两个** 通过 **同一 DLL 会话** 的路径——因 `luadap` 会话是 **进程内全局**，必须 **同一进程两个 lua_State**。  
   - **方案：** 小 C 测试宿主 `test_host_multi_state`（或扩 `lua-runner` 不合适）→ 更轻：在 `test/` 增加由 `lua.exe` + 无法实现；故 **CMake 测试可执行文件** `multi_state_dap_host`：创建 A/B，`start` 同端口，打印 listening，供 Python DAP client 连上后 `threads` 断言 ≥2 个 main 名。

2. **Handshake join：** B join 后 `threads` 含 `logic` 与 `ui`（或 `state-1`/`state-2`）

3. **Pause isolation：** A 脚本断点停下时 B 仍能通过「B 侧共享计数/文件心跳」证明在跑（或 B 不在 pause_loop）；`stackTrace(A)` 非空，`stackTrace(B)` 空

4. **Port mismatch：** 第二 state `start` 不同 port → 失败

5. 回归：现有单 state 协程 / handshake / evaluate / condition 全绿

---

## 实现分期（建议）

1. `state_registry` + `start` join / port 校验 + 多 main hook 安装  
2. `coro_registry` 多 mainL + 线程命名前缀 + `threads` 列表  
3. `reset_client` / shutdown 遍历所有 state  
4. 多 state 测试宿主 + Python 用例  
5. README 简述

---

## 风险与注意

- **Registry refs：** 每个 mainL 的 `LUA_REGISTRYINDEX` 独立；coro `reg_ref` 必须对所属 mainL `luaL_ref/unref`
- **Hook 与死锁：** 只停一 state 时，宿主若在 **同一 OS 线程** 交替 resume A/B，A 在 `pause_loop` 内调 `update` 即可继续服务 DAP；若 B 的逻辑也在同线程且 A 卡住，B 也不会跑——这是单线程宿主固有限制，文档写明：**多 state 同线程时，暂停会拖住同线程上其它 state 的调度**；真并行需多 OS 线程（V1 不保证跨线程安全，除非 asyncsocket/luadap 已假设单线程泵）
- **线程安全：** V1 保持「单线程调用 `update`/Lua」约定；多 OS 线程各跑一 state **不在 V1 范围**
- **单 state 兼容：** 未传 `name` 且仅一 state → 线程名 `main` / `coro-N`，行为与现网一致

---

## 验收标准

- [ ] 同进程两 `lua_State` 同 host/port 两次 `start`，VS Code / Python 看到两个 main thread（及各自协程）
- [ ] 一侧断点暂停时 `stopped.threadId` 对应该侧；另一侧 main 的 `stackTrace` 为空且不要求 `allThreadsStopped`
- [ ] 不同 port 第二次 `start` 失败
- [ ] 现有 `test/test_dap_*.py` 回归通过
- [ ] README 有多 state 用法小节段
