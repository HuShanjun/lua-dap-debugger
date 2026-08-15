# Lua DAP Debugger

C++ **`luadap`** 提供标准 [DAP](https://microsoft.github.io/debug-adapter-protocol/) TCP 服务（断点、步进、locals / table、Watch / Hover / Debug Console REPL）。VS Code 通过本仓库的 **`lua-dap` 扩展** 连接：

- **Launch：** 扩展拉起 **`lua-runner`**（已链接 Lua + `luadap`），调试当前 `.lua` 文件。不依赖系统 `lua.exe` 与 `luadap.dll` 的 ABI 是否匹配。
- **Attach：** 连到已调用 `dap.start` 的进程端口。游戏 / 自定义宿主仍须自行嵌入 `luadap`（通常 `bin/luadap.dll` + `require("luadap")`），并在主循环调用 `dap.update()`。

DAP 协议、断点/步进/变量与拆帧全部在 **C/C++**（`native/luadap`）实现。`script/lua-runtime/debugger.lua` 仅作对照参考，不参与构建与默认测试路径。

旧的 `type: node` + `debugServer` 配置已废弃，请改用 `type: lua-dap`。

---

## 项目结构

```
lua-dap-debugger/
├── main/main.cpp                 # 示例宿主：luadap.start → sample → 循环 dap.update()
├── tools/lua-runner/             # Launch 用 CLI：链接 liblua + luadap
├── native/luadap/                # C++ DAP：framing / session / lua_debug
├── native/asyncsocket/           # 异步 TCP（poll 线程；luadap 静态链接）
├── vscode-extension/             # type: lua-dap（Launch spawn runner / Attach 端口）
├── 3rd/cJSON/                    # 编进 luadap
├── script/lua-runtime/
│   ├── debugger.lua              # 对照参考（非运行时）
│   └── dkjson.lua
├── script/sample/main.lua        # 演示 locals + nested table
├── script/test/                  # Python DAP 回归
├── bin/                          # 编译产物：lua-runner.exe、main.exe、luadap.dll、lua.exe
└── .vscode/launch.json           # type: lua-dap（Launch / Attach）+ Extension Host
```

---

## 一 DLL 接入（`luadap`）

```lua
local dap = require("luadap")
dap.start(host, port, true)   -- true：阻塞到 DAP configurationDone
-- 业务脚本 / 游戏循环
dap.update()                  -- 每帧调用
dap.track(co, "worker")       -- 可选；start 已包装 coroutine.create / wrap
```

- `package.cpath` 含 `bin/?.dll` 即可 `require("luadap")`。
- **不必**把 `script/?.lua` 放进 `package.path`（DAP 在 DLL 内，不 load Lua 调试脚本）。
- `start(..., true)`：等到 `configurationDone` 再返回（与旧 `debugger.listen` 一致）。
- `start(..., false)`：立即返回，握手靠后续 `update()`。
- 协程自动登记为 DAP threads（`threadId=1` 为主线程）。`coroutine.create` / `coroutine.wrap` 在 `start` 时被包装；绕过包装时用 `dap.track(co, name?)`。

C++ 宿主等价写法见 `main/main.cpp`：`require("luadap")` → `start(..., true)` → `RunFile(sample)` → 循环 `update()`。

内部传输已切到通用 `asyncsocket` C API（`as_listen` / `as_conn_send` / `as_take_events`）。DAP 仍只接受 **一个** 调试客户端：第二个入站连接会被关掉。`disconnect` 只结束当前 client，**不停 listen**，所以 VS Code 可再 F5 附加。

---

## 通用异步 TCP（`asyncsocket` 0.3）

独立模块 `bin/asyncsocket.dll`：`require("asyncsocket")`，Lua 对象为 **Server + Connection**。`pump()` 只在主线程 drain 事件并触发回调。V1 全进程一个 `listen`；`connect` 可多个。

```lua
local as = require("asyncsocket")

-- Server
local srv = as.listen("127.0.0.1", 9000)
srv:on_accept(function(conn)
  conn:on_message(function(chunk)
    conn:send(chunk)  -- 原始字节，不组帧
  end)
  conn:on_close(function() end)
end)
-- srv:close() 只停 listen；已接受的 connection 仍保留

-- Client
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

`as._VERSION` 为 `"0.3.0"`。旧 listen 对象上的 `on_open`/`on_message` 已移除（破坏性）。

---

## Lua 5.1–5.4 兼容

`asyncsocket` 与 `luadap` 支持 **Lua 5.1 / 5.2 / 5.3 / 5.4**（不含 LuaJIT）。本地若存在 `3rd/lua-5.4.8` 则默认用该 vendored 树；`/3rd` 被 gitignore，CI 干净检出没有这份源码，5.4 与其它版本一样走 FetchContent。GitHub Actions 工作流 [`.github/workflows/lua-compat-matrix.yml`](.github/workflows/lua-compat-matrix.yml) 对四版本各编一次并跑最低套件（asyncsocket smoke、DAP handshake、evaluate、condition）。

CMake 选项：

| 选项 | 含义 |
|------|------|
| `-DLUA_VERSION=` | `5.1` \| `5.2` \| `5.3` \| `5.4`（默认 `5.4`） |
| `-DLUA_ROOT=` | 可选；已安装前缀（`include/` + `lib/`）或官方风格源码树 |

解析顺序：`LUA_ROOT` → 本地 `3rd/lua-5.4.8`（若存在）→ 否则 FetchContent 拉取 lua.org 官方 tarball 到 **build 目录**（不进 git）。PowerShell 下请给版本号加引号，例如 `"-DLUA_VERSION=5.4"`，否则 `5.4` 会在点号处被拆开。

`main`（sol2 宿主）与 `luasocket` **仅 5.4** 加入构建。非 5.4 只编 `lua` + `asyncsocket` + `luadap`；产物写到 `${CMAKE_BINARY_DIR}/bin`，避免覆盖默认的源码树 `bin/lua.exe`。

```powershell
cmake -S . -B build/msvc "-DLUA_VERSION=5.4"
cmake --build build/msvc --target main luadap asyncsocket --config Debug

cmake -S . -B build/lua51 "-DLUA_VERSION=5.1"
cmake --build build/lua51 --target lua asyncsocket luadap --config Release
```

无网络或需固定源码时用 `-DLUA_ROOT=` 指向本机 Lua，而不是 FetchContent。

---

## 快速开始（lua-dap 扩展）

### 1. 编译 `lua-runner`（及可选宿主）

```powershell
cmake --build E:\demo\lua-dap-debugger\build\msvc --target lua-runner --config Debug
```

产物：`bin/lua-runner.exe`。CMake POST_BUILD 会复制到 `vscode-extension/bin/win32-x64/lua-runner.exe`（该路径 gitignore，干净克隆后必须编一次）。也可手动：

```powershell
powershell -File vscode-extension/scripts/copy-runner.ps1
```

Attach 示例宿主另编 `main` + `luadap`：

```powershell
cmake --build E:\demo\lua-dap-debugger\build\msvc --target main luadap --config Debug
```

`lua-runner` CLI：`lua-runner [--host HOST] [--port PORT] [--] <program.lua> [script_args...]`

### 2. 编译扩展

```powershell
cd vscode-extension
npm install
npm run compile
```

本仓库调试：在 VS Code 打开本仓库，选 **Extension Host: lua-dap** 再 F5，会在 Extension Development Host 中加载 `vscode-extension/`。其它工作区：把扩展目录拷到 VS Code extensions，或设 `luadap.runnerPath` 指向自编的 `lua-runner`。

Runner 解析顺序：launch 的 `runnerPath` → 设置 `luadap.runnerPath` → 扩展内置 `bin/win32-x64/lua-runner.exe`。仓库 `.vscode/launch.json` 的 Launch 配置显式指向 `${workspaceFolder}/bin/lua-runner.exe`。

### 3. Launch 当前文件

在 Extension Development Host（或已安装扩展的窗口）打开要调试的 `.lua`，F5 选 **Lua DAP: Launch current file**。扩展会选空闲端口、spawn runner、等 listen 后再连 DAP。

Launch **不再**需要系统 `lua.exe` 与 `luadap.dll` ABI 一致；runner 自带匹配的 liblua 并静态链接 `luadap`。

### 4. Attach 到已 listen 的宿主

宿主仍须嵌入 `luadap` 并泵事件（与 `main/main.cpp` 相同）：

```powershell
.\bin\main.exe
```

默认端口 `8172`（`LUADAP_HOST` / `LUADAP_PORT` 可覆盖）。控制台出现 listening 后，F5 选 **Lua DAP: Attach**（`127.0.0.1:8172`）。Attach **不**使用 `lua-runner`。

### 5. 断点与变量

在 `script/sample/main.lua` 的 `local sum = add(x, y)` 一行打断点，查看 Variables：

- locals：`player` / `x` / `y` / `sum`
- 展开 `player` 可见 `name`、`stats`；再展开 `stats` 可见 `hp`、`mp`

Continue / Step Over / Step Into / Step Out 可用。Watch 与 Hover 求值只读表达式；Debug Console（`context=repl`）可执行语句并把赋值写回 local / upvalue。停止调试后宿主应继续跑完或正常退出，不应卡死。

仓库 `.vscode/launch.json`（旧 `type: node` + `debugServer` 已删除）：

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "Lua DAP: Launch current file",
      "type": "lua-dap",
      "request": "launch",
      "program": "${file}",
      "cwd": "${workspaceFolder}",
      "runnerPath": "${workspaceFolder}/bin/lua-runner.exe"
    },
    {
      "name": "Lua DAP: Attach",
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

```powershell
cd E:\demo\lua-dap-debugger
python script/test/test_asyncsocket_smoke.py
python script/test/test_asyncsocket_multi.py
python script/test/test_asyncsocket_connect.py
python script/test/test_dap_luadap_handshake.py
python script/test/test_dap_luadap_nowait.py
python script/test/test_dap_luadap_reconnect.py
python script/test/test_dap_handshake.py
python script/test/test_dap_breakpoint.py
python script/test/test_dap_step.py
python script/test/test_dap_disconnect.py
python script/test/test_dap_partial_frame.py
python script/test/test_dap_condition.py
python script/test/test_dap_evaluate.py
python script/test/test_dap_table_cycle.py
python script/test/test_dap_coro_threads.py
python script/test/test_dap_coro.py
```

全部走 `require("luadap")` + `bin/?.dll`，**不**依赖磁盘 `lua-runtime`。

---

## 已知限制

- 已 strip debug info 的字节码看不到变量；LuaJIT `-O2` 下部分 local 可能被优化
- V1 不做 pathMappings（同机路径规范化即可）
- 协程映射为 DAP threads：`start` 时包装 `coroutine.create` / `coroutine.wrap`；绕过包装的创建需 `dap.track(co, name?)`。未暂停协程的 `stackTrace` 为空栈。本轮不做 DAP `pause` 请求。
- 扩展内置 `lua-runner.exe` gitignore；干净克隆须先编 `lua-runner`（或 `copy-runner.ps1`）再 Launch
- 非 Windows 无预编译 runner，须自编并配置 `runnerPath` / `luadap.runnerPath`
- `luadap` 不导出 `shutdown`；进程退出由宿主负责
- 已废弃：`type: node` + `debugServer`（请用 `type: lua-dap`）
