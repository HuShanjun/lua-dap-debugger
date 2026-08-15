# VS Code 通用 lua-dap 扩展设计

**日期：** 2026-08-15  
**状态：** 已批准待实现  
**范围：** 将现有 C `luadap` DAP 后端封装为可安装的 VS Code 调试扩展，支持 **Launch 单文件** 与 **Attach 到已 listen 的端口**。

**前置：**
- `luadap.dll` 已提供完整 DAP TCP 服务（断点 / 步进 / 变量 / evaluate）
- 当前 VS Code 用法为 `type: node` + `debugServer`（非正式扩展）
- 仓库已有骨架 `vscode-extension/package.json`，无可用 Adapter 实现

## 目标

1. 贡献正式调试类型 `lua-dap`，可在任意工作区使用。  
2. **Launch：** 扩展拉起配置的 `luaexe`，加载 `luadap`，调试单个 `.lua` 文件。  
3. **Attach：** 连接到已调用 `dap.start` 的进程端口（宿主 / 游戏）。  
4. 二进制：扩展内置 Windows x64 `luadap.dll`，可用设置 / 配置覆盖。  
5. 替换仓库内 `type: node` + `debugServer` 黑科技为 `type: lua-dap`。

## 已锁定决策

| 决策 | 选择 |
|------|------|
| Attach 含义 | 连已 listen 的 DAP TCP 端口（非 PID 注入） |
| Launch 方式 | 扩展 spawn `luaexe` + 捆绑/覆盖的 `luadap` + `dofile(program)` |
| 二进制分发 | 优先扩展内置；设置 `luadapPath` 可覆盖（方案 C） |
| 扩展架构 | 薄扩展 + `DebugAdapterServer`（方案 1）；不在 Node 重写 DAP |
| 首版平台 | Windows x64 内置 DLL；其它平台靠 `luadapPath` |
| Marketplace | 非本设计必交付；先本地 F5 / `vsce package` |

## 范围

**内：**
- 重做 `vscode-extension/`（TypeScript）：`extension.ts`、`launch.ts`、`package.json`
- `runtime/launch_bootstrap.lua`
- 拷贝 / 文档约定：`bin/win32-x64/luadap.dll`
- 设置：`luadap.luaexe`、`luadap.luadapPath`、`luadap.defaultPort`
- 更新 `.vscode/launch.json`、`README.md`

**外：**
- PID / 任意进程注入  
- macOS / Linux 预编译进扩展包  
- 修改 `luadap` DAP 协议语义  
- 完整 Marketplace CI 发布流水线  

## 架构

```
┌──────────────────┐     DAP/TCP      ┌─────────────────────────┐
│ VS Code          │ ◄──────────────► │ luadap (in lua process  │
│ type: lua-dap    │  DebugAdapter-   │  or host already        │
│ DescriptorFactory│  Server(port)    │  listening)             │
└────────┬─────────┘                  └─────────────────────────┘
         │ Launch only: spawn
         ▼
┌──────────────────┐
│ luaexe + bootstrap│
│ require luadap    │
│ start(wait=true)  │
│ dofile(program)   │
└──────────────────┘
```

扩展 **不** 实现 Content-Length DAP 协议；只负责：
1. Attach：返回 `new vscode.DebugAdapterServer(port, host)`  
2. Launch：spawn 子进程 → 等端口可连 → 返回 `DebugAdapterServer`；会话结束杀子进程  

## Attach

**配置属性：**
| 字段 | 默认 | 说明 |
|------|------|------|
| `host` | `127.0.0.1` | DAP 地址 |
| `port` | 设置 `luadap.defaultPort` 或 `8172` | DAP 端口 |

假定对端已 `require("luadap"); dap.start(...);` 并在主循环调用 `dap.update()`（与现宿主一致）。

## Launch

**配置属性：**
| 字段 | 说明 |
|------|------|
| `program` | 要调试的 Lua 文件（支持 `${file}`） |
| `args` | 传给脚本的参数 |
| `cwd` | 工作目录 |
| `luaexe` | 解释器路径；缺省用设置 `luadap.luaexe` 或 `"lua"` |
| `host` / `port` | 可选；缺省 localhost + 自动选空闲端口 |
| `luadapPath` | 可选；覆盖设置与内置 DLL |

**DLL 解析顺序：** launch `luadapPath` → 设置 `luadap.luadapPath` → 扩展 `bin/win32-x64/luadap.dll`。

**子进程：**
1. 设置环境：`LUADAP_HOST`、`LUADAP_PORT`、`LUADAP_CPATH`（DLL 目录）  
2. 运行扩展自带 `runtime/launch_bootstrap.lua`，参数包含 `program` 与 `args`  
3. Bootstrap：追加 `package.cpath` → `require("luadap")` → `dap.start(host, port, true)` → `dofile(program)`  
4. 若脚本定义全局 `update`，则循环 `update(n)` + `dap.update()`；否则在脚本返回后继续泵 `dap.update` 直至 disconnect / 进程退出  
5. 扩展轮询 TCP 直至可连（超时失败并杀进程），再返回 `DebugAdapterServer`  

**ABI 注意：** 内置 DLL 按仓库默认 Lua 5.4 构建；用户使用 5.1–5.3 `luaexe` 时须自备匹配的 `luadapPath`（文档说明）。

## 扩展包结构

```
vscode-extension/
  package.json
  tsconfig.json
  src/extension.ts
  src/launch.ts
  runtime/launch_bootstrap.lua
  bin/win32-x64/luadap.dll    # 构建拷贝；可 gitignore
  out/                        # tsc
```

构建步骤：编译 `luadap` → 复制 DLL 到扩展 bin → `npm run compile`。可选后续 `vsce package`。

## 设置

| 键 | 含义 |
|----|------|
| `luadap.luaexe` | 默认 Lua 解释器 |
| `luadap.luadapPath` | 覆盖内置模块路径 |
| `luadap.defaultPort` | Attach 默认端口（8172） |

## 仓库集成

- `.vscode/launch.json`：提供 `Lua DAP: Launch current file` 与 `Lua DAP: Attach`（`type: lua-dap`）  
- 废弃说明：旧 `type: node` + `debugServer` 不再推荐  
- `README.md`：安装扩展（F5 扩展开发宿主或旁加载 VSIX）、Launch、Attach 步骤  

## 测试与验收

1. Extension Development Host：Launch 简单脚本，断点命中。  
2. Attach：先启动 `main` 或 `run_debugee_luadap.lua`，再 Attach 成功。  
3. Watch / Hover / REPL 在两种模式下仍可用（后端不变）。  
4. 现有 Python DAP 回归不因本改动失败。  

## 非目标与风险

| 项 | 说明 |
|----|------|
| Lua ABI 不匹配 | 用户 `luaexe` 与 DLL 主版本不一致会 `require` 失败；靠文档 + `luadapPath` |
| 无 `update` 的脚本 | Launch 在 `dofile` 返回后仍泵 DAP，直到 disconnect |
| 端口占用 | Launch 优先自动选空闲端口 |
| 多平台 | 非 Windows 必须配置 `luadapPath` |

## 实现顺序（摘要）

1. TypeScript 扩展骨架 + Attach `DebugAdapterServer`  
2. Launch spawn + bootstrap + 端口等待  
3. 设置、内置 DLL 拷贝脚本、`launch.json` / README  
4. 手工验收 Launch / Attach；标记设计已实现  

## 验收

- 扩展激活后可选 `lua-dap` Launch / Attach。  
- Launch `${file}` 可断点调试。  
- Attach `127.0.0.1:8172` 可连接已启动的 `luadap` 宿主。  
- 不再依赖 `type: node` + `debugServer` 作为推荐路径。
