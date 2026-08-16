# 多 lua_State 共用 DAP 会话 — 设计

**日期：** 2026-08-16  
**状态：** 已批准待实现  
**范围：** `native/luadap`（`dap_session` / `coro_registry` / `lua_debug` / `luadap.c`）、必要时 `asyncsocket` 泵约定、宿主用法、测试；VS Code 扩展无需改协议面（仍一个 TCP DAP）

---

## 背景与目标

当前 `luadap` 进程内只有一个全局 `dap_session`，`coro_registry` 挂在单一 `g_mainL` 上，且假定单线程泵 DAP。宿主若创建多个独立 `lua_State`（可在不同 OS 线程），无法安全地共用一次调试会话。

**目标：**

1. 同一进程、同一 DAP listen / 同一 VS Code 会话内，注册多个 main `lua_State`。  
2. Threads 扁平展示各 state 的主线程及其协程。  
3. 断点只暂停命中的执行流；**其它 OS 线程上的其它 state 继续真并行**。  
4. **跨 OS 线程** 调用 `start` / `update` / `track` / hook 暂停路径 **无数据竞争**（方案 A：会话短锁 + 无锁睡眠）。

**非目标（本版仍不做）：**

- 多个 DAP listen / 多个 VS Code 会话并行  
- `allThreadsStopped: true`（全局冻结）  
- 跨 state 的 variables / evaluate（变量域仍绑定暂停中的那个 `L`）  
- 改变 `asyncsocket`「全进程一个 listen」；不引入专用 DAP 泵线程（方案 B）  
- 同一 `lua_State*` 被多 OS 线程 concurrently 进入（仍遵守 Lua 规则：一 L 一拥有线程）

---

## 已锁定决策

| 项 | 选择 |
|----|------|
| 产品形态 | 多 VM **共用一个** DAP 会话 |
| 暂停语义 | 只停触发 hook 的执行流；**其它 OS 线程上的 state 继续跑** |
| 多暂停 | **允许** 多个 DAP thread 同时处于 stopped（多次 `stopped`，`allThreadsStopped: false`） |
| Threads | **扁平**：mainL + 协程；名称带 state 前缀 |
| 注册 API | 每 state 均可 `dap.start`；**同 host+port 自动 join**；不同 port → 错误 |
| 跨线程 | **方案 A**：会话互斥锁保护共享结构；`pause_loop` **不持锁睡眠**；禁止持锁调 Lua |
| 泵 DAP | 任一线程的 `dap.update()` 或任一 `pause_loop` 内的 `update`；短锁串行化 |

---

## API

### Lua

```lua
dap.start(host, port, wait [, name])  -- 首 call 建 listen；同 host/port join
dap.update()                          -- 任意已注册 state、任意拥有线程可调
dap.track(co [, name])
```

- `name`（可选）：Threads 前缀；仅一个 state 且未传 name → 主线程名 `main`；多 state 未传 → `state-N`
- `wait=true`：会话尚未 `configurationDone` 则阻塞等待；已 configured 则立即返回
- `(host,port)` 与已有会话不一致 → 失败

### 行为不变 / 强化

- 仍只接受 **一个** DAP client  
- `continue` / `next` / `stepIn` / `stepOut`：**必须按 `threadId` 只恢复/步进对应暂停流**（省略或错误 id → 失败或仅当唯一暂停时兼容，实现时写死：**推荐要求匹配 paused 集合中的 id**）  
- `stackTrace`：对该 thread 若在暂停集合中则出栈，否则空栈  

---

## 线程安全模型（方案 A）

### 锁

- 一把进程级 **`session_mutex`**（可递归或文档禁止重入；推荐 **非递归** + 明确临界区不嵌套）。  
- **持锁允许：** 改 `dap_session`、`state_registry`、`coro_registry` 元数据、断点表、组 DAP 帧、`as_conn_send` / `as_take_events` 消费（若 asyncsocket API 非线程安全，则 **所有** asyncsocket 调用也仅在持锁下进行）。  
- **持锁禁止：** 任何 `lua_*` 进入用户脚本 / `lua_pcall` / 读任意 `lua_State` 的栈做 evaluate（evaluate/stack 在放锁后、且仅在该 L 的拥有线程上执行——见下）。

### 所有权

- 每个 `lua_State`（含其协程）只由 **创建/驱动它的 OS 线程** 调用 Lua 与安装 hook。  
- 其它线程不得对该 `L` 调 `lua_getstack` / `evaluate`；DAP 请求若需要读栈，必须 **投递到拥有线程** 或仅在该线程已处于 `pause_loop`、由该线程自己在放锁后处理。  

**本版采用的读栈策略（与现单线程兼容、多线程正确）：**

- `stackTrace` / `scopes` / `variables` / `evaluate` 只服务 **已在暂停集合中的 threadId**。  
- 处理这些请求时：持锁校验 id ∈ paused，取出 `L` 指针后 **释放锁**，在 **当前正在执行 `pause_loop` 的那条 OS 线程** 上跑 Lua（即：请求必须由该暂停线程的 `update`/`pause_loop` 路径处理，而不是由另一线程的 `update` 直接摸别人的 `L`）。  
- 实现要点：`update` 在持锁下只把入站 DAP 请求放入队列；**真正 `dispatch` 里碰 Lua 的命令**，若 target `L` 不是「本线程正在 pause 的 L」，则留在队列，等拥有线程的 `pause_loop`/`update` 再取。不碰 Lua 的命令（`initialize`、`setBreakpoints`、`threads`、`continue`、`disconnect` 等）可在任意 `update` 持锁路径执行。

### Hook → 暂停

```
on_line_hook(L):
  快速判断（可持锁读断点/step 标志，立刻放锁）
  若需停:
    持锁: 加入 paused 集合，分配/确认 threadId，发送 stopped 事件
    放锁
    pause_loop(L):
      while 本 thread 仍在 paused 集合:
        持锁: 泵 asyncsocket + 处理「非 Lua 或本 L 的」DAP 请求
        放锁
        若仍 paused: 短睡或 condvar 等（continue 时 signal）
      持锁: 清本 thread 的 step/paused 项（若 continue 已清则可跳过）
      放锁
      return 到 Lua
```

- **不持锁睡眠**，故其它线程可继续跑 Lua、可再断下、可 `update`。  
- 多个 `pause_loop` 同时跑时，靠 `session_mutex` 串行化泵与发送。

### `continue` / `step*`

- 持锁：按 `threadId` 从 paused 集合移除（或设 step 标志仅绑定该 `L`），`cond_signal` 对应等待者。  
- 不匹配的 `threadId` → 失败响应。  
- 旧客户端不传 `threadId`：若 paused 恰有 1 个则作用其上，否则失败（写进实现与测试）。

---

## 内部模型

### `state_registry`

```
state { id, mainL, name[64] }
```

- `start`/join 追加；同 `mainL` 重复 start → no-op  
- 所有访问在 `session_mutex` 下  

### `coro_registry`

- entries 带归属 `mainL` / `state_id`  
- 每 mainL 登记 main 线程 + 包装该 L 上的 `coroutine.create`/`wrap`  
- `threadId` 全局单调唯一  
- 显示名：`{stateName}` / `{stateName}/{coroName}`；单 state 无 name 时主线程为 `main`  

### `dap_session`

- 单例 + `listen_host` / `listen_port`  
- **`paused` 从单槽改为集合**：`threadId → { L, condvar/世代 }`  
- `step` 绑定具体 `L` + threadId（可多 thread 各有 step，或 V1 步进仍只对「刚操作的」那个；**推荐每 paused thread 独立 step 槽**）  
- 断点表会话共享，持锁读写  

### Disconnect / re-attach

- Client disconnect：持锁唤醒并清空 **全部** paused；清断点/recv；对所有 mainL 清 hook（清 hook 须在各拥有线程？`lua_sethook` 对非运行中 L 通常可从他线程调用，但为稳妥：**持锁只标 `hooks_armed=0`，各线程下一次 `update`/进 hook 前见标志则 `clear_hook`**；或 disconnect 路径文档要求宿主停妥后单线程调用——**推荐：disconnect 时持锁设标志，各 `pause_loop`/`update` 在本线程对所属 L 卸 hook**）  
- 再 attach / `configurationDone` 后各线程本 L 再 `install_hook`  
- 进程 shutdown：唤醒所有等待者，卸包装，清 registry，关 listen  

---

## 宿主用法示例

```cpp
// Thread 1
lua_State* A = luaL_newstate();
// require luadap; dap.start("127.0.0.1", 8172, false, "logic")
for (;;) { /* run A */ dap.update via A; }

// Thread 2
lua_State* B = luaL_newstate();
// dap.start("127.0.0.1", 8172, false, "ui")  -- join
for (;;) { /* run B */ dap.update via B; }
```

同线程多 state 仍可用，但一 state 进入 `pause_loop` 会挡住该线程上其它 state 的调度（不是数据竞争，是调度限制）。

`sample` 不强制改；以测试宿主为准。

---

## 测试

1. **同线程双 state：** `multi_state_dap_host`（单线程轮询 A/B）— threads 名、join、port mismatch  
2. **跨线程双 state：** 两 OS 线程各一 L；A 断下后 B 用原子心跳证明仍在跑；再让 B 断下 → 两个 stopped；分别 `continue`  
3. **并发 `update`：** 两线程同时猛调 `update` + 一侧 pause，ASAN/TSan（若 CI 有）或压力下无崩溃、DAP 仍可 continue  
4. **错误 port join** 失败  
5. 现有 `test/test_dap_*.py` 回归  

---

## 实现分期（建议）

1. `session_mutex` + 临界区梳理（先单 paused 行为不变，通回归）  
2. `paused` 集合 + 按 `threadId` 的 continue/step + `pause_loop` 无锁等  
3. DAP 请求队列：碰 Lua 的命令仅在拥有/暂停线程 dispatch  
4. `state_registry` + `start` join + 多 main hook / coro 前缀名  
5. 同线程 + 跨线程测试宿主与 Python 用例  
6. README：多 state、跨线程约定、同线程暂停挡调度说明  

---

## 风险与注意

- **asyncsocket：** 若底层非线程安全，必须把所有 `as_*` 放进 `session_mutex`；评估 poll 线程与主泵的现有约定是否已被锁覆盖。  
- **死锁：** 禁止 `lua_pcall` → 用户代码 → 再 `dap.update` 时持锁；`pause_loop` 必须先放锁。  
- **Lua 所有权：** 绝不以线程 B 的 `update` 去 `lua_getstack(A)`。  
- **同 OS 线程多 state：** 暂停仍会卡住同线程兄弟 state——文档写明；真并行用多线程。  
- **VS Code：** 多 stopped 时 UI 以当前 thread 为准；扩展侧无需改。  

---

## 验收标准

- [ ] 同进程两 `lua_State` 同 host/port 两次 `start`，Threads 可见两个 main（及协程）  
- [ ] **两 OS 线程** 下 A 暂停时 B 心跳仍前进；A、B 可同时处于 stopped  
- [ ] `continue`/`step` 按 `threadId` 只恢复对应流  
- [ ] 不同 port 第二次 `start` 失败  
- [ ] 双线程压力 `update` 下无数据竞争导致的崩溃/协议错乱（至少 Debug 构建 + 回归）  
- [ ] 现有 `test/test_dap_*.py` 通过  
- [ ] README 含多 state 与跨线程用法/限制  

---

## 修订记录

- 2026-08-16：初版（多 state join；当时写明跨线程非目标）  
- 2026-08-16：纳入跨线程方案 A、多 paused 集合、碰 Lua 请求的线程亲和 dispatch  
