# `test/` 测试说明

本文档说明本目录下**每个测试用例的作用**，以及如何**一键跑完全部回归**。

以仓库根目录为工作目录（例如 `E:\demo\lua-dap-debugger`）。

---

## 一键运行所有测试

### 方式 A：脚本（推荐）

先按下方「编译」准备好 `bin/`，然后：

```powershell
# PowerShell
.\test\run_all_tests.ps1
```

或：

```powershell
python test/run_all_tests.py
```

脚本会：

1. 把 `bin/` 加到 `PATH`
2. 按固定顺序跑全部 `test/test_*.py`
3. 再跑 `bin/coro_registry_test.exe`、`bin/circle_buffer_test.exe`（若存在）
4. 遇失败立即停止并返回非 0；全部通过打印汇总

可选参数（Python）：

```powershell
python test/run_all_tests.py --skip-c          # 只跑 Python
python test/run_all_tests.py --only dap        # 名称含 dap 的 Python 用例
python test/run_all_tests.py --only asyncsocket
python test/run_all_tests.py --list            # 只列出将要跑的项
```

### 方式 B：一行 PowerShell

```powershell
$env:PATH = "$pwd\bin;$env:PATH"
Get-ChildItem test\test_*.py | Sort-Object Name | ForEach-Object {
  Write-Host "=== $($_.Name) ==="
  python $_.FullName
  if ($LASTEXITCODE -ne 0) { throw "failed: $($_.Name)" }
}
.\bin\coro_registry_test.exe
.\bin\circle_buffer_test.exe
```

---

## 编译（跑测前）

```powershell
cmake -S . -B build/msvc "-DLUA_VERSION=5.4"   # PowerShell 必须给版本号加引号
cmake --build build/msvc --config Release --target `
  lua luadap asyncsocket lua-runner multi_state_dap_host `
  coro_registry_test circle_buffer_test
```

**ABI 一致：** `lua.exe` / `luadap.dll` / `asyncsocket.dll` / `lua-runner.exe` / `multi_state_dap_host.exe` 必须来自同一次 `LUA_VERSION` 配置，不要混用 `build/` 与 `build/msvc` 编出的不同版本。

| 产物 | 谁在用 |
|------|--------|
| `bin/lua5.x/lua.exe`（或 `bin/lua.exe`） | 多数 Python DAP / asyncsocket 用例 |
| `bin/luadap.dll` | `require("luadap")` |
| `bin/asyncsocket.dll` | asyncsocket 用例 |
| `bin/lua-runner.exe` | `test_dap_runner_handshake.py` |
| `bin/multi_state_dap_host.exe` | 多 state 用例 |
| `bin/coro_registry_test.exe` | C：协程/多 main 命名 |
| `bin/circle_buffer_test.exe` | C：环形缓冲 |

---

## Python 用例逐项说明

### asyncsocket（传输层）

| 文件 | 作用 |
|------|------|
| `test_asyncsocket_smoke.py` | listen → 分片收包 → echo → close/join → 再 listen，验证 `pump` 主路径 |
| `test_asyncsocket_multi.py` | 两个 TCP 客户端同时连，按连接分别 echo |
| `test_asyncsocket_connect.py` | Lua 侧 `listen` + `connect`，经 `pump` 完成 ping/pong |

配套 Lua：`run_asyncsocket_*.lua`（由上述脚本拉起，不要单独当通过标准）。

### DAP / luadap（调试协议）

| 文件 | 作用 |
|------|------|
| `test_dap_luadap_handshake.py` | 经 `luadap.dll` 完成 initialize / attach / configurationDone；`package.path` 清空，证明不依赖磁盘 `lua-runtime` |
| `test_dap_luadap_nowait.py` | `dap.start(..., false)` + 宿主循环 `update()` 的非阻塞启动路径 |
| `test_dap_luadap_reconnect.py` | 客户端 disconnect 后，同一 listen 端口可再次 attach（F5 重连） |
| `test_dap_handshake.py` | 内嵌短脚本的最小 DAP 握手（对照/补充） |
| `test_dap_breakpoint.py` | 行断点命中；Variables 可见 locals / 嵌套 table |
| `test_dap_step.py` | continue / next（及错误 `threadId` 拒绝等步进相关行为） |
| `test_dap_disconnect.py` | 暂停中 disconnect、客户端掉线、握手后断开等生命周期 |
| `test_dap_partial_frame.py` | Content-Length 半包/粘包：不完整帧不解析，拼齐后再 dispatch |
| `test_dap_condition.py` | 条件断点：仅表达式为真时 stopped |
| `test_dap_evaluate.py` | Watch / Hover / REPL：求值、写回 local、失败路径 |
| `test_dap_table_cycle.py` | Variables 展开循环引用 / 共享引用 table，不死循环 |
| `test_dap_coro_threads.py` | `threads` 列出 `main/main` 与 `coroutine.create` 包装出的协程线程 |
| `test_dap_coro.py` | 断点打在协程内时，`stopped.threadId` 为该协程；主线程 stack 为空 |
| `test_dap_runner_handshake.py` | 不经过 `luadap.dll`，用静态链接的 `lua-runner` 完成 DAP 握手（含 launch 语义） |
| `test_dap_multi_state.py` | 同 OS 线程双 `lua_State`：Threads 见 `logic/main` 与 `ui/main`；A 停 B 空栈；取消断点时路径大小写不一致也能清掉 |
| `test_dap_multi_state_mt.py` | 两 OS 线程各一 state：A 暂停时 B 心跳仍前进；可双 stopped；按 `threadId` 分别 continue |

配套 debugee：`run_debugee*.lua`、`run_ms_a.lua` / `run_ms_b.lua`。

### 非主回归（默认一键脚本不跑）

| 文件 | 作用 |
|------|------|
| `protocol_test.py` | 早期协议玩具（自模拟前后端），**不是** 当前 `luadap` 回归路径 |
| `dap_client.py` | DAP TCP 客户端库，被其它用例 import |

---

## C 单测说明

| 可执行文件 | 源码 | 作用 |
|------------|------|------|
| `coro_registry_test.exe` | `native/luadap/coro_registry_test.c` | 多 mainL 登记、`main/main` / `ui/main` 命名、threadId 不冲突 |
| `circle_buffer_test.exe` | `native/base/circle_buffer/circle_buffer_test.c` | `circle_buffer` 创建销毁、raw/framed push/pop、多块、空闲回收等 |

```powershell
.\bin\coro_registry_test.exe
.\bin\circle_buffer_test.exe
```

---

## 端口与并发注意

各 Python 用例使用**固定本机端口**（如 18172、18180、18210、18290…）。若失败并提示连接拒绝/超时，先结束残留的 `lua.exe`、`lua-runner.exe`、`multi_state_dap_host.exe`。

一键脚本**顺序串行**执行，避免端口冲突。

---

## 常见失败

| 现象 | 处理 |
|------|------|
| `lua.exe not found` | 先编 `lua` 目标 |
| `multi_state_dap_host not found` | 编 `multi_state_dap_host` |
| `lua-runner not found` | 编 `lua-runner` |
| require luadap / DLL 错误 | 检查 `bin/luadap.dll` 与 `lua.exe` 是否同 Lua 版本 |
| Connection refused | 端口占用或宿主未 listen |

---

## CI

`.github/workflows/lua-compat-matrix.yml` 跑矩阵子集（smoke / handshake / evaluate / condition）。本地完整集以本目录一键脚本为准。
