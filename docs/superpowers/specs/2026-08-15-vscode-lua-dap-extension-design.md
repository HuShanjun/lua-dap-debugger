# VS Code 通用 lua-dap 扩展设计

**日期：** 2026-08-15  
**状态：** 已实现  
**范围：** 将现有 C `luadap` DAP 后端封装为可安装的 VS Code 调试扩展，支持 **Launch 单文件**（经 `lua-runner`）与 **Attach 到已 listen 的端口**。

**前置：**
- `luadap` 已提供完整 DAP TCP 服务（断点 / 步进 / 变量 / evaluate）
- 当前 VS Code 用法为 `type: node` + `debugServer`（非正式扩展）
- 仓库已有骨架 `vscode-extension/package.json`，无可用 Adapter 实现

## 目标

1. 贡献正式调试类型 `lua-dap`，可在任意工作区使用。  
2. **Launch：** 扩展拉起 **`lua-runner <lua文件> [args...]`**；runner 内已链接 Lua + `luadap`。  
3. **Attach：** 连接到已调用 `dap.start` 的进程端口（宿主 / 游戏）。  
4. 二进制：扩展内置 Windows x64 `lua-runner`；设置 `runnerPath` 可覆盖。  
5. 替换仓库内 `type: node` + `debugServer` 黑科技为 `type: lua-dap`。

## 已锁定决策

| 决策 | 选择 |
|------|------|
| Attach 含义 | 连已 listen 的 DAP TCP 端口（非 PID 注入） |
| Launch 方式 | **仅** spawn `lua-runner`（CLI = 待调试 `.lua` + 可选脚本参数） |
| 不用 | 外挂 `luaexe` + `require("luadap")` 的 Launch 路径 |
| 二进制分发 | 优先扩展内置 `lua-runner`；设置 `luadap.runnerPath` 可覆盖 |
| 扩展架构 | 薄扩展 + `DebugAdapterServer`；不在 Node 重写 DAP |
| 首版平台 | Windows x64 内置 runner；其它平台靠自编 `runnerPath` |
| Marketplace | 非本设计必交付；先本地 F5 / `vsce package` |

## 范围

**内：**
- 新建可执行目标 **`lua-runner`**（C/C++）：链接 `liblua` + `luadap`（静态或按仓库惯例）
- 重做 `vscode-extension/`（TypeScript）：`extension.ts`、`launch.ts`、`package.json`
- 扩展内置 / 拷贝：`bin/win32-x64/lua-runner.exe`
- 设置：`luadap.runnerPath`、`luadap.defaultPort`
- 更新 `.vscode/launch.json`、`README.md`

**外：**
- Launch 时使用系统 `lua` + `luadap.dll`  
- PID / 任意进程注入  
- macOS / Linux 预编译进扩展包  
- 修改 `luadap` DAP 协议语义  
- 完整 Marketplace CI 发布流水线  

## 架构

```
┌──────────────────┐     DAP/TCP      ┌─────────────────────────┐
│ VS Code          │ ◄──────────────► │ luadap inside           │
│ type: lua-dap    │  DebugAdapter-   │ lua-runner  OR host     │
│ DescriptorFactory│  Server(port)    │ already listening       │
└────────┬─────────┘                  └─────────────────────────┘
         │ Launch only: spawn
         ▼
┌──────────────────────────────┐
│ lua-runner program.lua ...   │
│ (liblua + luadap linked)     │
│ dap.start(host,port,true)    │
│ run file + pump dap.update   │
└──────────────────────────────┘
```

扩展 **不** 实现 Content-Length DAP 协议；只负责：
1. Attach：返回 `new vscode.DebugAdapterServer(port, host)`  
2. Launch：spawn `lua-runner` → 等端口可连 → 返回 `DebugAdapterServer`；会话结束杀子进程  

## Attach

**配置属性：**
| 字段 | 默认 | 说明 |
|------|------|------|
| `host` | `127.0.0.1` | DAP 地址 |
| `port` | 设置 `luadap.defaultPort` 或 `8172` | DAP 端口 |

假定对端已嵌入 / `require("luadap")` 并 `dap.start` + 主循环 `dap.update()`（与现宿主一致）。Attach **不** 依赖 `lua-runner`。

## lua-runner（原生）

**CLI：**
```text
lua-runner [--host HOST] [--port PORT] [--] <program.lua> [script_args...]
```
- 无 `--port` 时：读环境变量 `LUADAP_PORT`，再否则默认 `8172`（Launch 时由扩展传入空闲端口）。  
- 无 `--host` 时：`LUADAP_HOST` 或 `127.0.0.1`。

**行为：**
1. 创建 `lua_State`，打开标准库。  
2. 将 `luadap` 以链接方式注册（`luaopen_luadap` / 等价），**不**依赖旁路 `luadap.dll` + `require`。  
3. `dap.start(host, port, true)`（阻塞到 VS Code `configurationDone`）。  
4. 设置 `arg`（`arg[0]=program`，其后为 script_args），`dofile` / `luaL_dofile` 运行目标文件。  
5. 若存在全局函数 `update`，则循环 `update(n)` + `dap.update()`；否则在脚本返回后继续泵 `dap.update` 直至 disconnect / 出错退出。  
6. 标准输出可打印一行就绪标记（如 `LISTEN_DONE`）供扩展可选探测；扩展仍以 TCP 可连为主。

**构建：** CMake 目标 `lua-runner`，输出到 `bin/`；链接与仓库默认 Lua（5.4）及 `luadap` / `asyncsocket_static` 一致。可与 `main` 并存；`main` 仍作示例宿主，不替代 runner。

## Launch（扩展）

**配置属性：**
| 字段 | 说明 |
|------|------|
| `program` | 要调试的 Lua 文件（支持 `${file}`） |
| `args` | 传给脚本的参数（不是传给 runner 的其它开关） |
| `cwd` | 工作目录 |
| `host` / `port` | 可选；缺省 localhost + 自动选空闲端口 |
| `runnerPath` | 可选；覆盖设置与内置 runner |

**Runner 解析顺序：** launch `runnerPath` → 设置 `luadap.runnerPath` → 扩展 `bin/win32-x64/lua-runner.exe`。

**子进程：**
1. 解析空闲端口（若未指定 `port`）。  
2. Spawn：`runner --host <h> --port <p> -- <program> [args...]`（或等价 argv）。  
3. 轮询 TCP 直至可连（超时失败并杀进程），返回 `DebugAdapterServer`。  
4. 调试会话结束时终止 runner 进程。

**不再提供：** `luaexe`、`luadapPath`、bootstrap `require("luadap")` 的 Launch 路径。

## 扩展包结构

```
vscode-extension/
  package.json
  tsconfig.json
  src/extension.ts
  src/launch.ts
  bin/win32-x64/lua-runner.exe   # 构建拷贝；可 gitignore
  out/                           # tsc
```

构建步骤：编译 `lua-runner` → 复制到扩展 bin → `npm run compile`。可选后续 `vsce package`。

## 设置

| 键 | 含义 |
|----|------|
| `luadap.runnerPath` | 覆盖内置 `lua-runner` 可执行文件路径 |
| `luadap.defaultPort` | Attach 默认端口（8172） |

## 仓库集成

- `.vscode/launch.json`：`Lua DAP: Launch current file` 与 `Lua DAP: Attach`（`type: lua-dap`）  
- 废弃说明：旧 `type: node` + `debugServer` 不再推荐  
- `README.md`：安装扩展、`lua-runner` 说明、Launch / Attach 步骤  

## 测试与验收

1. 命令行：`lua-runner script/test/...lua` 能 listen 并用现有 Python DAP 客户端握手 / 断点。  
2. Extension Development Host：Launch `${file}` 断点命中。  
3. Attach：先启动 `main` 或其它已嵌入 `luadap` 的进程，再 Attach 成功。  
4. Watch / Hover / REPL 在两种模式下仍可用。  
5. 现有 Python DAP 回归不因本改动失败（可增 runner 驱动用例）。  

## 非目标与风险

| 项 | 说明 |
|----|------|
| 系统 Lua ABI | Launch 不再依赖用户 `lua.exe`；runner 自带匹配的 liblua |
| 无 `update` 的脚本 | `dofile` 返回后仍泵 DAP，直到 disconnect |
| 端口占用 | Launch 优先自动选空闲端口 |
| 多平台 | 非 Windows 须自编并配置 `runnerPath` |
| Attach 宿主 | 仍自行嵌入 `luadap`（DLL 或静态），与 runner 无关 |

## 实现顺序（摘要）

1. CMake 目标 `lua-runner` + CLI + 泵循环；命令行 / Python 冒烟  
2. TypeScript 扩展骨架 + Attach `DebugAdapterServer`  
3. Launch spawn runner + 端口等待 + 设置 / 拷贝脚本  
4. `launch.json` / README；手工验收；标记设计已实现  

## 验收

- 存在可运行的 `lua-runner <file>`，内链 `luadap`。  
- 扩展激活后可选 `lua-dap` Launch / Attach。  
- Launch `${file}` 可断点调试（经 runner）。  
- Attach `127.0.0.1:8172` 可连接已启动的 `luadap` 宿主。  
- 不再依赖 `type: node` + `debugServer` 或 Launch 时的系统 `lua` + `luadap.dll`。
