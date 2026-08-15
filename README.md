# Lua DAP Debugger

C++ 宿主通过 **`bin/luadap.dll`** 提供标准 [DAP](https://microsoft.github.io/debug-adapter-protocol/) TCP 服务；VS Code 用内置 `debugServer` 直连，即可断点、步进、查看 locals / table 成员。

DAP 协议、断点/步进/变量与拆帧全部在 **C/C++**（`native/luadap`）实现，静态链接 `asyncsocket` 与 cJSON。部署只需 **`luadap.dll`**，不需要旁路 `debugger.lua`、`dkjson.lua` 或独立 `asyncsocket.dll`。`script/lua-runtime/debugger.lua` 仅作对照参考，不参与构建与默认测试路径。

宿主在主循环中调用 **`dap.update()`**（泵网络事件、拆帧、dispatch），以便运行中处理 disconnect 等事件。

V1 **不需要** `vscode-extension/`（可保留但不参与调试流程）。

---

## 项目结构

```
lua-dap-debugger/
├── main/main.cpp                 # 宿主：luadap.start → sample → 循环 dap.update()
├── native/luadap/                # C++ DAP：framing / session / lua_debug
├── native/asyncsocket/           # 异步 TCP（poll 线程；luadap 静态链接）
├── 3rd/cJSON/                    # 编进 luadap.dll
├── script/lua-runtime/
│   ├── debugger.lua              # 对照参考（非运行时）
│   └── dkjson.lua
├── script/sample/main.lua        # 演示 locals + nested table
├── script/test/                  # Python DAP 回归（全部 require("luadap")）
├── bin/                          # 编译产物：main.exe、luadap.dll、lua.exe
└── .vscode/launch.json           # type: node + debugServer: 8172
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

---

## 快速开始（V1 真实流程）

### 1. 编译宿主

用仓库已有的 CMake / MSVC 流程编译，确保以下文件可用：

- `bin/main.exe`
- `bin/luadap.dll`（`require("luadap")`）

示例（本机已配置过 `build/msvc` 时）：

```powershell
cmake --build E:\demo\lua-dap-debugger\build\msvc --target main --config Debug
cmake --build E:\demo\lua-dap-debugger\build\msvc --target luadap --config Debug
```

默认端口 `8172`，可用环境变量 `LUADAP_HOST` / `LUADAP_PORT` 覆盖。

### 2. 先启动宿主

```powershell
.\bin\main.exe
```

控制台应出现类似：

```
[lua-dap] listening on 127.0.0.1:8172, waiting for VS Code debugServer...
```

`start(..., true)` 内部泵事件，直到 DAP 握手完成（`configurationDone`），**然后**才执行 `script/sample/main.lua`。

### 3. VS Code 附加

用 VS Code 打开本仓库，按 **F5**，选择 **Lua DAP Attach (debugServer)**。

配置在 `.vscode/launch.json`：

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "Lua DAP Attach (debugServer)",
      "type": "node",
      "request": "attach",
      "debugServer": 8172
    }
  ]
}
```

`type: "node"` 只是为了复用 VS Code 内置 DAP 客户端；没有 Node 调试器逻辑。握手成功后 sample 开始执行。

### 4. 断点与变量

在 `script/sample/main.lua` 的 `local sum = add(x, y)` 一行打断点，查看 Variables：

- locals：`player` / `x` / `y` / `sum`
- 展开 `player` 可见 `name`、`stats`；再展开 `stats` 可见 `hp`、`mp`

Continue / Step Over / Step Into / Step Out 可用。停止调试后宿主应继续跑完或正常退出，不应卡死。

---

## 自动化冒烟（可选）

```powershell
cd E:\demo\lua-dap-debugger
python script/test/test_asyncsocket_smoke.py
python script/test/test_dap_luadap_handshake.py
python script/test/test_dap_luadap_nowait.py
python script/test/test_dap_handshake.py
python script/test/test_dap_breakpoint.py
python script/test/test_dap_step.py
python script/test/test_dap_disconnect.py
python script/test/test_dap_partial_frame.py
python script/test/test_dap_condition.py
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
- `vscode-extension/` 为历史目录，V1 非必需
- `luadap` 不导出 `shutdown`；进程退出由宿主负责
