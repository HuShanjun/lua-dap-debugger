# Lua DAP Debugger

C/C++ **`luadap`** 提供标准 [DAP](https://microsoft.github.io/debug-adapter-protocol/) TCP 服务（断点、步进、locals / table、Watch / Hover / Debug Console REPL）。VS Code / Cursor 通过本仓库的 **`lua-dap` 扩展** 连接：

- **Launch：** 扩展拉起 **`lua-runner`**（已链接 Lua + `luadap`），调试当前 `.lua` 文件。不依赖系统 `lua.exe` 与 `luadap.dll` 的 ABI 是否匹配。
- **Attach：** 连到已调用 `dap.start` 的进程端口。游戏 / 自定义宿主仍须自行嵌入 `luadap`（通常 `bin/luadap.dll` + `require("luadap")`），并在主循环调用 `dap.update()`。仓库示例宿主为 **`sample`**（`bin/sample.exe`）。

DAP 协议、断点/步进/变量与拆帧全部在 **C**（`native/luadap`）实现。`sample/script/lua-runtime/debugger.lua` 仅作对照参考，不参与构建与默认测试路径。

推荐配置：`type: lua-dap`。仓库里仍保留一条旧的 `type: node` + `debugServer`（**Debug Sample**），仅方便直接附加到 `sample.exe`；Node 专有选项（如 `sourceMapPathOverrides`）**不会**生效。

---

## 项目结构

```
lua-dap-debugger/
├── sample/
│   ├── main.cpp                  # 示例宿主：luadap.start → sample/script → 循环 dap.update()
│   └── script/
│       ├── main.lua              # 多协程演示（DAP threads）
│       └── lua-runtime/          # 对照参考（非运行时）
├── native/
│   ├── base/                     # C 基础库：cjson、circle_buffer
│   ├── lua_compat/               # Lua 5.1–5.4 兼容层（独立静态库）
│   ├── asyncsocket/              # 异步 TCP（poll 线程；luadap 静态链接）
│   ├── luadap/                   # DAP：framing / session / lua_debug
│   └── lua-runner/               # Launch 用 CLI：链接 liblua + luadap_static
├── vscode-extension/             # type: lua-dap（Launch spawn runner / Attach 端口）
├── test/                         # Python DAP / asyncsocket 回归
├── 3rd/                          # Lua 5.x（CMake 输出到 bin/lua5.x）、sol2、nlohmann
├── bin/
│   ├── lua5.1/ … lua5.4/         # 各版本 liblua / lua / luac
│   └── …                         # luadap.dll、lua-runner.exe、sample 等
└── .vscode/launch.json           # Debug Sample / Extension Host / lua-dap Launch·Attach
```

---

## 一 DLL 接入（`luadap`）

```lua
local dap = require("luadap")
dap.start(host, port, true [, name])   -- true：阻塞到 DAP configurationDone；name 可选
-- 业务脚本 / 游戏循环
dap.update()                  -- 每帧调用
dap.track(co, "worker")       -- 可选；start 已包装 coroutine.create / wrap
```

- `package.cpath` 含 `bin/?.dll` 即可 `require("luadap")`。
- **不必**把 `lua-runtime` 放进 `package.path`（DAP 在 DLL 内，不 load Lua 调试脚本）。
- `start(..., true)`：等到 `configurationDone` 再返回。
- `start(..., false)`：立即返回，握手靠后续 `update()`（`sample` 宿主用此模式）。
- 协程自动登记为 DAP threads（`threadId=1` 为主线程）。`coroutine.create` / `coroutine.wrap` 在 `start` 时被包装；绕过包装时用 `dap.track(co, name?)`。

C++ 宿主等价写法见 `sample/main.cpp`：`require("luadap")` → `start(..., false)` → `RunFile(sample/script/main.lua)` → 循环 `update()`。

内部传输走通用 `asyncsocket` C API。DAP 仍只接受 **一个** 调试客户端：第二个入站连接会被关掉。`disconnect` 只结束当前 client，**不停 listen**，所以可再 F5 附加。

---

## 多 `lua_State` 共用 DAP

同一进程内多个独立 `lua_State` 可 **共用一次 DAP listen / 同一 VS Code 会话**。每个 state 各自调用 `dap.start(host, port, wait [, name])`：

- **首次** `start`：在本机 `host:port` 上 listen。
- **后续** 同 `host` + `port`：**自动 join** 已有会话（无需再开端口）。
- **`name`（可选）：** Threads 面板前缀。仅一个 state 且未传 `name` → 主线程名 `main`；多 state 未传 → `state-N`。
- **`(host, port)` 与已有会话不一致** → 失败。

```lua
-- Thread 1
dap.start("127.0.0.1", 8172, false, "logic")
-- Thread 2（join，不另开 listen）
dap.start("127.0.0.1", 8172, false, "ui")
```

各 state 在 **拥有它的 OS 线程** 上驱动 Lua 并调用 `dap.update()`。断点只暂停命中的执行流；**其它 OS 线程上的 state 继续真并行**。多个 DAP thread 可同时处于 stopped（`allThreadsStopped: false`）；`continue` / `step*` 须带正确的 `threadId`。

### 跨线程约定

| 规则 | 说明 |
|------|------|
| **一 L 一 OS 线程** | 每个 `lua_State`（含其协程）只由创建/驱动它的线程进入 Lua；其它线程不得对该 `L` 做 `lua_getstack` / evaluate。 |
| **持锁不进用户 Lua** | 会话互斥锁保护 DAP 共享结构；`pause_loop` 睡眠前必须放锁；禁止持锁调用用户脚本。 |
| **同线程暂停挡兄弟 state** | 同一 OS 线程上跑多个 state 时，一个 state 进入 `pause_loop` 会阻塞该线程上其它 state 的调度（非数据竞争，是调度限制）。需要真并行请用多 OS 线程。 |

回归测试：`test/test_dap_multi_state.py`（同线程双 state）、`test/test_dap_multi_state_mt.py`（跨线程双 state、并行心跳）。需先编译 `multi_state_dap_host`。

---

## 通用异步 TCP（`asyncsocket` 0.3）

独立模块 `bin/asyncsocket.dll`：`require("asyncsocket")`，Lua 对象为 **Server + Connection**。`pump()` 只在主线程 drain 事件并触发回调。V1 全进程一个 `listen`；`connect` 可多个。

```lua
local as = require("asyncsocket")

local srv = as.listen("127.0.0.1", 9000)
srv:on_accept(function(conn)
  conn:on_message(function(chunk)
    conn:send(chunk)  -- 原始字节，不组帧
  end)
  conn:on_close(function() end)
end)

local conn = as.connect("127.0.0.1", 9000)
conn:on_open(function()
  conn:send("ping")
end)
conn:on_message(function(chunk) end)
conn:on_close(function() end)

while running do
  as.pump()
  as.sleep(0.01)
end
```

`as._VERSION` 为 `"0.3.0"`。

---

## Lua 5.1–5.4 兼容

`asyncsocket` 与 `luadap` 支持 **Lua 5.1 / 5.2 / 5.3 / 5.4**（不含 LuaJIT）。CMake **必须**指定 `-DLUA_VERSION=`；`3rd/CMakeLists.txt` 单独编译对应解释器，产物输出到 **`bin/lua5.x/`**。

| 选项 | 含义 |
|------|------|
| `-DLUA_VERSION=` | `5.1` \| `5.2` \| `5.3` \| `5.4`（**必填**，编 vendored Lua 时） |
| `LUA_INC_DIR` | Lua 头文件目录（由 `3rd` 自动设置，也可 `-D` 覆盖） |
| `LUA_LIB_DIR` | Lua 库目录，默认 `bin/lua5.x`（`luadap` / `asyncsocket` 使用） |
| `-DLUA_SKIP_VENDOR=ON` | 与自定义 `LUA_INC_DIR` + `LUA_LIB_DIR` 一起，跳过编仓库内 Lua |

PowerShell 下请给版本号加引号，例如 `"-DLUA_VERSION=5.4"`，否则 `5.4` 会在点号处被拆开。

`sample`（sol2 宿主）与完整工具链建议用 **5.4**。非 5.4 可编 `lua` + `asyncsocket` + `luadap`（及 `lua-runner`）。

```powershell
cmake -S . -B build "-DLUA_VERSION=5.4"
cmake --build build --config Release --target lua-runner sample luadap asyncsocket

# Lua 产物：bin/lua5.4/liblua.lib、lua.exe、luac.exe
# 或 Ninja（见 build-ninja.bat）
.\build-ninja.bat 5.4
cmake --build build/ninja --target lua-runner sample
```

GitHub Actions：[`.github/workflows/lua-compat-matrix.yml`](.github/workflows/lua-compat-matrix.yml) 对四版本矩阵构建并跑冒烟测试。

---

## 快速开始（lua-dap 扩展）

### 1. 编译 `lua-runner`（及可选宿主）

```powershell
cmake -S . -B build "-DLUA_VERSION=5.4"
cmake --build build --config Release --target lua-runner
```

产物：`bin/lua-runner.exe`。CMake POST_BUILD 会复制到 `vscode-extension/bin/win32-x64/lua-runner.exe`（该路径通常不入库，干净克隆后须编一次）。也可手动：

```powershell
powershell -File vscode-extension/scripts/copy-runner.ps1
```

Attach 示例宿主另编 `sample` + `luadap`：

```powershell
cmake --build build --config Release --target sample luadap
```

`lua-runner` CLI：`lua-runner [--host HOST] [--port PORT] [--] <program.lua> [script_args...]`

### 2. 编译扩展

```powershell
cd vscode-extension
npm install
npm run compile
```

本仓库调试：打开本仓库，选 **`1. Start Extension Host (do this first)`** 再 F5，在 Extension Development Host 中加载 `vscode-extension/`。也可把扩展安装到 Cursor/VS Code extensions，或设 `luadap.runnerPath` 指向自编的 `lua-runner`。

Runner 解析顺序：launch 的 `runnerPath` → 设置 `luadap.runnerPath` → 扩展内置 `bin/win32-x64/lua-runner.exe`。仓库 Launch 配置显式指向 `${workspaceFolder}/bin/lua-runner.exe`。

### 3. Launch 当前文件

在已加载 `lua-dap` 扩展的窗口打开要调试的 `.lua`，F5 选 **`2. Launch Lua file`**。扩展会选空闲端口、spawn runner、等 `listening on` 后再连 DAP。

Launch **不再**需要系统 `lua.exe` 与 `luadap.dll` ABI 一致；runner 自带匹配的 liblua 并静态链接 `luadap`。

### 4. Attach 到已 listen 的宿主

```powershell
.\bin\sample.exe
```

默认端口 `8172`（`LUADAP_HOST` / `LUADAP_PORT` 可覆盖）。控制台出现 listening 后：

- 推荐：F5 选 **`3. Attach DAP`**（`type: lua-dap`，`127.0.0.1:8172`）
- 或：选 **Debug Sample**（`type: node` + `debugServer: 8172`，旧入口）

Attach **不**使用 `lua-runner`。

### 5. 断点与变量

在 `sample/script/main.lua` 中标有 `BREAKPOINT` 的行打断点（如 `worker_alpha` / `worker_beta` 循环内），查看 Variables / Call Stack（多协程对应多个 DAP threads）。

Continue / Step Over / Step Into / Step Out 可用。Watch 与 Hover 求值只读表达式；Debug Console（`context=repl`）可执行语句并把赋值写回 local / upvalue。

仓库 `.vscode/launch.json` 当前大致为：

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "Debug Sample",
      "type": "node",
      "request": "attach",
      "debugServer": 8172
    },
    {
      "name": "1. Start Extension Host (do this first)",
      "type": "extensionHost",
      "request": "launch",
      "args": [
        "--extensionDevelopmentPath=${workspaceFolder}/vscode-extension"
      ]
    },
    {
      "name": "2. Launch Lua file (use in Extension Host window)",
      "type": "lua-dap",
      "request": "launch",
      "program": "${file}",
      "cwd": "${workspaceFolder}",
      "runnerPath": "${workspaceFolder}/bin/lua-runner.exe"
    },
    {
      "name": "3. Attach DAP (use in Extension Host window)",
      "type": "lua-dap",
      "request": "attach",
      "host": "127.0.0.1",
      "port": 8172
    }
  ]
}
```

---

## 自动化冒烟（可选）

完整说明见 [`test/README.md`](test/README.md)（**每个用例作用**、编译、一键跑全量）。

```powershell
# 一键跑全部回归
.\test\run_all_tests.ps1
# 或
python test/run_all_tests.py
```

```powershell
cd E:\demo\lua-dap-debugger
$env:PATH = "$pwd\bin;$env:PATH"
python test/test_asyncsocket_smoke.py
python test/test_asyncsocket_multi.py
python test/test_asyncsocket_connect.py
python test/test_dap_luadap_handshake.py
python test/test_dap_luadap_nowait.py
python test/test_dap_luadap_reconnect.py
python test/test_dap_handshake.py
python test/test_dap_breakpoint.py
python test/test_dap_step.py
python test/test_dap_disconnect.py
python test/test_dap_partial_frame.py
python test/test_dap_condition.py
python test/test_dap_evaluate.py
python test/test_dap_table_cycle.py
python test/test_dap_coro_threads.py
python test/test_dap_coro.py
python test/test_dap_runner_handshake.py
python test/test_dap_multi_state.py
python test/test_dap_multi_state_mt.py
```

测试脚本期望 `bin/lua5.x/lua.exe`（或 `bin/lua.exe`）、`bin/luadap.dll`、`bin/asyncsocket.dll`（及 runner 测试用的 `bin/lua-runner.exe`）已就绪。

---

## 已知限制

- 已 strip debug info 的字节码看不到变量；LuaJIT `-O2` 下部分 local 可能被优化
- V1 不做 pathMappings（同机路径规范化即可）；`sourceMapPathOverrides` 对 `debugServer` 附加无效
- 协程映射为 DAP threads：`start` 时包装 `coroutine.create` / `coroutine.wrap`；绕过包装的创建需 `dap.track(co, name?)`。未暂停协程的 `stackTrace` 为空栈。本轮不做 DAP `pause` 请求。
- 扩展内置 `lua-runner.exe` 通常不入库；干净克隆须先编 `lua-runner`（或 `copy-runner.ps1`）再 Launch
- 非 Windows 无预编译 runner，须自编并配置 `runnerPath` / `luadap.runnerPath`
- `luadap` 不导出 `shutdown`；进程退出由宿主负责
- VS Code 对 `request: launch` 会向 DAP 发 `launch` 命令；`luadap` 将其按 `attach` 处理（进程已由扩展 / 宿主拉起）
