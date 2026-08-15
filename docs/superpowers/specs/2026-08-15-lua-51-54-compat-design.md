# asyncsocket / luadap Lua 5.1–5.4 兼容设计

**日期：** 2026-08-15  
**状态：** 已批准待验证  
**范围：** 使 `asyncsocket` 与 `luadap` 在 Lua **5.1 / 5.2 / 5.3 / 5.4** 下可编译、可链接，并由 CI 四版本矩阵验证。

**前置：**
- 当前工程仅 vendoring `3rd/lua-5.4.8`，C API 按 5.4 编写
- 既有 DAP / evaluate / asyncsocket 0.3 行为保持不变（仅附着 env / userdata 的实现按版本分支）

## 目标

1. `asyncsocket` + `luadap` 支持 Lua 5.1–5.4。  
2. 自研薄兼容层（仿 luasocket），不引入 sol2/Kepler 整包。  
3. 去掉对 `lua_newuserdatauv` / `lua_getiuservalue` / `lua_setiuservalue` 的硬依赖。  
4. evaluate / 条件断点在 5.1 用 `setfenv`，在 5.2+ 继续 `_ENV` + `setupvalue`。  
5. 默认仍用仓库内 5.4；其它版本经 CMake `LUA_ROOT` 或 FetchContent。  
6. CI 矩阵：5.1 / 5.2 / 5.3 / 5.4 全编全测。

## 已锁定决策

| 决策 | 选择 |
|------|------|
| 版本范围 | Lua 5.1–5.4（不含 LuaJIT） |
| 兼容层 | 自研 `native/common/lua_compat.{h,c}`（方案 B） |
| userdata | 全版本统一「单 uservalue 表」 |
| 求值 env | 5.1=`setfenv`；5.2+=`setupvalue(_ENV)` |
| Lua 源码 | 默认只 vendor 5.4；其它版本 `LUA_ROOT` 或 FetchContent（不进 git） |
| 验证 | CI 四版本矩阵编 + 测 |
| 宿主 | `main`/sol2 仅默认 5.4 构建；非 5.4 CI 测 DLL + 轻量 Lua/Python 驱动 |

## 范围

**内：**
- `native/common/lua_compat.h` / `lua_compat.c`
- `native/asyncsocket` userdata / uservalue 改写
- `native/luadap` chunk env 附着（evaluate + BP condition）
- 根 / 子目录 CMake：`LUA_VERSION`、`LUA_ROOT`、FetchContent
- CI workflow 矩阵与最低测试套件
- README / 本文档中的支持说明

**外：**
- LuaJIT
- 强制 `main` + sol2 链 5.1–5.3
- 修改 `3rd/luasocket` 自带 compat
- 改变 DAP 协议语义或 evaluate 写回规则

## 兼容层（`lua_compat`）

在 `#include <lua.h>` / `<lauxlib.h>` 之后由 `asyncsocket`、`luadap` 包含。

仅 polyfill **实际用到**的符号：

| 符号 | 5.1 | 5.2+ |
|------|-----|------|
| `LUA_OK` | `#define 0` | 原生 |
| `lua_absindex` | 实现 | 原生 |
| `lua_pushglobaltable` | `LUA_GLOBALSINDEX` | 原生 |
| `lua_rawgetp` / `lua_rawsetp` | lightuserdata 键 | 原生 |
| `luaL_setfuncs` | 仿 luasocket | 原生 |
| `luaL_tolstring` | 实现 | 5.2+ 原生 |
| `luaL_checkinteger` | 映射到 `luaL_checkint`（或等价） | 原生 |
| `lua_getuservalue` / `lua_setuservalue` | `getfenv` / `setfenv`（值须为 table） | 原生 |

5.3 / 5.4：头文件对已存在符号不重定义。实现集中在 `lua_compat.c`，由两个 native 目标编译或链入。

**不做：** `lua_arith`、完整 buffer API、Kepler 全量 compat53。

## asyncsocket userdata

当前：`lua_newuserdatauv(..., 1)` + `lua_setiuservalue` / `lua_getiuservalue`（仅 5.4）。

**目标模型（全版本一致）：**
1. `lua_newuserdata(L, sizeof(ud))` 分配 Server/Connection。  
2. 创建回调表，`lua_setuservalue(L, ud_idx)`。  
3. 读回调：`lua_getuservalue` → `getfield`（`on_accept` / `on_open` / …）。

对外 Lua API（`listen` / `connect` / `pump` / 回调名）不变。

## luadap evaluate / 条件断点

现逻辑依赖 chunk 的 `_ENV` upvalue（5.2+）。5.1 无 `_ENV`，`lua_setupvalue(chunk, 1)` 失败则 frame env 不生效。

**Helper：** `ld_set_chunk_env(L, chunk_idx, env_idx)`  
- `LUA_VERSION_NUM == 501`：`lua_setfenv`  
- 否则：`lua_setupvalue(..., 1)`（失败则 pop env，与今行为一致）

供 `eval_breakpoint_condition` 与 `lua_debug_evaluate` 共用。

**不变：**
- Watch / Hover / REPL 语义与写回规则（见 evaluate 设计）
- 只给**临时求值 chunk** 设 env，不修改用户函数 fenv
- REPL `__newindex` 写回 local / upvalue / `_G`

## CMake

| 选项 | 含义 |
|------|------|
| `LUA_VERSION` | `5.1` \| `5.2` \| `5.3` \| `5.4`，默认 `5.4` |
| `LUA_ROOT` | 可选；指向已安装/已解压的 Lua 前缀或源码树 |

解析顺序：
1. 若设 `LUA_ROOT` → 用其 headers + 库（或源码编 `liblua`）。  
2. `LUA_VERSION=5.4` 且 `3rd/lua-5.4.8` 存在 → vendored 树（本地开发）。  
3. 否则 FetchContent 拉取对应官方发布版到 **build 目录**，编 `liblua`，**不提交进仓库**（含 5.4：`/3rd` 被 gitignore，CI 干净检出没有 vendored 5.4.8）。

`asyncsocket` / `luadap`：include 跟随所选 Lua，并编译 `lua_compat.c`。

`main`（sol2）：**仅当 `LUA_VERSION=5.4`** 时加入构建；其它版本 CI 只构建 native DLL + 测试驱动。

## CI 与测试

矩阵 job：`lua: [5.1, 5.2, 5.3, 5.4]`  
每 job：配置 → 编译 → 跑最低套件：

1. asyncsocket smoke（listen / connect / pump）  
2. DAP handshake  
3. DAP evaluate（含至少一条 REPL 写回）  
4. DAP condition breakpoint  

非 5.4：用可 `require("luadap")` / `require("asyncsocket")` 的轻量宿主（不必链 sol2）；复用现有 Python DAP 测试与 Lua smoke，按需加版本参数/路径。

## 非目标与风险

| 项 | 说明 |
|----|------|
| LuaJIT | 明确不做；若日后需要另开设计 |
| 整数语义 | 5.1 无数/整二分；测试避免依赖 5.3+ 整型边角 |
| FetchContent 网络 | CI 需缓存或镜像；文档写明可改用 `LUA_ROOT` |
| sol2 宿主 | 非 5.4 不保证 `main` 可编 |

## 实现顺序（摘要）

1. `lua_compat` + 接入两个 native 目标（5.4 回归绿）  
2. 改写 `asyncsocket` userdata  
3. `ld_set_chunk_env` + evaluate/BP  
4. CMake `LUA_VERSION` / `LUA_ROOT` / FetchContent  
5. 轻量多版本测试驱动 + CI 矩阵  
6. README / 状态更新

## 验收

- 默认（5.4）现有 DAP / asyncsocket 全套回归通过。  
- CI 上 5.1–5.4 各自通过上述最低四项测试。  
- 源码中无未包装的 `lua_newuserdatauv` / `lua_getiuservalue` / `lua_setiuservalue`（`asyncsocket`/`luadap`）。
