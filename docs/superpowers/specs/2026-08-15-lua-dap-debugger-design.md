# Lua DAP 调试器 V1 设计

**日期：** 2026-08-15  
**状态：** 已批准并完成 V1 实现  
**范围：** 第一版可用 — 宿主作为 DAP TCP 服务端，VS Code 通过 `debugServer` 直连

## 目标

实现一个 Lua 调试器，满足：

1. C++ 宿主程序作为 **TCP Server**。
2. VS Code 作为客户端，用 `debugServer` **直连**宿主端口。
3. Lua 运行时通过 **luasocket** 直接讲 **标准 DAP**。
4. 支持 **行断点**，以及查看 **局部变量 / table 成员**。

## 已锁定决策

| 决策 | 选择 |
|------|------|
| DAP 落点 | Lua 后端直接说标准 DAP（`Content-Length` 帧 + JSON） |
| VS Code 接入 | 使用内置 `debugServer` 直连；**V1 不写自定义扩展桥** |
| 启动时序 | 宿主在 `listen` 中阻塞，直到 `configurationDone`，再跑业务脚本 |
| 实现形态 | 单文件 `debugger.lua` + 内置 `dkjson.lua` |
| launch 模式 | V1 不做（仅 attach / debugServer） |
| 路径映射 | 仅同机路径规范化；不做远程 `pathMappings` / `sourceMapPathOverrides` |
| JSON | 内置纯 Lua `dkjson`（MIT） |

## 架构

```
┌─────────────┐   DAP over TCP（debugServer）   ┌────────────────────────────┐
│  VS Code UI │ ◄─────────────────────────────► │ C++ 宿主 + debugger.lua    │
│ type=node   │   Content-Length + JSON         │ listen → accept → DAP 状态机 │
│ debugServer │                                 │ debug.sethook 断点/步进/变量 │
└─────────────┘                                 └────────────────────────────┘
```

### 组件职责

| 组件 | 职责 |
|------|------|
| C++ `main` | 初始化 Lua（sol2），设置 `package.path` / `package.cpath`，调用 `debugger.listen`，再执行 sample |
| `script/lua-runtime/debugger.lua` | TCP server、DAP 帧解析与分发、hook、断点、步进、调用栈、变量 |
| `script/lua-runtime/dkjson.lua` | 纯 Lua JSON 编解码 |
| `.vscode/launch.json` | 使用 `type: "node"` + `debugServer: <端口>` 直连 |
| `script/sample/main.lua` | 演示脚本（含 locals 与嵌套 table，便于手工验收） |

> 说明：`type: "node"` 仅借用已安装的 Node 调试器贡献点，让 VS Code 允许该配置；真正处理 DAP 的是 Lua 侧服务。Node 专有选项（如 `sourceMapPathOverrides`）**不会**自动生效。

## 连接时序（V1）

1. 宿主启动 → `require("lua-runtime.debugger").listen("127.0.0.1", 8172)`。
2. Server `bind` 后 **阻塞在 `accept`**。
3. 用户在 VS Code 按 F5（`debugServer` 配置）→ 直连该端口。
4. DAP 握手：
   - `initialize` → 回 capability → 发 `initialized` 事件
   - `attach` → 回响应
   - `setBreakpoints`（0 次或多次）→ 回响应
   - `setExceptionBreakpoints`（若发来）→ 空成功响应
   - `configurationDone` → 回响应
5. `listen` 返回 C++ 宿主。
6. 宿主执行业务脚本；此时行 hook 已安装。
7. 命中断点/步进 → 发 `stopped` → 在 hook 内同步读 DAP，直到 continue/step。
8. 脚本结束或 `disconnect` → 视情况发 `terminated`，卸 hook，关 socket。

## DAP 传输

标准 Debug Adapter Protocol 帧格式：

```
Content-Length: <n>\r\n
\r\n
<长度为 n 的 JSON body>
```

消息为 DAP JSON 对象（`type`：`request` | `response` | `event`）。

## DAP 表面（V1）

### 处理的请求

| 请求 | 行为 |
|------|------|
| `initialize` | 返回 capabilities：`supportsConfigurationDoneRequest=true`；其它高级能力不声明或为 false |
| `attach` | 标记会话已连接；**不**启动业务脚本（由宿主在 `listen` 返回后启动） |
| `setBreakpoints` | 按 source path **全量替换**该文件断点；路径规范化后入库；返回 verified 断点 |
| `setExceptionBreakpoints` | 空成功响应（VS Code 常发；V1 不因异常停） |
| `configurationDone` | 结束握手，解除 `listen` 阻塞 |
| `threads` | 返回单一合成线程 `{ id = 1, name = "main" }` |
| `continue` | 清除步进状态并恢复；`allThreadsContinued = true` |
| `next` | 步过（按栈深度） |
| `stepIn` | 步入（下一行事件停下） |
| `stepOut` | 步出（栈深度变浅时停下） |
| `stackTrace` | 用 `debug.getinfo` 构建帧（threadId=1） |
| `scopes` | 提供 `Locals` 与 `Upvalues` 两个 scope |
| `variables` | 按 reference 解析 locals / table 成员 / upvalues |
| `disconnect` / `terminate` | 卸 hook、关连接，宿主继续跑完或干净退出 |

所有 response/event 使用单调递增的 `seq`。未知请求返回 `success=false`，避免客户端挂死。

### 发出的事件

| 事件 | 时机 |
|------|------|
| `initialized` | `initialize` 响应成功之后 |
| `stopped` | 断点或步进命中（`reason`: `breakpoint` \| `step`，`threadId`: 1） |
| `terminated` | 调试会话结束 / 调试下脚本结束 |

### V1 明确不做

- 扩展侧 `launch`（由扩展拉起 Lua 进程）
- 条件断点、日志点、命中次数
- `evaluate` / Watch 表达式
- 协程调试
- 多客户端、断线重连策略
- 远程路径映射（含 Node 的 `sourceMapPathOverrides`）
- 修改变量 / 设置表达式
- 自定义 VS Code 调试扩展（薄桥也不做）

## 调试运行时设计

### Hook

- 最晚在 `configurationDone` 返回前安装 `debug.sethook(hook, "lcr")`。
- 每个 line 事件：
  1. 规范化当前源路径 + 行号。
  2. 命中断点表 → `pause("breakpoint")`。
  3. 否则若步进条件满足 → `pause("step")`。

### 暂停模型（关键）

暂停时**不要**切到独立调试协程导致 debugee 栈失效。

在 hook 内：

1. 发送 `stopped` 事件。
2. 进入 **同步** socket 读/分发循环。
3. 在现场栈上处理 `stackTrace` / `scopes` / `variables`。
4. 仅在 `continue` / `next` / `stepIn` / `stepOut` / disconnect 时退出循环。

这样 `debug.getlocal` 才能看到正确的局部变量。

### 路径规范化

断点 key 与栈帧路径：

1. 去掉 `debug.getinfo` source 的前导 `@`。
2. `\` 统一为 `/`。
3. Windows 盘符小写。
4. 可选：去掉尾部 `/`。

V1 假定 VS Code 与宿主看到**同一台机器的同一套路径**（无 path mapping 表）。

### 变量

- **Locals：** `debug.getlocal(level, i)`；跳过 `(temporary)` / `(*temporary*)` 等临时名。
- **Upvalues：** 对帧函数 `debug.getupvalue`；单独一个 scope。
- **Table：** 分配递增 `variablesReference`，缓存 table；展开时 `pairs` 遍历；嵌套 table 再分配新 ref。
- **标量：** `value` 字符串化，`variablesReference = 0`。
- 每次新 stop 或离开 pause 时清空 ref 缓存，避免陈旧引用。

### 步进

| 命令 | 规则 |
|------|------|
| `stepIn` | 下一次 line 事件停下 |
| `next`（步过） | 恢复时记录深度；line 且深度 ≤ 记录深度时停下 |
| `stepOut` | 记录深度；深度 < 记录深度时停下 |
| `continue` | 无步进标志；仅断点可停 |

计算深度 / `stackTrace` 时跳过调试器自身内部帧。

## 宿主接入

`main/main.cpp` 在 Lua 初始化后：

```cpp
// package.path 已包含 script/
lua.script(R"(
  local dbg = require("lua-runtime.debugger")
  dbg.listen("127.0.0.1", 8172)
)");
RunFile(lua, "E:/demo/lua-dap-debugger/script/sample/main.lua");
```

说明：

- host/port 优先读环境变量 `LUADAP_HOST` / `LUADAP_PORT`，默认 `127.0.0.1` / `8172`。
- V1 demo 可硬编码 sample 路径；后续再改为 argv。
- `package.cpath` 必须能找到 luasocket（`bin/?.dll`）。

对外 Lua API：

```lua
local dbg = require("lua-runtime.debugger")
dbg.listen(host, port)  -- 阻塞到 configurationDone
```

## VS Code 配置（debugServer）

不写自定义扩展。项目 `.vscode/launch.json` 使用：

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "Lua DAP Attach",
      "type": "node",
      "request": "attach",
      "debugServer": 8172
    }
  ]
}
```

使用方式：

1. 先启动宿主 `main.exe`（监听 8172）。
2. 再在 VS Code 对上述配置按 F5。

可选：若需改端口，宿主与 `debugServer` 保持一致即可。

仓库中已有的 `vscode-extension/` 在 V1 **不再作为必交付物**；可保留但不维护，或后续删除。

## 文件改动

| 路径 | 动作 |
|------|------|
| `script/lua-runtime/debugger.lua` | **重写**为标准 DAP + luasocket server |
| `script/lua-runtime/dkjson.lua` | **新增**内置 dkjson（MIT） |
| `main/main.cpp` | 跑 sample 前调用 `dbg.listen` |
| `.vscode/launch.json` | 改为 `type: node` + `debugServer` |
| `script/sample/main.lua` | 增加 table 字段，便于验成员变量 |
| `README.md` | 按 V1 真实 attach/`debugServer` 流程更新 |
| `vscode-extension/` | V1 不依赖；可不改或标注废弃 |

## 错误处理（V1）

- 端口 bind 失败：打印明确错误并中止启动。
- 暂停期间客户端断开：卸 hook，退出 pause 循环；默认脚本继续跑（无调试器）。
- 非法 DAP 帧 / JSON：日志记录并结束会话，尽量不崩宿主（decode/dispatch 包 `pcall`）。
- 缺少 luasocket / dkjson：`require` 失败并快速暴露。

## 验收标准

- [x] 1. 启动 `main.exe` → 控制台显示监听 `127.0.0.1:8172` 并等待。（Task 6 已验；`test_dap_handshake.py` 间接覆盖 listen/accept）
- [x] 2. VS Code F5（`debugServer: 8172`）→ 握手成功，sample 开始执行。（`test_dap_handshake.py` 覆盖 DAP 握手 + 脚本继续；VS Code F5 需手工）
- [x] 3. 在 `sample/main.lua` 某行打断点 → 命中且行号正确。（`test_dap_breakpoint.py`）
- [x] 4. Variables 能看到 locals；展开 table 能看到字段。（`test_dap_breakpoint.py`：x/y/player + player.name/stats）
- [x] 5. Continue / Step Over / Step Into / Step Out 可用。（`test_dap_step.py`：next / stepIn / stepOut / continue）
- [ ] 6. 停止调试后宿主不卡死（继续跑完或可预期退出）。（未在本环境跑 VS Code Stop；`disconnect` 路径在代码中实现，建议手工确认）

自动化冒烟：`script/test/test_dap_handshake.py`、`test_dap_breakpoint.py`、`test_dap_step.py`（2026-08-15 全部通过）。

## 非目标（再次强调）

V1 故意收窄：仅 `debugServer` 直连、同机路径、断点 + 步进 + locals/table 成员。自定义扩展、launch、远程 path mapping、表达式求值等一律后置。

## 实现备注

| 项 | 实际实现 |
|----|----------|
| 栈帧校准 | 不用固定 `frameId+3` 或 `debug.getinfo(2)`。`walk_user_frames()` 从 level 2 起跳过 `debugger.lua` / `dkjson.lua` 及非 `@` 源；`getlocal` 用 `level - 1`（相对调用方），`frame_id 0` 为离暂停点最近的用户帧。 |
| 变量 reference | locals scope = `100000 + frameId`；upvalues = `200000 + frameId`；table 对象 ref 自 `1000` 递增，每次 stop 重置，与 scope id 不冲突。 |
| luasocket | `require("socket")`；宿主 `package.cpath` 指向 `bin/?.dll`（Windows 上勿用 `path/?.dll` 会变成盘根路径）。 |
| 测试 debugee | 冒烟用 `lua.exe` + `run_debugee*.lua`，非 `main.exe`；行为与 `debugger.listen` 一致。 |
| VS Code | `type: node` + `debugServer` 仅借 Node 调试器贡献点；Node 专有选项不生效。 |
