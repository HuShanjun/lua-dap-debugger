# Lua DAP Debugger — 通用 Lua 调试器（VS Code + DAP）

一个基于 **Debug Adapter Protocol (DAP)** 的通用 Lua 调试器：
- **前端**：VS Code 扩展（TypeScript），实现 DAP 协议，复用 VS Code 原生调试 UI
- **后端**：纯 Lua 调试运行时，基于 `debug` 库 + `debug.sethook` 实现断点/步进/变量

支持 Lua 5.1–5.4 + LuaJIT，本地 launch 和远程 attach 两种模式。

---

## 项目结构

```
lua-dap-debugger/
├── vscode-extension/          # VS Code 扩展（前端，TypeScript）
│   ├── package.json
│   ├── tsconfig.json
│   ├── src/
│   │   ├── extension.ts       # 扩展入口，注册 DAP adapter
│   │   ├── debugger.ts        # LuaDebugSession：实现 DAP 协议
│   │   └── adapter.ts         # LuaDebugAdapter：启动/连接后端进程
├── lua-runtime/               # Lua 调试后端
│   └── debugger.lua           # 调试运行时（断点/钩子/变量/步进）
├── sample/                    # 使用示例
│   └── main.lua
└── .vscode/launch.json        # 开箱即用的调试配置
```

---

## 快速开始

### 1. 安装 Lua
确保系统 PATH 中有 `lua`（5.4 最佳，5.1/5.3 也可）。

### 2. 安装 VS Code 扩展
本仓库就是 VS Code 扩展，可直接在 `vscode-extension/` 目录按 F5 调试扩展本身；
正式使用时，把 `vscode-extension` 作为扩展打包，或临时用 `launch.json` 指向它。

### 3. 配置调试
在项目根目录创建 `.vscode/launch.json`：

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "lua-dap",
      "request": "launch",
      "name": "Lua DAP Launch",
      "program": "${workspaceFolder}/sample/main.lua",
      "cwd": "${workspaceFolder}",
      "luaexe": "lua",
      "args": [],
      "stopOnEntry": false
    },
    {
      "type": "lua-dap",
      "request": "attach",
      "name": "Lua DAP Attach",
      "host": "127.0.0.1",
      "port": 8172
    }
  ]
}
```

### 4. 运行
按 **F5** → 断点命中 → 左侧变量树/调用栈/Watch 正常工作。

---

## DAP 协议实现说明

调试器核心是一个 **请求-响应 + 事件** 的状态机：

```
VS Code UI                          Lua 调试后端
─────────────                      ─────────────────
DAP Request  ────────────────▶    命令分发器
   setBreakpoints                  │ 写入断点表
DAP Request  ────────────────▶    continue / stepIn / stepOver
   (继续/步进)                    │ 恢复执行，等下一次 hook
                                  ▲ 断点命中 → 发 stopped 事件
                                  └────────────  stopped event
DAP Request  ────────────────▶    stackTrace / variables / evaluate
   stackTrace/variables/eval      │ 通过 debug 库采集信息
```

### 后端关键能力（`debugger.lua`）
| 能力 | 实现方式 |
|------|----------|
| 行断点 | `debug.sethook(hook, "lcr")`，hook 内查断点表 |
| 条件断点 | 断点表存 condition，hook 内求值 |
| Step Into / Over / Out | hook 内维护 step 标志 + 调用栈深度计数 |
| 调用栈 | `debug.getinfo(level, "Snl")` 逐级采集 |
| 局部变量 | `debug.getlocal` + `debug.getupvalue`（upvalue 作用域） |
| 表达式求值 | 在暂停的闭包上下文里 `load+run` 表达式 |

### 前端关键能力（`debugger.ts`）
实现 DAP 规范要求的请求处理器：`initialize / launch / attach / setBreakpoints /
stackTrace / variables / evaluate / continue / next / stepIn / stepOut / disconnect`，
并发送 `initialized / stopped / terminated / output` 事件。

---

## 调试后端注入方式

### 本地 launch
扩展启动 `lua debugger.lua -- <program> [args]`，后端自动 `dofile(program)`。

### 远程 attach（游戏/服务端/嵌入式）
在宿主入口加一行：
```lua
local dbg = require("debugger")  -- 把 debugger.lua 放到 package.path 能找到的位置
dbg.start("127.0.0.1", 8172)    -- 连上 VS Code（默认端口 8172，可用 env MOBDEBUG_PORT 改）
```
然后 F5 选 attach 配置，触发宿主逻辑即可断下。

---

## 已知限制
- 受 Lua 限制：已 strip debug info 的字节码看不到变量值；LuaJIT `-O2` 下部分 local 被优化
- 表达式求值在全局环境进行（能访问 _G 下的变量，闭包内局部变量需通过 _G 暴露）
- 协程调试需额外 patch（`debugger.coro()` 思路），本期未包含

这些都是 Lua 语言级限制，不是调试器本身的问题。
