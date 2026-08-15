# luadap.dll 封装设计（方案 A）

**日期：** 2026-08-15  
**状态：** 待确认  
**范围：** 将现有 Lua DAP 调试器（`debugger.lua` + `dkjson` + `asyncsocket`）封装为**单一 DLL**，对外只暴露 `start` / `update`

**前置：**
- `docs/superpowers/specs/2026-08-15-lua-dap-debugger-design.md`
- `docs/superpowers/specs/2026-08-15-asyncsocket-dap-design.md`

## 目标

1. 最终交付 **`bin/luadap.dll`**（`require("luadap")`）。  
2. 对外仅两个接口：
   - `start(host, port, is_wait_connect)`
   - `update()`
3. **不**把 DAP/hook 用纯 C 重写；逻辑仍在嵌入的 `debugger.lua`，由 C 负责加载与门面。  
4. `is_wait_connect == true` 时，阻塞到 DAP **`configurationDone`** 再返回（与现 `listen` 一致）。  
5. 部署时不需要旁路 `debugger.lua` / `dkjson.lua` / 独立 `asyncsocket.dll`。

## 已锁定决策

| 决策 | 选择 |
|------|------|
| 实现形态 | 方案 1：单 DLL，静态链 asyncsocket + 嵌入 Lua 脚本 |
| `is_wait_connect=true` | 等到 `configurationDone` |
| `is_wait_connect=false` | 启动后立即返回，握手靠后续 `update` |
| 磁盘上的 `script/lua-runtime/*.lua` | 保留为可编辑源；发布物以嵌入为准 |

## 架构

```
宿主 Lua / C++(sol2)
    require("luadap")
         │
         ▼
┌─────────────────── bin/luadap.dll ───────────────────┐
│  luaopen_luadap                                      │
│    ├─ package.preload["asyncsocket"] = 静态链入模块   │
│    ├─ load 嵌入 dkjson.lua / debugger.lua            │
│    └─ 导出 start / update                            │
│  内部: asyncsocket poll 线程 + DAP 协程 + debug hook   │
└──────────────────────────────────────────────────────┘
```

## 对外 API

```lua
local dap = require("luadap")

-- is_wait_connect: true → 阻塞到 configurationDone；false → 立即返回
dap.start(host, port, is_wait_connect)

-- 每帧驱动：pump 网络事件 + resume DAP 读协程
dap.update()
```

可选：模块表带 `_VERSION`；**不**要求导出 `shutdown`（进程退出由宿主负责；后续可加 `stop`）。

### `start` 与现有 `listen` 的关系

在 `debugger.lua` 增加可选等待控制（建议第三参或 options）：

- `listen(host, port)` 或 `listen(host, port, true)`：现行为（泵到 `configurationDone`）
- `listen(host, port, false)`：创建 sock/协程/回调后立即返回，不进入 wait 循环

`luadap.start(host, port, wait)` 直接转发到上述 API。

### `update`

等价于现有 `debugger.update()`。

## 嵌入与加载

1. CMake 将下列文件生成 C 字符串资源：
   - `script/lua-runtime/debugger.lua`
   - `script/lua-runtime/dkjson.lua`
2. `luaopen_luadap`：
   - 注册 `package.preload["asyncsocket"]` → 链入的 `luaopen_asyncsocket`
   - 加载 dkjson（`package.preload["lua-runtime.dkjson"]` 或内部等价名，与 debugger 的 `require` 一致）
   - `load` debugger，保存模块表引用（registry）
   - 构造导出表 `{ start = ..., update = ..., _VERSION = "..." }`
3. 改 Lua 源后必须**重编** `luadap.dll`（CMake 依赖源文件）。

## CMake / 产物

| 目标 | 类型 | 说明 |
|------|------|------|
| `asyncsocket` | STATIC 或 OBJECT | 供 `luadap` 链接；开发期可另保留 SHARED 可选 |
| `luadap` | SHARED | 输出 `bin/luadap.dll`，导出 `luaopen_luadap`，链 `liblua` + asyncsocket + `ws2_32` |

目录建议：

```
native/luadap/
  luadap.c
  embed_gen.cmake（或脚本）
  （生成）embedded_debugger.c / embedded_dkjson.c
native/asyncsocket/   # 现有，改为可被静态链接
```

## 宿主改动

`main.cpp` 简化为：

```cpp
require("luadap")
start(host, port, true)
RunFile(sample)
loop: update()  // + 可选业务 update
```

不必再 `require("lua-runtime.debugger")` 或设置 `script/lua-runtime` 到 path（仅跑嵌入 DLL 时）。

## 测试

- 新增/改造 debugee：只 `require("luadap")` + `start` + 业务代码；`cpath` 含 `bin/?.dll`。
- 回归：handshake / breakpoint / step / disconnect / partial_frame（经 luadap）。
- 验收「无旁路 lua 文件」：临时从 path 去掉 `script/?.lua` 中的 runtime，仍能 `require("luadap")` 完成握手。

## 非目标

- 纯 C 重写 DAP/hook  
- 删除仓库内 `script/lua-runtime` 源  
- 新的 VS Code 扩展  
- 多实例 `luadap` 同时 listen（仍 V1 单实例）

## 风险与缓解

| 风险 | 缓解 |
|------|------|
| 嵌入与磁盘脚本漂移 | CMake 依赖 `.lua`；文档写明需重编 |
| asyncsocket 命名冲突 | 仅 `package.preload`，不依赖磁盘 `asyncsocket.dll` |
| 双份 liblua / 缺导出 | 对齐现有 MSVC `dllexport` + 同链 `liblua` |
| `wait=false` 路径未测全 | 至少一条冒烟：start(false) + update 完成握手 |

## 验收清单

1. `bin/luadap.dll` + `require("luadap")`  
2. `start(..., true)` 等到 `configurationDone`  
3. `update()` 驱动下断点/步进/变量可用  
4. 无旁路 debugger/dkjson/asyncsocket 文件可运行  
5. 核心 Python DAP 冒烟通过  
