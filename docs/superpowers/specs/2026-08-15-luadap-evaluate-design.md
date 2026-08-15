# luadap DAP evaluate 设计

**日期：** 2026-08-15  
**状态：** 已实现  
**范围：** 在 C++ `luadap` 中实现 DAP `evaluate`，支持 Watch / Hover / Debug Console（REPL）；REPL 可多语句并写回 local/upvalue。

**前置：**
- `docs/superpowers/specs/2026-08-15-luadap-cpp-dap-design.md`（已实现；原 `supportsEvaluateForHovers=false`）
- 现有 `lua_debug` 栈帧 / `format_var` / 条件断点求值

## 目标

1. `initialize`：`supportsEvaluateForHovers = true`。  
2. 实现 `evaluate`：`context` = `watch` | `hover` | `repl`（缺省 `watch`）。  
3. 仅在**暂停**时求值；按 `frameId`（缺省 0）绑定当前暂停协程帧。  
4. Watch/Hover：只读 `return (expr)`。  
5. REPL：表达式或语句；赋值经 env `__newindex` 写回 `lua_setlocal` / `lua_setupvalue`；未知名写 `_G`。  
6. 结果复用现有变量展示（含表 `variablesReference`）。  
7. 新增 Python 测试；现有 DAP 回归仍绿。

## 已锁定决策

| 决策 | 选择 |
|------|------|
| UI 覆盖 | Watch + Hover + REPL（方案 1） |
| 求值深度 | 完整 REPL，含写回（方案 3） |
| 实现结构 | 帧环境表 + `__newindex` 写回（方案 A） |
| 未暂停 | `success=false` |
| 多返回值 | V1 **只取第一个返回值** |
| REPL 无返回 | `result = "nil"` |
| `setVariable` 命令 | 非目标（可用 REPL 部分替代） |

## DAP 行为

| 字段 | 规则 |
|------|------|
| `arguments.expression` | 必填字符串 |
| `arguments.frameId` | 可选；默认 0；必须属于当前暂停协程用户帧 |
| `arguments.context` | `watch` / `hover` / `repl`；其它/缺省 → `watch` |
| 成功 body | `result`, `type`, `variablesReference`（非表为 0） |
| 失败 | `success=false`，`message` 含编译/运行错误 |

## 求值语义

### 环境

对暂停 `lua_State*` + `frameId`：

1. `walk_user_frames` → stack `level`  
2. env 表填入 locals（跳过 `(` 前缀名）与 upvalues（local 优先）  
3. `__index = _G`  
4. Watch/Hover：无 `__newindex`（赋值报错）  
5. REPL：`__newindex`：local → `lua_setlocal`；upvalue → `lua_setupvalue`；否则 `rawset(_G, k, v)`

维护侧表：变量名 → `{ kind: local|upvalue, index }` 供写回。

### 加载

| context | 步骤 |
|---------|------|
| watch / hover | `luaL_loadstring` / `lua_load`：`return (expr)`，`pcall` |
| repl | 先试 `return (expr)`；失败则 `load` 原 chunk 作语句；`pcall` |

全程 `pcall`；失败不拆会话、不卸 hook。

### 结果

把栈顶值交给与 `format_var` 一致的逻辑生成 DAP 字段；表分配 `variablesReference`（当前 stop 的 ref 表）。

## 实现落点

| 文件 | 职责 |
|------|------|
| `native/luadap/dap_session.c` | capability；`handle_evaluate` |
| `native/luadap/lua_debug.c/.h` | `lua_debug_evaluate(...)` |
| 可选重构 | 条件断点与 evaluate 共用建 env 辅助 |

对外 Lua API 不变（`start` / `update` / `track`）。

## 测试

新增 `script/test/test_dap_evaluate.py`（debugee 可复用 `run_debugee.lua` 或专用脚本）：

1. 暂停后 watch：`x + y`（或等价）正确  
2. `context=hover` 同样成功  
3. REPL：`x = 99` 后 variables/evaluate 反映写回  
4. 表表达式 `variablesReference > 0`  
5. 未暂停或坏表达式 → 失败响应  

全量既有 DAP 回归仍通过。

## 非目标

- 独立 `setVariable` 请求  
- 未暂停时求值  
- 沙箱隔离 / 超时杀脚本  
- 多返回值完整列表  
- 修改 VS Code 扩展（仍 `debugServer`）

## 风险与缓解

| 风险 | 缓解 |
|------|------|
| 写回 level 算错 | 与 stackTrace/getlocal 共用 walk；单测 REPL 赋值 |
| REPL 无限循环 | V1 不杀；文档提示；后续可加 hook 计数 |
| 与条件断点 env 漂移 | 抽公共 helper |
| 表 ref 与 variables 冲突 | 同一 stop 的 `next_ref` / `var_refs` |

## 验收清单

1. Hover/Watch/REPL 在 VS Code 或 Python 客户端可用  
2. REPL 写回 local 后 Variables 一致  
3. `supportsEvaluateForHovers=true`  
4. 失败路径不破坏会话  
5. 回归套件绿  
