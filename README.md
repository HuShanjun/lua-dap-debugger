# Lua DAP Debugger

C++ 宿主通过 **luasocket** 提供标准 [DAP](https://microsoft.github.io/debug-adapter-protocol/) TCP 服务；VS Code 用内置 `debugServer` 直连，即可断点、步进、查看 locals / table 成员。

V1 **不需要** `vscode-extension/`（可保留但不参与调试流程）。

---

## 项目结构

```
lua-dap-debugger/
├── main/main.cpp                 # 宿主：listen 后再跑 sample
├── script/lua-runtime/
│   ├── debugger.lua              # DAP server + hook + 断点/步进/变量
│   └── dkjson.lua
├── script/socket.lua             # luasocket Lua 层
├── script/sample/main.lua        # 演示 locals + nested table
├── script/test/                  # Python DAP 冒烟测试
├── bin/                          # 编译产物：main.exe、lua.exe、socket.dll
│   └── socket/core.dll           # require("socket.core")
└── .vscode/launch.json           # type: node + debugServer: 8172
```

---

## 快速开始（V1 真实流程）

### 1. 编译宿主

用仓库已有的 CMake / MSVC 流程编译，确保以下文件可用：

- `bin/main.exe`
- `bin/socket.dll` 以及 `bin/socket/core.dll`（`require("socket.core")`）
- `script/socket.lua`（`require("socket")`）

示例（本机已配置过 `build/msvc` 时）：

```powershell
cmake --build E:\demo\lua-dap-debugger\build\msvc --target main --config Debug
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

此时进程阻塞等待调试器连接，**还不会**执行 `script/sample/main.lua`。

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

## 宿主接入

业务脚本之前调用：

```lua
local host = os.getenv("LUADAP_HOST") or "127.0.0.1"
local port = tonumber(os.getenv("LUADAP_PORT") or "8172")
local dbg = require("lua-runtime.debugger")
dbg.listen(host, port)  -- 阻塞到 configurationDone
```

`package.path` 需能 `require("lua-runtime.debugger")` 与 `require("socket")`（`script/?.lua`）；`package.cpath` 需能 `require("socket.core")`（`bin/?.dll` → `bin/socket/core.dll`）。

---

## 自动化冒烟（可选）

```powershell
cd E:\demo\lua-dap-debugger\script\test
python test_dap_handshake.py
python test_dap_breakpoint.py
python test_dap_step.py
```

这些测试走 `lua.exe` + 独立 debugee，不启动 `main.exe`。

---

## 已知限制

- 已 strip debug info 的字节码看不到变量；LuaJIT `-O2` 下部分 local 可能被优化
- V1 不做 pathMappings（同机路径规范化即可）
- 协程调试未包含
- `vscode-extension/` 为历史目录，V1 非必需
